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

static inline void log(const char* format, ...)
{
	static const auto start_ms = get_milliseconds();
	va_list argptr;
	va_start(argptr, format);
	fprintf(stderr, "[%5d %6ld] ", getpid(), get_milliseconds() - start_ms);
	vfprintf(stderr, format, argptr);
	va_end(argptr);
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

class UpstreamLink
{
public:
	zmq::socket_t control_socket, data_socket;

	void connect(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint)
	{
		// Connect to upstream sockets
		control_socket.connect(upstreamControlEndpoint);
		data_socket.connect(upstreamDataEndpoint);
	}

	UpstreamLink(zmq::context_t& ctx)
		: control_socket{ctx, zmq::socket_type::sub}
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
	std::string controlEndpoint;
	std::string dataEndpoint;

	zmq::socket_t control_socket, data_socket;

	DownstreamLink(zmq::context_t& ctx,
		std::string const& controlEndpoint_, std::string const& dataEndpoint_)
		: control_socket{ctx, zmq::socket_type::pub}
		, data_socket{ctx, zmq::socket_type::pull}
	{
		// Bind to specified endpoints
		control_socket.bind(controlEndpoint_);
		data_socket.bind(dataEndpoint_);

		// Set real endpoint values
		controlEndpoint = control_socket.get(zmq::sockopt::last_endpoint);
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

static inline auto recv_message(zmq::socket_t& socket, zmq::recv_flags const sockopt = zmq::recv_flags::none)
{
	while (true) {
		auto received_result = zmq::message_t{};
		auto maybe_bytes = socket.recv(received_result, sockopt);
		if (!maybe_bytes) {
			continue;
		}

		return received_result;
	}
}

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
	static constexpr auto AllRanks = int{0xff};
};

// Subscribe to messages starting with the given tag
static inline void subscribe_to_tag(UpstreamLink& upstream, int tag)
{
	assert((0 < tag) && (tag <= 0xff)); // TODO: pad out to the right

	// ff-pad tag into char buffer
	unsigned char buf[] = {(unsigned char)tag, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, '\0'};

	// Subscribe to padded tag topic
	upstream.control_socket.set(zmq::sockopt::subscribe, (char const*)buf);
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

	ReaderStorage(zmq::message_t&& message_)
		: message{std::move(message_)}
		, tagReaderPair{extractTagReader(message)}
	{}

	auto getTag() const { return tagReaderPair.first; }

	template <typename T>
	auto getRoot() { return tagReaderPair.second.getRoot<T>(); }

	auto& get() { return message; }

	auto eject()
	{
		assert(tagReaderPair.first != 0x00);
		tagReaderPair.first = 0x00;
		return std::move(message);
	}
};

static inline auto recv_MessageReader(zmq::socket_t& socket, zmq::recv_flags const sockopt = zmq::recv_flags::none)
{
	auto message = net::recv_message(socket, sockopt);
	return ReaderStorage{std::move(message)};
}

} // net


// Message collectors / distributors for asynchronous implementations
namespace async
{

// Proxy result messages produced by local stream tasks to upstream data socket
// Returns control of upstream data socket upon completion
static inline zmq::socket_t collect_network_responses(zmq::context_t& ctx, std::string collectorEndpoint, zmq::socket_t&& output_socket)
{
	// Create incoming message socket
	auto collector_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	collector_socket.bind(collectorEndpoint);

	while (true) {
		// Read next incoming local message
		auto reader = net::recv_MessageReader(collector_socket);

		// If exit message, return socket back to caller for cleanup
		if (reader.getRoot<NetworkResponse>().isExit()) {
			return std::move(output_socket);
		}

		// Otherwise, relay application result response upstream
		output_socket.send(reader.eject(), zmq::send_flags::none);
	}
}

// Standard endpoint format for task sockets created by message distributor
static inline std::string task_socket_endpoint(zmqnet::Stream::Id const streamId)
{
	return "inproc://stream-" + std::to_string(streamId);
}

namespace
{

static inline zmq::socket_t make_task_socket(zmq::context_t& ctx, zmqnet::Stream::Id const streamId, zmq::message_t&& stream_message)
{
	// Create task socket and bind to known endpoint format
	auto task_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	auto const taskEndpoint = task_socket_endpoint(streamId);
	log("distributor creating new task socket %s\n", taskEndpoint.c_str());
	task_socket.bind(taskEndpoint);

	// Relay message to new task socket
	task_socket.send(std::move(stream_message), zmq::send_flags::none);

	return std::move(task_socket);
}

} // anonymous namespace

// Select stream task based on incoming message's stream ID, and send message to stream task socket
// Stream tasks are created by main thread.
// Completed when received `num_clients` exit messages.
// Returns control of input socket upon completion.
static constexpr auto BackendClient = int{1}; // Frontends and backends have only one client
static constexpr auto FrontendClient = int{1};
using MaybeSemaphore = std::optional<std::reference_wrapper<net::shared_semaphore>>;
using MaybeLock = std::optional<std::unique_lock<std::mutex>>;
template <typename MessageProto>
static inline zmq::socket_t distribute_messages(zmq::context_t& ctx, zmq::socket_t&& input_socket, int num_clients, MaybeSemaphore maybeStreamSemaphore = std::nullopt)
{
	auto taskSockets = std::unordered_map<zmqnet::Stream::Id, zmq::socket_t>{};

	while (true) {
		// Receive next message
		log("distributor: receiving message...\n");
		auto reader = net::recv_MessageReader(input_socket);
		auto content = reader.getRoot<MessageProto>();

		// If exit, clean up
		if (content.isExit()) {
			log("distributor: exit (%d remaining)\n", num_clients - 1);

			// Relay exit message to task sockets
			auto message = reader.eject();
			for (auto&& [streamId, task_socket] : taskSockets) {
				task_socket.send(message, zmq::send_flags::none);
			}

			// One client has exited
			num_clients--;

			// If all clients have exited, run cleanup
			if (num_clients == 0) {

				// Return control of the upstream control socket back to main thread
				return std::move(input_socket);
			}

			continue;
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
			idSocketPair->second.send(reader.eject(), zmq::send_flags::none);
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
