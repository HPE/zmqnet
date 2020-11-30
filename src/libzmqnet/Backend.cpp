/*
 * Copyright 2020 Hewlett Packard Enterprise Development LP.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

// External interface
#include "zmqnet.hpp"

// Internal helpers and classes
#include "libzmqnet.hpp"

zmqnet::Backend::Backend(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int rank)
	: m_ctx{}
	, m_upstream{std::make_unique<net::UpstreamLink>(m_ctx, upstreamControlEndpoint, upstreamDataEndpoint)}
	, m_rank{rank}
{}

zmqnet::Backend::~Backend()
{
	// Generate destructor for forward-declared net::UpstreamLink
}

void zmqnet::Backend::notifyReady()
{
	// Notify parent commnode that setup is completed
	log("rank backend %d: notifying parent ready from leaf node\n", m_rank);
	auto builder = capnp::MallocMessageBuilder{};
	auto response = builder.initRoot<NetworkResponse>();
	response.setReadyAck();

	// Send acknowledgement
	net::send_MessageBuilder(m_upstream->data_socket, builder);
}

void zmqnet::Backend::sendResult(zmqnet::StreamId const& streamId, char const* result_buffer, size_t result_len)
{
	// Notify parent commnode of result
	auto builder = capnp::MallocMessageBuilder{};
	auto response = builder.initRoot<NetworkResponse>();
	auto userResult = response.initUserResult();
	userResult.setStreamId(streamId);
	if (result_len > 0) {
		userResult.setContent(kj::ArrayPtr<kj::byte const>{(unsigned char const*)result_buffer, result_len});
	}

	// Send result
	net::send_MessageBuilder(m_upstream->data_socket, builder);
}

// Synchronous Backend

zmqnet::SyncBackend::SyncBackend(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int rank)
	: Backend(upstreamControlEndpoint, upstreamDataEndpoint, rank)
{
	// Backend instantiated, notify network ready
	notifyReady();
}

std::tuple<zmqnet::StreamId, zmqnet::ByteArray> zmqnet::SyncBackend::nextCommand()
{
	while (true) {
		// Receive next network command
		log("rank backend %d: waiting for next command...\n", m_rank);
		auto reader = net::recv_MessageReader(m_upstream->control_socket);
		auto command = reader.getRoot<NetworkCommand>();

		// Detect exit case
		if (command.isExit()) {
			log("rank backend %d: got exit\n", m_rank);

			// Send exit acknowledgement to parent
			{ auto builder = capnp::MallocMessageBuilder{};
				auto response = builder.initRoot<NetworkResponse>();
				response.setExitAck();

				net::send_MessageBuilder(m_upstream->data_socket, builder);
			}
			m_upstream->control_socket.close();
			m_upstream->data_socket.close();

			return std::make_tuple(-1, ByteArray{});

		// Acknowledge filter case
		} else if (command.isNewStream()) {
			log("rank backend %d: filter response\n", m_rank);
			auto builder = capnp::MallocMessageBuilder{};
			auto response = builder.initRoot<NetworkResponse>();
			auto streamAck = response.initStreamAck();
			streamAck.setStreamId(command.getNewStream().getNewStreamId());

			net::send_MessageBuilder(m_upstream->data_socket, builder);

			continue;

		} else if (command.isStartLeaves()) {
			log("rank backend %d: unexpected: startLeaves\n", m_rank);
			continue;

		// Extract stream ID and user command
		} else {
			log("rank backend %d: user command\n", m_rank);
			assert(command.isUserCommand());
			auto userCommand = command.getUserCommand();
			return std::make_tuple(userCommand.getStreamId(), kj::heapArray(userCommand.getContent().asBytes()));
		}
	}
}

// Asynchronous Backend

// Static asynchronous backend functions to be run in own threads
namespace async
{

// Proxy result messages produced by local stream tasks to upstream data socket
// Returns control of upstream data socket upon completion
static zmq::socket_t proxy_results_upstream(zmq::context_t& ctx, std::string resultEndpoint, zmq::socket_t&& upstream_data_socket)
{
	// Create result
	auto result_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	result_socket.bind(resultEndpoint);

	while (true) {
		// Read next incoming local message
		auto message = net::recv_message(result_socket);

		// If exit message indicating all streams completed, return socket back to frontend for cleanup
		if (net::make_MessageReader(message).getRoot<NetworkResponse>().isExitAck()) {
			return std::move(upstream_data_socket);
		}

		// Otherwise, relay application result response upstream
		upstream_data_socket.send(std::move(message), zmq::send_flags::none);
	}
}

static bool callback_worker(zmq::context_t& ctx, std::string resultEndpoint, zmqnet::StreamId streamId, zmqnet::AsyncBackend::CallbackFunc&& callbackFunc)
{
	// Open task socket to receive this stream's command from upstream control task
	auto task_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	{ auto const taskEndpoint = "inproc://stream-" + std::to_string(streamId);
		log("backend %d: connecting to task endpoint %s\n", streamId, taskEndpoint.c_str());
		task_socket.connect(taskEndpoint);
	}

	// Open result socket to send packets upstream
	log("backend %d: connecting to upstream proxy %s\n", streamId, resultEndpoint.c_str());
	auto result_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	result_socket.connect(resultEndpoint);

	// Wait for stream create command relayed from main thread and acknowledge to proxy
	log("backend %d: waiting for stream create command\n", streamId);
	{ auto result = net::recv_message(task_socket);
		auto reader = net::make_MessageReader(result);
		auto command = reader.getRoot<NetworkCommand>();
		assert(command.isNewStream() && (command.getNewStream().getNewStreamId() == streamId));
	}

	// Create acknowledgement
	log("backend %d: sending stream ack\n", streamId);
	{ auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		auto streamAck = response.initStreamAck();
		streamAck.setStreamId(streamId);

		// Relay acknowledgement to proxy
		net::send_MessageBuilder(result_socket, builder);
	}

	while (true) {

		// Receive result from task socket
		log("backend %d: waiting for command from task socket\n", streamId);
		auto reader = net::recv_MessageReader(task_socket);
		auto command = reader.getRoot<NetworkCommand>();

		// If exit command, return
		if (command.isExit()) {
			log("backend %d: exiting\n", streamId);
			return true;
		}

		// Command is user command
		assert(!command.isNewStream() && command.isUserCommand());

		// Check stream ID and user result
		assert(command.getUserCommand().getStreamId() == streamId);

		// Run user-provided callback. TODO: pass ArrayPtr instead of new heap array
		auto result = callbackFunc(streamId, kj::heapArray(command.getUserCommand().getContent().asBytes()));

		// Create outgoing result message
		if (result.size() > 0) {
			auto builder = capnp::MallocMessageBuilder{};
			auto response = builder.initRoot<NetworkResponse>();
			auto userResult = response.initUserResult();
			userResult.setStreamId(streamId);
			auto const result_size = result.size();
			userResult.setContent(std::move(result));

			// Send result
			log("backend %d: sending result size %lu\n", streamId, result_size);
			net::send_MessageBuilder(result_socket, builder);
		}
	}
}

static zmq::socket_t callback_distributor(zmq::context_t& ctx, std::string resultEndpoint, zmq::socket_t&& upstream_control_socket, net::shared_semaphore& exiting)
{
	auto callbackSockets = std::unordered_map<zmqnet::StreamId, zmq::socket_t>{};

	// Connect to upstream proxy
	log("backend: connecting to upstream proxy socket %s\n", resultEndpoint.c_str());
	auto result_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	result_socket.connect(resultEndpoint);

	while (true) {
		// Receive next command
		log("backend: receiving command...\n");
		auto result = net::recv_message(upstream_control_socket);
		auto reader = net::make_MessageReader(result);
		auto command = reader.getRoot<NetworkCommand>();

		// If exit, clean up
		if (command.isExit()) {
			log("backend: exit\n");

			// Relay exit message to workers
			for (auto&& [streamId, callback_socket] : callbackSockets) {
				callback_socket.send(result, zmq::send_flags::none);
			}

			// Send exit message to proxy
			{ auto builder = capnp::MallocMessageBuilder{};
				auto response = builder.initRoot<NetworkResponse>();
				response.setExitAck();

				// Send acknowledgement to proxy
				net::send_MessageBuilder(result_socket, builder);
			}

			// Set exiting flag
			{ auto exiting_lock = exiting.acquire_lock();
				assert(exiting.get(exiting_lock) == false);
				exiting.set(exiting_lock, true);
				exiting.unlock_and_notify_one(std::move(exiting_lock));
			}

			break;

		// If new stream, add new stream task socket
		} else if (command.isNewStream()) {
			auto const streamId = command.getNewStream().getNewStreamId();
			log("backend: new stream %d\n", streamId);

			// Get socket for given stream ID
			auto const idSocketPair = callbackSockets.find(streamId);
			assert(idSocketPair == callbackSockets.end());

			// Create task socket
			auto task_socket = zmq::socket_t{ctx, zmq::socket_type::push};
			auto taskEndpoint = "inproc://stream-" + std::to_string(streamId);
			log("backend creating new task socket %s\n", taskEndpoint.c_str());
			task_socket.bind(taskEndpoint);

			// Relay new stream command to new task socket
			task_socket.send(std::move(result), zmq::send_flags::none);

			// Store in stream task socket map
			callbackSockets.emplace_hint(idSocketPair, streamId, std::move(task_socket));

			continue;
		}

		// Response is user command
		assert(command.isUserCommand());

		// Extract stream ID to select stream worker
		auto const streamId = command.getUserCommand().getStreamId();
		log("backend: user command on %d\n", streamId);

		// Get socket for given stream ID
		auto const idSocketPair = callbackSockets.find(streamId);
		assert(idSocketPair != callbackSockets.end());

		// Relay message to socket worker
		idSocketPair->second.send(std::move(result), zmq::send_flags::none);
	}

	// Return control of the upstream control socket back to main thread
	return std::move(upstream_control_socket);
}

} // async

zmqnet::AsyncBackend::AsyncBackend(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int rank)
	: Backend{upstreamControlEndpoint, upstreamDataEndpoint, rank}
	, m_ctx{}
	, m_callbackWorkers{}
	, m_exiting{std::make_unique<net::shared_semaphore>(false)}
	, m_upstreamProxyEndpoint{"inproc://" + net::generate_uuid()}
	, m_upstreamProxySocket{m_ctx, zmq::socket_type::push}
	, m_upstreamProxy{}
	, m_callbackDistributor{}
{
	// Launch result proxy and move control of upstream data socket
	m_upstreamProxy = std::async(std::launch::async,
		[](zmq::context_t& ctx, std::string resultEndpoint, zmq::socket_t&& upstream_data_socket) {
			return async::proxy_results_upstream(ctx, std::move(resultEndpoint), std::move(upstream_data_socket));
		}, std::ref(m_ctx), m_upstreamProxyEndpoint, std::move(m_upstream->data_socket));

	m_upstreamProxySocket.connect(m_upstreamProxyEndpoint);

	// Launch callback distributor task
	m_callbackDistributor = std::async(std::launch::async,
		[](zmq::context_t& ctx, std::string resultEndpoint, zmq::socket_t&& upstream_control_socket, net::shared_semaphore& exiting) {
			return async::callback_distributor(ctx, std::move(resultEndpoint), std::move(upstream_control_socket), exiting);
		}, std::ref(m_ctx), m_upstreamProxyEndpoint, std::move(m_upstream->control_socket), std::ref(*m_exiting));
}

zmqnet::AsyncBackend::~AsyncBackend()
{
	// Generate destructor for forward-declared net::shared_semaphore
}

void zmqnet::AsyncBackend::registerCallback(StreamId const& streamId, zmqnet::AsyncBackend::CallbackFunc callbackFunc)
{
	// Ensure stream not registered
	if (m_callbackWorkers.find(streamId) != m_callbackWorkers.end()) {
		throw std::runtime_error("asynchronous callback already registered for stream ID " + std::to_string(streamId));
	}

	// Launch worker thread
	m_callbackWorkers.emplace(streamId, std::async(std::launch::async,
		[](zmq::context_t& ctx, std::string resultEndpoint, zmqnet::StreamId streamId, CallbackFunc&& callbackFunc) {
			return async::callback_worker(ctx, std::move(resultEndpoint), streamId, std::move(callbackFunc));
		}, std::ref(m_ctx), m_upstreamProxyEndpoint, streamId, std::move(callbackFunc)));
}

void zmqnet::AsyncBackend::notifyReady()
{
	// Notify parent commnode that setup is completed
	log("rank backend %d: notifying parent ready from leaf node\n", m_rank);
	auto builder = capnp::MallocMessageBuilder{};
	auto response = builder.initRoot<NetworkResponse>();
	response.setReadyAck();

	// Send ready acknowledgement
	net::send_MessageBuilder(m_upstreamProxySocket, builder);
}

void zmqnet::AsyncBackend::waitForExit()
{
	// Wait for exiting flag
	{ auto exiting_lock = m_exiting->acquire_lock();
		m_exiting->unlock_and_wait_until(std::move(exiting_lock), true);
	}

	// Wait for stream workers
	log("rank backend %d: waiting for stream workers\n", m_rank);
	for (auto&& [streamId, callbackWorker] : m_callbackWorkers) {
		(void)callbackWorker.get();
	}

	// Reacquire upstream data socket from exiting proxy
	log("rank backend %d: waiting for proxy\n", m_rank);
	m_upstream->data_socket = m_upstreamProxy.get();
	m_upstreamProxySocket.close();

	// Reacquire upstream data socket from exiting callback processor
	log("rank backend %d: waiting for distributor\n", m_rank);
	m_upstream->control_socket = m_callbackDistributor.get();

	// Send exit acknowledgement
	{ log("rank backend %d: notifying parent leaf node exiting\n", m_rank);
		auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		response.setExitAck();

		// Send acknowledgement to proxy
		net::send_MessageBuilder(m_upstream->data_socket, builder);
	}
}
