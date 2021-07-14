/*
 * Copyright 2020 Hewlett Packard Enterprise Development LP.
 */

#pragma once

#include <stdarg.h>
#include <unistd.h>
#include <wait.h>

#include <uuid/uuid.h>

#include <vector>
#include <string>
#include <tuple>
#include <optional>

// Logging

#include <sys/time.h>

// ZMQ and network

#include <zmq.hpp>

// Capnp and application protocol

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include "libzmqnet/NetworkSchema.capnp.h"

// Logging

static inline auto get_milliseconds()
{
	struct timeval tv{};
	::gettimeofday(&tv, nullptr);
	return (tv.tv_sec) * 1000 + (tv.tv_usec) / 1000;
}

static inline bool debug_enabled()
{
	static const auto _enabled = []() {
		if (::getenv("CTI_DEBUG")) {
			return true;
		}

		return false;
	}();
	return _enabled;
}

static inline void log(const char* format, ...)
{
	if (debug_enabled()) {
		static const auto start_ms = get_milliseconds();
		va_list argptr;
		va_start(argptr, format);
		fprintf(stderr, "[%5d %6ld] ", getpid(), get_milliseconds() - start_ms);
		vfprintf(stderr, format, argptr);
		va_end(argptr);
	}
}

static inline void enable_individual_logfile(std::string filename = "") 
{
	if (!debug_enabled()) return;

	if (filename == "") filename = "zmqnet_" + std::to_string(getpid()) + ".log";

	const char *dir = getenv("CRAY_DBG_LOG_DIR");
    if (!dir) {
        dir = "/tmp";
    }

	auto fullpath = std::string(dir) + "/" + filename;

	auto file = fopen(fullpath.c_str(), "w");
	if (!file) {
		log("Failed to open %s for writing.\n", fullpath.c_str());
		return;
	}

	if (dup2(fileno(file), STDERR_FILENO) < 0) {
		log("(%s) dup2 failed.\n", fullpath.c_str());
		return;
	}
}

namespace net
{

// Synchronization

// Cleaner to pass this object instead of three from main thread to std::async launched tasks
struct shared_semaphore
{
	std::mutex m_mutex;
	std::condition_variable m_cv;
	std::atomic<bool> m_flag;

	shared_semaphore(bool flag) : m_mutex{}, m_cv{}, m_flag{flag} {}

	auto acquire_lock() { return std::unique_lock<std::mutex>{m_mutex}; }
	bool get(std::unique_lock<std::mutex> const&) { return m_flag.load(); }
	void set(std::unique_lock<std::mutex> const&, bool flag) { m_flag = flag; }
	void unlock_and_notify_one(std::unique_lock<std::mutex>&& expiring_lock) { m_cv.notify_one(); }
	void unlock_and_wait_until(std::unique_lock<std::mutex>&& lock, bool target) {
		m_cv.wait(lock, [this, target=target] { return (m_flag.load() == target); });
	}
};

// ZMQ and network

static inline std::string generate_uuid()
{
	uuid_t id;
	uuid_generate(id);

	char uuid_str[37];
	uuid_unparse(id, uuid_str);

	return std::string{uuid_str};
}

class PubSubLoggingProxy
{
public:
	bool capture_enabled;
	std::string proxyCaptureEndpoint;
	zmq::socket_t proxy_incoming, proxy_outgoing, proxy_capture;
	std::thread proxyTask, captureTask;

	PubSubLoggingProxy(zmq::context_t& ctx, std::string const& logLinePrefix)
		: capture_enabled{debug_enabled()}
		, proxyCaptureEndpoint{"inproc://" + generate_uuid()}
		, proxy_incoming{ctx, zmq::socket_type::xsub}
		, proxy_outgoing{ctx, zmq::socket_type::xpub}
		, proxy_capture{ctx, zmq::socket_type::pair}
		, proxyTask{}
		, captureTask{}
	{
		if (!capture_enabled) {
			return;
		}

		// Bind to first end of capture socket
		proxy_capture.bind(proxyCaptureEndpoint);

		// Start capture task
		captureTask = std::thread{[&](std::string logLinePrefix) {

			// Connect to second end of capture socket
			auto socket = zmq::socket_t{ctx, zmq::socket_type::pair};
			socket.connect(proxyCaptureEndpoint);

			// Receive until socket closed or context terminated
			while (true) {
				auto received_result = zmq::message_t{};
				try {

					// Ignore interrupted receive
					if (!socket.recv(received_result, zmq::recv_flags::none)) {
						continue;
					}

				// If error was not caused by termination, rethrow
				} catch (zmq::error_t const& err) {
					if (err.num() == ETERM) {
						return;
					}
					throw;
				}

				// Log incoming message
				log("%s: %s\n", logLinePrefix.c_str(), received_result.str().c_str());
			}
		}, logLinePrefix};

		// Manually tell proxy socket to subscribe to all incoming messages
		{ unsigned char const buf[] = {0x01};
			proxy_incoming.send(zmq::const_buffer{buf, 1}, zmq::send_flags::none);
		}

		// Start proxy task
		proxyTask = std::thread{[&]() {

			// Proxy until socket closed or context terminated
			try {
				(void)zmq::proxy(zmq::socket_ref{proxy_incoming}, proxy_outgoing, proxy_capture);

			// If error was not caused by termination, rethrow
			} catch (zmq::error_t const& err) {
				if (err.num() == ETERM) {
					return;
				}
				throw;
			}
		}};

	}

	// Log messages coming from upstream into control socket
	void logIncoming(zmq::socket_t& control_socket, std::string const& controlEndpoint)
	{
		if (capture_enabled) {
			auto const proxyOutgoingEndpoint = std::string{"inproc://" + generate_uuid()};

			// Messages arrive from upstream control endpoint and flow into proxy
			proxy_incoming.connect(controlEndpoint);

			// Messages leave proxy and are sent to downstream control socket
			proxy_outgoing.bind(proxyOutgoingEndpoint);
			control_socket.connect(proxyOutgoingEndpoint);

		// Capture disabled, connect directly
		} else {
			control_socket.connect(controlEndpoint);
		}
	}

	// Log messages originating from control socket downstream
	// Return real control endpoint
	std::string logOutgoing(zmq::socket_t& control_socket, std::string const& controlEndpoint)
	{
		if (capture_enabled) {
			auto const proxyIncomingEndpoint = std::string{"inproc://" + generate_uuid()};

			// Messages are sent to downstream control endpoint and flow into proxy
			control_socket.bind(proxyIncomingEndpoint);
			proxy_incoming.connect(proxyIncomingEndpoint);

			// Messages leave proxy and are sent to downstream control endpoint
			proxy_outgoing.bind(controlEndpoint);

			return proxy_outgoing.get(zmq::sockopt::last_endpoint);

		// Capture disabled, bind directly
		} else {
			control_socket.bind(controlEndpoint);
			return control_socket.get(zmq::sockopt::last_endpoint);
		}
	}

	~PubSubLoggingProxy()
	{
		// Join proxy and capture threads
		if (capture_enabled) {
			captureTask.join();
			proxyTask.join();
		}
	}
};

class UpstreamLink
{
public:
	PubSubLoggingProxy loggingProxy;
	zmq::socket_t control_socket, data_socket;

	void connect(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint)
	{
		// Connect to upstream sockets
		loggingProxy.logIncoming(control_socket, upstreamControlEndpoint);
		data_socket.connect(upstreamDataEndpoint);
	}

	UpstreamLink(zmq::context_t& ctx)
		: loggingProxy{ctx, "receiving"}
		, control_socket{ctx, zmq::socket_type::sub}
		, data_socket{ctx, zmq::socket_type::push}
	{}

	UpstreamLink(zmq::context_t& ctx,
		std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint)
		: UpstreamLink{ctx}
	{
		connect(upstreamControlEndpoint, upstreamDataEndpoint);
	}
};

class DownstreamLink
{
public:
	PubSubLoggingProxy loggingProxy;

	std::string controlEndpoint, dataEndpoint;
	zmq::socket_t control_socket, data_socket;

	DownstreamLink(zmq::context_t& ctx,
		std::string const& controlEndpoint_, std::string const& dataEndpoint_)
		: loggingProxy{ctx, "sending"}
		, control_socket{ctx, zmq::socket_type::pub}
		, data_socket{ctx, zmq::socket_type::pull}
	{
		// Bind to downstream endpoints and set real endpoint values
		controlEndpoint = loggingProxy.logOutgoing(control_socket, controlEndpoint_);
		data_socket.bind(dataEndpoint_);
		dataEndpoint = data_socket.get(zmq::sockopt::last_endpoint);
	}
};

class InprocDownstreamLink : public DownstreamLink
{
public:
	InprocDownstreamLink(zmq::context_t& ctx)
		: DownstreamLink
			{ ctx
			, "inproc://" + generate_uuid() // control endpoint
			, "inproc://" + generate_uuid() // data endpoint
		}
	{}
};

class IpcDownstreamLink : public DownstreamLink
{
public:
	IpcDownstreamLink(zmq::context_t& ctx)
		: DownstreamLink
			{ ctx
			, "ipc://ipc-" + generate_uuid() // control endpoint
			, "ipc://ipc-" + generate_uuid() // data endpoint
		}
	{}
};

class TcpDownstreamLink : public DownstreamLink
{
public:
	TcpDownstreamLink(zmq::context_t& ctx)
		: DownstreamLink
			{ ctx
			, "tcp://*:*" // control endpoint
			, "tcp://*:*" // data endpoint
		}
	{}
};

static inline auto replace_endpoint_host(std::string endpoint, std::string const& hostname)
{
	// Extract port from generated endpoint
	endpoint = endpoint.substr(endpoint.find("://") + 3);
	endpoint = endpoint.substr(endpoint.find(":") + 1);

	return "tcp://" + hostname + ":" + std::move(endpoint);
};

// Replace the sockets' endpoints with e.g. known-accessible hostname from CTI
static inline auto formatEndpoints(TcpDownstreamLink const& downstream, std::string const& hostname)
{
	return std::make_tuple(
		replace_endpoint_host(downstream.controlEndpoint, hostname),
		replace_endpoint_host(downstream.dataEndpoint,    hostname)
	);
}

// Capnp and application protocol

namespace communicator_ids
{
	static constexpr auto AllNodes = int{0xff};
};

// Subscribe to messages starting with the given tag
static inline void subscribe_to_tag(zmq::socket_t& socket, int tag)
{
	assert((0 < tag) && (tag <= 0xff)); // TODO: pad out to the right

	// Subscribe to tag topic
	unsigned char buf[] = {(unsigned char)tag, '\0'};
	socket.set(zmq::sockopt::subscribe, (char const*)buf);
}

static inline auto builderToTaggedMessage(int tag, capnp::MessageBuilder& builder)
{
	auto const& segments = builder.getSegmentsForOutput();

	auto result = kj::heapArray<capnp::word>(1 + capnp::computeSerializedSizeInWords(segments));

	// ff-pad tag to word length
	{ assert((0 < tag) && (tag <= 0xff));
		unsigned char buf[] = {(unsigned char)tag, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
		static_assert(sizeof(buf) == sizeof(capnp::word));

		// Write padded tag to beginning of message
		result[0] = *(capnp::word*)buf;
	}

	// Pack rest of message using builder's segments (see capnp::messageToFlatArray)
	auto table = reinterpret_cast<capnp::_::WireValue<uint32_t>*>(result.begin() + 1);

	table[0].set(segments.size() - 1);

	for (uint i = 0; i < segments.size(); i++) {
		table[i + 1].set(segments[i].size());
	}

	if (segments.size() % 2 == 0) {
		// Set padding byte.
		table[segments.size() + 1].set(0);
	}

	auto dst = (result.begin() + 1) + segments.size() / 2 + 1;

	for (auto& segment: segments) {
		memcpy(dst, segment.begin(), segment.size() * sizeof(capnp::word));
		dst += segment.size();
	}

	KJ_DASSERT(dst == result.end(), "Buffer overrun/underrun bug in code above.");

	return kj::mv(result);
}

static inline void send_MessageBuilder(zmq::socket_t& socket, zmqnet::Communicator::Id communicatorId, capnp::MessageBuilder& builder, zmq::send_flags const sockopt = zmq::send_flags::none)
{
	auto messageArray = builderToTaggedMessage(communicatorId, builder);
	auto const messageBytes = messageArray.asBytes();
	socket.send(zmq::message_t{messageBytes.begin(), messageBytes.size()}, sockopt);
}

// Store required message / Reader state in custom object to avoid dropped backing storage
// Note: Capnp Reader arenas apparently do not move correctly
class ReaderStorage
{
public:
	zmq::message_t message;
	std::pair<int, capnp::UnalignedFlatArrayMessageReader> tagReaderPair;

	static inline auto extractTagReader(zmq::message_t& message)
	{
		assert(message.size() % sizeof(capnp::word) == 0);

		// Extract tag from first word of message
		auto const tag = (int)message.data<unsigned char>()[0];
		assert((0 < tag) && (tag <= 0xff));

		// Rest of message contains capnp words
		auto const messageWords = message.data<capnp::word>() + 1;
		auto const messageWordsLen = (message.size() / sizeof(capnp::word)) - 1;

		auto messageArray = kj::arrayPtr(messageWords, messageWordsLen);
		return std::make_pair(tag, capnp::UnalignedFlatArrayMessageReader(messageArray));
	}

	ReaderStorage(ReaderStorage&& expiring)
		: message{std::move(expiring.message)}
		, tagReaderPair{extractTagReader(message)}
	{
		expiring.tagReaderPair.first = 0x00;
	}

	ReaderStorage(zmq::message_t&& message_)
		: message{std::move(message_)}
		, tagReaderPair{extractTagReader(message)}
	{}

	auto getTag() const { return tagReaderPair.first; }

	template <typename T>
	auto getRoot() { return tagReaderPair.second.getRoot<T>(); }

	auto eject()
	{
		assert(tagReaderPair.first != 0x00);
		tagReaderPair.first = 0x00;
		return std::move(message);
	}
};

static inline void send_MessageReader_copy(zmq::socket_t& socket, ReaderStorage& reader, zmq::send_flags const sockopt = zmq::send_flags::none)
{
	// Messages can't be reused after sending
	auto message = zmq::message_t{};
	message.copy(reader.message);
	socket.send(std::move(message), sockopt);
}

static inline void send_MessageReader(zmq::socket_t& socket, ReaderStorage&& expiring, zmq::send_flags const sockopt = zmq::send_flags::none)
{
	socket.send(expiring.eject(), sockopt);
}

static inline auto recv_MessageReader(zmq::socket_t& socket, zmq::recv_flags const sockopt = zmq::recv_flags::none)
{
	while (true) {
		auto received_result = zmq::message_t{};
		if (socket.recv(received_result, sockopt)) {
			return ReaderStorage{std::move(received_result)};
		}
	}
}

static inline auto recv_MessageReader_timeout(zmq::socket_t& socket, zmq::recv_flags const sockopt = zmq::recv_flags::none)
{
	auto received_result = zmq::message_t{};

	// Nonzero return value indicates successful result
	if (socket.recv(received_result, sockopt)) {
		return std::make_optional(ReaderStorage{std::move(received_result)});

	// Zero value indicates timeout
	} else {
		return std::optional<ReaderStorage>{};
	}
}

} // net


// Message collectors / distributors for asynchronous implementations
namespace async
{

// Proxy result messages produced by local stream tasks to upstream data socket
// Returns control of upstream data socket upon completion
static inline zmq::socket_t collect_network_responses(zmq::context_t& ctx, std::string resultEndpoint, zmq::socket_t&& output_socket)
{
	// Create incoming message socket
	auto result_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	result_socket.bind(resultEndpoint);

	while (true) {
		// Read next incoming local message
		auto reader = net::recv_MessageReader(result_socket);
		auto response = reader.getRoot<NetworkResponse>();

		// If exit message, return socket back to caller for cleanup
		if (response.isExit()) {
			return std::move(output_socket);
		}

		// Otherwise, relay application result response upstream
		net::send_MessageReader(output_socket, std::move(reader));
	}
}

// Standard endpoint format for task sockets created by message distributors
static inline std::string stream_endpoint(zmqnet::Stream const& stream)
{
	return "inproc://stream-" + std::to_string(stream.communicatorId) + "-" + std::to_string(stream.id);
}

static inline zmq::socket_t make_task_socket(zmq::context_t& ctx, zmqnet::Stream const& stream, net::ReaderStorage&& expiring)
{
	// Create task socket and bind to known endpoint format
	auto task_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	auto const taskEndpoint = stream_endpoint(stream);
	task_socket.connect(taskEndpoint);

	// Relay message to new task socket
	net::send_MessageReader(task_socket, std::move(expiring));

	return std::move(task_socket);
}

// Select stream task based on incoming message's stream ID, and send message to stream task socket
// Stream tasks are created by main thread.
// Completed when received an exit message.
// Returns control of input socket upon completion.
using MaybeSemaphore = std::optional<std::reference_wrapper<net::shared_semaphore>>;
using MaybeLock = std::optional<std::unique_lock<std::mutex>>;
template <typename MessageProto>
static inline zmq::socket_t distribute_messages(zmq::context_t& ctx, zmq::socket_t&& input_socket, MaybeSemaphore maybeStreamSemaphore = std::nullopt)
{
	auto taskSockets = std::unordered_map<zmqnet::Stream::Id, zmq::socket_t>{};

	while (true) {
		// Receive next message
		log("distributor: receiving message...\n");
		auto reader = net::recv_MessageReader(input_socket);
		auto content = reader.getRoot<MessageProto>();

		// If exit, clean up
		if (content.isExit()) {
			log("distributor: exit\n");

			// Relay exit message to task sockets
			for (auto&& [streamId, task_socket] : taskSockets) {
				net::send_MessageReader(task_socket, reader);
			}

			// Return control of the upstream control socket back to main thread
			return std::move(input_socket);
		}

		// Content is either stream or user content
		assert(content.isStream() || content.isUser());

		// Extract stream ID to select stream worker
		auto const streamId = (content.isStream())
			? content.getStream().getStreamId()
			: content.getUser().getStreamId();

		// Get socket for given stream ID
		auto const idSocketPair = taskSockets.find(streamId);

		// Create new stream task connection with known endpoint format, if needed
		if (idSocketPair == taskSockets.end()) {
			log("distributor: new stream %d\n", streamId);
			assert(content.isStream());

			// Lock stream registration flag shared with main thread, if provided
			auto maybeStreamLock = (maybeStreamSemaphore)
				? MaybeLock{maybeStreamSemaphore->get().acquire_lock()}
				: std::nullopt;
			assert(!maybeStreamLock || (maybeStreamSemaphore->get().get(*maybeStreamLock) == true));

			// Create task socket and store in stream task socket map
			taskSockets.emplace_hint(idSocketPair, streamId,
				make_task_socket(ctx, streamId, reader.eject()));

		// Relay message to task socket
		} else {
			log("distributor: relay message to stream %d\n", streamId);
			net::send_MessageReader(idSocketPair->second, std::move(reader));
		}

		// Stream messages should notify main thread, if raised
		if (content.isStream() && maybeStreamSemaphore) {
			auto stream_lock = maybeStreamSemaphore->get().acquire_lock();

			log("distributor: notifying main thread that stream registration has completed\n");
			if (maybeStreamSemaphore->get().get(stream_lock) == true) {
				maybeStreamSemaphore->get().set(stream_lock, false);
				maybeStreamSemaphore->get().unlock_and_notify_one(std::move(stream_lock));
			}
		}
	}
}

} // async
