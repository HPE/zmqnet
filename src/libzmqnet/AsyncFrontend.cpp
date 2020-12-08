/*
 * Copyright 2020 Hewlett Packard Enterprise Development LP.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <cassert>

#include <unordered_map>

// External interface
#include "zmqnet.hpp"

// Internal helpers and classes
#include "libzmqnet.hpp"

// Static asynchronous frontend functions to be run in own threads
namespace workers
{

static bool response_callback(zmq::context_t& ctx, zmqnet::Stream::Id streamId, zmqnet::AsyncFrontend::CallbackFunc&& callbackFunc)
{
	// Open task socket to receive this stream's input from downstream data task
	auto callback_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	{ auto const callbackEndpoint = async::task_socket_endpoint(streamId);
		log("%d: connecting to callback endpoint %s\n", streamId, callbackEndpoint.c_str());
		callback_socket.connect(callbackEndpoint);
	}

	// Get forwarded stream create acknowledgement
	{ log("%d: waiting for stream create ack\n", streamId);
		auto reader = net::recv_MessageReader(callback_socket);
		auto response = reader.getRoot<NetworkResponse>();
		assert(response.isStream() && (response.getStream().getStreamId() == streamId));
	}

	while (true) {

		// Receive result from task socket
		log("%d: waiting for result from callback socket\n", streamId);
		auto reader = net::recv_MessageReader(callback_socket);
		auto response = reader.getRoot<NetworkResponse>();

		// If exit message, return
		if (response.isExit()) {
			return true;
		}

		// Check stream ID and user result
		assert(response.isUser() && (response.getUser().getStreamId() == streamId));

		// Run user-provided callback. TODO: pass ArrayPtr instead of new heap array
		callbackFunc(streamId, kj::heapArray(response.getUser().getContent().asBytes()));
	}
}

} // async

zmqnet::AsyncFrontend::AsyncFrontend(int const app_id, long int const session_id, long int const manifest_id,
	char const* const* leaf_argv, zmqnet::Topology const& topology)
	: Frontend{app_id, session_id, manifest_id, leaf_argv, topology}
	, m_ctx{}
	, m_callbackWorkers{}
	, m_stream_registering{std::make_unique<net::shared_semaphore>(false)}
	, m_responseDistributor{}
{
	// Launch callback distributor task
	m_responseDistributor = std::async(std::launch::async,
		[](zmq::context_t& ctx, zmq::socket_t&& input_socket, async::MaybeSemaphore maybeStreamSemaphore) {
			return async::distribute_messages<NetworkResponse>(ctx, std::move(input_socket), async::FrontendClient, std::move(maybeStreamSemaphore));
		}, std::ref(m_ctx), std::move(m_downstream->data_socket), async::MaybeSemaphore{*m_stream_registering});
}

zmqnet::AsyncFrontend::~AsyncFrontend()
{
	// Generate destructor for forward-declared net::shared_semaphore
}

zmqnet::Stream zmqnet::AsyncFrontend::registerStream(zmqnet::Stream::Id const& streamId, std::string const& filterLibraryName, std::string const& filterFunctionName,
	zmqnet::AsyncFrontend::CallbackFunc callbackFunc)
{
	// Ensure stream not registered
	if (m_callbackWorkers.find(streamId) != m_callbackWorkers.end()) {
		throw std::runtime_error("asynchronous callback already registered for stream ID " + std::to_string(streamId));
	}

	// Launch worker thread
	m_callbackWorkers.emplace(streamId, std::async(std::launch::async,
		[](zmq::context_t& ctx, zmqnet::Stream::Id streamId, CallbackFunc&& callbackFunc) {
			return workers::response_callback(ctx, streamId, std::move(callbackFunc));
		}, std::ref(m_ctx), streamId, std::move(callbackFunc)));

	// Lock stream registration flag shared with callback distributor task
	log("starting stream %d registration\n", streamId);
	auto stream_lock = m_stream_registering->acquire_lock();

	// Set stream registration flag
	assert(m_stream_registering->get(stream_lock) == false);
	m_stream_registering->set(stream_lock, true);

	// Send filter create command
	auto const result = createStream(streamId, filterLibraryName, filterFunctionName);

	// Wait for distributor to clear stream registration flag (registration flag is unlocked during wait)
	log("waiting for stream %d registration to complete\n", streamId);
	m_stream_registering->unlock_and_wait_until(std::move(stream_lock), false);
	log("stream %d registration completed\n", streamId);

	// Return new stream handle
	return result;
}

void zmqnet::AsyncFrontend::sendCommand(zmqnet::Stream const& stream, char const* command_buffer, size_t command_len)
{
	// Ensure stream registered
	if (m_callbackWorkers.find(stream.id) == m_callbackWorkers.end()) {
		throw std::runtime_error("no asynchronous callback registered for stream ID " + std::to_string(stream.id));
	}

	// Send user command
	Frontend::sendCommand(stream, command_buffer, command_len);
}

void zmqnet::AsyncFrontend::runExit()
{
	auto const start_ms = get_milliseconds();
	log("sending exit command\n");

	// Send exit command
	sendExit();

	// Wait for stream workers
	for (auto&& [streamId, callbackWorker] : m_callbackWorkers) {
		(void)callbackWorker.get();
	}

	// Reacquire downstream data socket from exiting callback processor
	m_downstream->data_socket = m_responseDistributor.get();

	log("exit completed\n");

	auto const exit_ms = get_milliseconds();
	log("exit RT: %5ldms\n", exit_ms - start_ms);
}
