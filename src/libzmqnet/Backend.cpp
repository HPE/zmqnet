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
	log("rank %d: notifying parent ready from leaf node\n", m_rank);
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
		log("rank %d: waiting for next command...\n", m_rank);
		auto reader = net::recv_MessageReader(m_upstream->control_socket);
		auto command = reader.getRoot<NetworkCommand>();

		// Detect exit case
		if (command.isExit()) {
			log("rank %d: got exit\n", m_rank);

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
			log("rank %d: filter response\n", m_rank);
			auto builder = capnp::MallocMessageBuilder{};
			auto response = builder.initRoot<NetworkResponse>();
			auto streamAck = response.initStreamAck();
			streamAck.setStreamId(command.getNewStream().getNewStreamId());

			net::send_MessageBuilder(m_upstream->data_socket, builder);

			continue;

		} else if (command.isStartLeaves()) {
			log("rank %d: unexpected: startLeaves\n", m_rank);
			continue;

		// Extract stream ID and user command
		} else {
			log("rank %d: user command\n", m_rank);
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

static bool callback_worker(zmq::context_t& ctx, zmqnet::StreamId streamId, zmqnet::CallbackFunc&& callbackFunc)
{
	// Open task socket to receive this stream's command from upstream control task
	auto callback_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	{ auto const callbackEndpoint = "inproc://stream-" + std::to_string(streamId);
		log("%d: connecting to callback endpoint %s\n", streamId, callbackEndpoint.c_str());
		callback_socket.connect(callbackEndpoint);
	}

	while (true) {

		// Receive result from task socket
		log("%d: waiting for result from callback socket\n", streamId);
		auto reader = net::recv_MessageReader(callback_socket);
		auto command = reader.getRoot<NetworkCommand>();

		// If exit command, return
		if (command.isExit()) {
			return true;
		}

		// Check stream ID and user result
		if (command.isUserCommand()) {
			assert(command.getUserCommand().getStreamId() == streamId);

			// Run user-provided callback. TODO: pass ArrayPtr instead of new heap array
			callbackFunc(streamId, kj::heapArray(command.getUserCommand().getContent().asBytes()));

		// Relay stream acknowledgement
		} else {
			
		}
	}
}

static zmq::socket_t callback_distributor(zmq::context_t& ctx, zmq::socket_t&& upstream_control_socket, net::shared_semaphore& exiting)
{
	auto callbackSockets = std::unordered_map<zmqnet::StreamId, zmq::socket_t>{};

	while (true) {
		// Receive next command
		log("dist: receiving command...\n");
		auto result = net::recv_message(upstream_control_socket);
		auto reader = net::make_MessageReader(result);
		auto command = reader.getRoot<NetworkCommand>();

		// If exit, clean up
		if (command.isExit()) {
			log("dist: exit\n");

			// Relay exit message
			for (auto&& [streamId, callback_socket] : callbackSockets) {
				callback_socket.send(result, zmq::send_flags::none);
			}

			// Set exiting flag
			{ auto exiting_lock = exiting.acquire_lock();
				assert(exiting.get(exiting_lock) == false);
				exiting.set(exiting_lock, true);
				exiting.unlock_and_notify_one(std::move(exiting_lock));
			}

			break;
		}

		// Response is either stream registration or user command
		assert(command.isNewStream() || command.isUserCommand());

		// Extract stream ID to select stream worker
		auto const streamId = (command.isNewStream())
			? command.getNewStream().getNewStreamId()
			: command.getUserCommand().getStreamId();


		// Find callback socket corresponding to this stream
		auto streamIdSocketPair = callbackSockets.find(streamId);
		if (streamIdSocketPair == callbackSockets.end()) {
			throw std::runtime_error("no callback socket registered for stream ID " + std::to_string(streamId));
		}

		// Relay stream registration or user command
		streamIdSocketPair->second.send(result, zmq::send_flags::none);
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
	, m_callbackDistributor{}
{
	// Launch callback distributor task
	m_callbackDistributor = std::async(std::launch::async,
		[](zmq::context_t& ctx, zmq::socket_t&& upstream_control_socket, net::shared_semaphore& exiting) {
			return async::callback_distributor(ctx, std::move(upstream_control_socket), exiting);
		}, std::ref(m_ctx), std::move(m_upstream->control_socket), std::ref(*m_exiting));
}

zmqnet::AsyncBackend::~AsyncBackend()
{
	// Generate destructor for forward-declared net::shared_semaphore
}

void zmqnet::AsyncBackend::registerCallback(StreamId const& streamId, CallbackFunc callbackFunc)
{
	// Ensure stream not registered
	if (m_callbackWorkers.find(streamId) != m_callbackWorkers.end()) {
		throw std::runtime_error("asynchronous callback already registered for stream ID " + std::to_string(streamId));
	}

	// Launch worker thread
	m_callbackWorkers.emplace(streamId, std::async(std::launch::async,
		[](zmq::context_t& ctx, zmqnet::StreamId streamId, zmqnet::CallbackFunc&& callbackFunc) {
			return async::callback_worker(ctx, streamId, std::move(callbackFunc));
		}, std::ref(m_ctx), streamId, std::move(callbackFunc)));
}

void zmqnet::AsyncBackend::waitForExit()
{

	// Wait for exiting flag
	{ auto exiting_lock = m_exiting->acquire_lock();
		m_exiting->unlock_and_wait_until(std::move(exiting_lock), true);
	}

	// Wait for stream workers
	for (auto&& [streamId, callbackWorker] : m_callbackWorkers) {
		(void)callbackWorker.get();
	}

	// Reacquire downstream data socket from exiting callback processor
	m_upstream->control_socket = m_callbackDistributor.get();
}
