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
	response.setReady();

	// Send acknowledgement
	net::send_MessageBuilder(m_upstream->data_socket, builder);
}

void zmqnet::Backend::sendResult(zmqnet::Stream::Id const& streamId, char const* result_buffer, size_t result_len)
{
	// Notify parent commnode of result
	auto builder = capnp::MallocMessageBuilder{};
	auto response = builder.initRoot<NetworkResponse>();
	auto user = response.initUser();
	user.setStreamId(streamId);
	if (result_len > 0) {
		user.setContent(kj::ArrayPtr<kj::byte const>{(unsigned char const*)result_buffer, result_len});
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

std::tuple<zmqnet::Stream::Id, zmqnet::ByteArray> zmqnet::SyncBackend::nextCommand()
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
				response.setExit();

				net::send_MessageBuilder(m_upstream->data_socket, builder);
			}
			m_upstream->control_socket.close();
			m_upstream->data_socket.close();

			return std::make_tuple(-1, ByteArray{});

		// Acknowledge filter case
		} else if (command.isStream()) {
			log("rank backend %d: filter response\n", m_rank);
			auto builder = capnp::MallocMessageBuilder{};
			auto response = builder.initRoot<NetworkResponse>();
			auto streamAck = response.initStream();
			streamAck.setStreamId(command.getStream().getStreamId());

			net::send_MessageBuilder(m_upstream->data_socket, builder);

			continue;

		} else if (command.isStartLeaves()) {
			log("rank backend %d: unexpected: startLeaves\n", m_rank);
			continue;

		// Extract stream ID and user command
		} else {
			log("rank backend %d: user command\n", m_rank);
			assert(command.isUser());
			auto userCommand = command.getUser();
			return std::make_tuple(userCommand.getStreamId(), kj::heapArray(userCommand.getContent().asBytes()));
		}
	}
}

// Asynchronous Backend

// Static asynchronous backend functions to be run in own threads
namespace workers
{

static bool command_callback(zmq::context_t& ctx, std::string resultEndpoint, zmqnet::Stream::Id streamId, zmqnet::AsyncBackend::CallbackFunc&& callbackFunc)
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
		assert(command.isStream() && (command.getStream().getStreamId() == streamId));
	}

	// Create acknowledgement
	log("backend %d: sending stream ack\n", streamId);
	{ auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		auto streamAck = response.initStream();
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
		assert(!command.isStream() && command.isUser());

		// Check stream ID and user result
		assert(command.getUser().getStreamId() == streamId);

		// Run user-provided callback. TODO: pass ArrayPtr instead of new heap array
		auto result = callbackFunc(streamId, kj::heapArray(command.getUser().getContent().asBytes()));

		// Create outgoing result message
		if (result.size() > 0) {
			auto builder = capnp::MallocMessageBuilder{};
			auto response = builder.initRoot<NetworkResponse>();
			auto user = response.initUser();
			user.setStreamId(streamId);
			auto const result_size = result.size();
			user.setContent(std::move(result));

			// Send result
			log("backend %d: sending result size %lu\n", streamId, result_size);
			net::send_MessageBuilder(result_socket, builder);
		}
	}
}

} // async

zmqnet::AsyncBackend::AsyncBackend(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int rank)
	: Backend{upstreamControlEndpoint, upstreamDataEndpoint, rank}
	, m_ctx{}

	, m_collectorEndpoint{"inproc://" + net::generate_uuid()}
	, m_collectorSocket{m_ctx, zmq::socket_type::push}
	, m_responseCollector{}

	, m_commandWorkers{}
	, m_commandDistributor{}
{
	// Launch response collector, moving control of upstream data socket
	m_responseCollector = std::async(std::launch::async,
		[](zmq::context_t& ctx, std::string collectorEndpoint, zmq::socket_t&& output_socket) {
			return async::collect_network_responses(ctx, std::move(collectorEndpoint), std::move(output_socket));
		}, std::ref(m_ctx), m_collectorEndpoint, std::move(m_upstream->data_socket));

	// Connect for message injection by main thread
	m_collectorSocket.connect(m_collectorEndpoint);

	// Launch response distributor task, moving control of upstream control socket
	m_commandDistributor = std::async(std::launch::async,
		[](zmq::context_t& ctx, zmq::socket_t&& input_socket) {
			return async::distribute_messages<NetworkCommand>(ctx, std::move(input_socket), async::BackendClient);
		}, std::ref(m_ctx), std::move(m_upstream->control_socket));
}

zmqnet::AsyncBackend::~AsyncBackend()
{
	// Generate destructor for forward-declared net::shared_semaphore
}

void zmqnet::AsyncBackend::registerCallback(zmqnet::Stream::Id const& streamId, zmqnet::AsyncBackend::CallbackFunc callbackFunc)
{
	// Ensure stream not registered
	if (m_commandWorkers.find(streamId) != m_commandWorkers.end()) {
		throw std::runtime_error("asynchronous callback already registered for stream ID " + std::to_string(streamId));
	}

	// Launch worker thread
	m_commandWorkers.emplace(streamId, std::async(std::launch::async,
		[](zmq::context_t& ctx, std::string collectorEndpoint, zmqnet::Stream::Id streamId, CallbackFunc&& callbackFunc) {
			return workers::command_callback(ctx, std::move(collectorEndpoint), streamId, std::move(callbackFunc));
		}, std::ref(m_ctx), m_collectorEndpoint, streamId, std::move(callbackFunc)));
}

void zmqnet::AsyncBackend::notifyReady()
{
	// Notify parent commnode that setup is completed
	log("rank backend %d: notifying parent ready from leaf node\n", m_rank);
	auto builder = capnp::MallocMessageBuilder{};
	auto response = builder.initRoot<NetworkResponse>();
	response.setReady();

	// Send ready acknowledgement
	net::send_MessageBuilder(m_collectorSocket, builder);
}

void zmqnet::AsyncBackend::waitForExit()
{
	// Wait for distributor to exit after receiving network exit command, reacquiring upstream data socket
	log("rank backend %d: waiting for distributor\n", m_rank);
	m_upstream->control_socket = m_commandDistributor.get();

	// Wait for stream workers
	log("rank backend %d: waiting for stream workers\n", m_rank);
	for (auto&& [streamId, command_worker] : m_commandWorkers) {
		(void)command_worker.get();
	}

	// Construct exit message for proxy
	{ auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		response.setExit();

		// Send exit message to proxy
		net::send_MessageBuilder(m_collectorSocket, builder);
	}

	// Reacquire upstream data socket from exiting proxy
	log("rank backend %d: waiting for proxy\n", m_rank);
	m_upstream->data_socket = m_responseCollector.get();
	m_collectorSocket.close();

	// Send exit acknowledgement
	{ log("rank backend %d: notifying parent leaf node exiting\n", m_rank);
		auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		response.setExit();

		// Send acknowledgement to proxy
		net::send_MessageBuilder(m_upstream->data_socket, builder);
	}
}
