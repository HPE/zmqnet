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
namespace async
{

static bool callback_worker(zmq::context_t& ctx, zmqnet::StreamId streamId, zmqnet::AsyncFrontend::CallbackFunc&& callbackFunc)
{
	// Open task socket to receive this stream's input from downstream data task
	auto callback_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	{ auto const callbackEndpoint = "inproc://stream-" + std::to_string(streamId);
		log("%d: connecting to callback endpoint %s\n", streamId, callbackEndpoint.c_str());
		callback_socket.connect(callbackEndpoint);
	}

	// Get forwarded stream create acknowledgement
	{ log("%d: waiting for stream create ack\n", streamId);
		auto reader = net::recv_MessageReader(callback_socket);
		auto response = reader.getRoot<NetworkResponse>();
		assert(response.isStreamAck() && (response.getStreamAck().getStreamId() == streamId));
	}

	while (true) {

		// Receive result from task socket
		log("%d: waiting for result from callback socket\n", streamId);
		auto reader = net::recv_MessageReader(callback_socket);
		auto response = reader.getRoot<NetworkResponse>();

		// If exit message, return
		if (response.isExitAck()) {
			return true;
		}

		// Check stream ID and user result
		assert(response.isUserResult() && (response.getUserResult().getStreamId() == streamId));

		// Run user-provided callback. TODO: pass ArrayPtr instead of new heap array
		callbackFunc(kj::heapArray(response.getUserResult().getContent().asBytes()));
	}
}

static zmq::socket_t callback_distributor(zmq::context_t& ctx, zmq::socket_t&& downstream_data_socket, net::shared_semaphore& stream_registering)
{
	auto callbackSockets = std::unordered_map<zmqnet::StreamId, zmq::socket_t>{};

	while (true) {
		// Receive aggregated result
		log("dist: receiving results...\n");
		auto result = net::recv_message(downstream_data_socket);
		auto reader = net::make_MessageReader(result);
		auto response = reader.getRoot<NetworkResponse>();

		// If exit, clean up
		if (response.isExitAck()) {
			log("dist: exitAck\n");

			// Relay exit message
			for (auto&& [streamId, callback_socket] : callbackSockets) {
				callback_socket.send(result, zmq::send_flags::none);
			}

			break;
		}

		// Response is either stream acknowledgement or user result
		assert(response.isStreamAck() || response.isUserResult());

		// Extract stream ID to select stream worker
		auto const streamId = (response.isStreamAck())
			? response.getStreamAck().getStreamId()
			: response.getUserResult().getStreamId();

		// If new stream, create task distribution socket
		if (response.isStreamAck()) {
			log("dist: streamAck\n");

			// Find callback corresponding to this stream
			auto const streamId = response.getStreamAck().getStreamId();

			// Lock stream registration flag shared with main thread
			auto stream_lock = stream_registering.acquire_lock();
			assert(stream_registering.get(stream_lock) == true);

			// Launch and relay to callback socket (endpoint is based on stream ID)
			{ auto callback_socket = zmq::socket_t{ctx, zmq::socket_type::push};
				auto callbackEndpoint = "inproc://stream-" + std::to_string(streamId);
				log("creating new task socket %s\n", callbackEndpoint.c_str());
				callback_socket.bind(callbackEndpoint);

				// Relay result to new callback socket
				callback_socket.send(result, zmq::send_flags::none);

				// Store in stream callback socket map
				callbackSockets.emplace(streamId, std::move(callback_socket));
			}

			// Notify main thread that stream registration has completed
			log("dist: notifying main thread that stream registration has completed\n");
			stream_registering.set(stream_lock, false);
			stream_registering.unlock_and_notify_one(std::move(stream_lock));

			continue;
		}

		// Otherwise, response to user result
		assert(response.isUserResult());
		log("dist: userResult\n");

		// Find callback socket corresponding to this stream
		auto streamIdSocketPair = callbackSockets.find(streamId);
		if (streamIdSocketPair == callbackSockets.end()) {
			throw std::runtime_error("no callback socket registered for stream ID " + std::to_string(streamId));
		}

		// Relay user response
		streamIdSocketPair->second.send(result, zmq::send_flags::none);
	}

	// Return control of the downstream data socket back to main thread
	return std::move(downstream_data_socket);
}

} // async

zmqnet::AsyncFrontend::AsyncFrontend(int const app_id, long int const session_id, long int const manifest_id,
	char const* const* leaf_argv, zmqnet::Topology const& topology)
	: Frontend{app_id, session_id, manifest_id, leaf_argv, topology}
	, m_ctx{}
	, m_callbackWorkers{}
	, m_stream_registering{std::make_unique<net::shared_semaphore>(false)}
	, m_callbackDistributor{}
{
	// Launch callback distributor task
	m_callbackDistributor = std::async(std::launch::async,
		[](zmq::context_t& ctx, zmq::socket_t&& downstream_data_socket, net::shared_semaphore& stream_registering) {
			return async::callback_distributor(ctx, std::move(downstream_data_socket), stream_registering);
		}, std::ref(m_ctx), std::move(m_downstream->data_socket), std::ref(*m_stream_registering));
}

zmqnet::AsyncFrontend::~AsyncFrontend()
{
	// Generate destructor for forward-declared net::shared_semaphore
}

void zmqnet::AsyncFrontend::registerStream(zmqnet::StreamId const& streamId, std::string const& filterLibraryName, std::string const& filterFunctionName,
	zmqnet::AsyncFrontend::CallbackFunc callbackFunc)
{
	// Ensure stream not registered
	if (m_callbackWorkers.find(streamId) != m_callbackWorkers.end()) {
		throw std::runtime_error("asynchronous callback already registered for stream ID " + std::to_string(streamId));
	}

	// Launch worker thread
	m_callbackWorkers.emplace(streamId, std::async(std::launch::async,
		[](zmq::context_t& ctx, zmqnet::StreamId streamId, zmqnet::AsyncFrontend::CallbackFunc&& callbackFunc) {
			return async::callback_worker(ctx, streamId, std::move(callbackFunc));
		}, std::ref(m_ctx), streamId, std::move(callbackFunc)));

	// Lock stream registration flag shared with callback distributor task
	log("starting stream registration\n");
	auto stream_lock = m_stream_registering->acquire_lock();

	// Set stream registration flag
	assert(m_stream_registering->get(stream_lock) == false);
	m_stream_registering->set(stream_lock, true);

	// Send filter create command
	createStream(streamId, filterLibraryName, filterFunctionName);

	// Wait for distributor to clear stream registration flag (registration flag is unlocked during wait)
	log("waiting for stream registration to complete\n");
	m_stream_registering->unlock_and_wait_until(std::move(stream_lock), false);
}

void zmqnet::AsyncFrontend::sendCommand(zmqnet::StreamId const& streamId, std::string const& command)
{
	// Ensure stream registered
	if (m_callbackWorkers.find(streamId) == m_callbackWorkers.end()) {
		throw std::runtime_error("no asynchronous callback registered for stream ID " + std::to_string(streamId));
	}

	// Send user command
	Frontend::sendCommand(streamId, command);
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
	m_downstream->data_socket = m_callbackDistributor.get();

	log("exit completed\n");

	auto const exit_ms = get_milliseconds();
	log("exit RT: %5ldms\n", exit_ms - start_ms);
}
