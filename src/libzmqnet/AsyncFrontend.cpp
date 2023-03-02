/*
 * Copyright 2020 Hewlett Packard Enterprise Development LP.
 */

#include <config.h>

#include <cassert>

#include <unordered_map>

// External interface
#include "zmqnet.hpp"

// Internal helpers and classes
#include "libzmqnet.hpp"
#include "lrle_bitset_impl.hpp"

static int bitsetToInt(const lrle_bitset& bitset) {
	return lrle_next_set_bit(bitset.compressed.data(), bitset.compressed.size(), nullptr);
}

// Static asynchronous frontend functions to be run in own threads
namespace workers
{

static void callback_processor(zmq::context_t& ctx, zmqnet::Stream const& stream, zmqnet::AsyncFrontend::CallbackFunc&& callbackFunc)
{
	// Open task socket to receive this stream's input from downstream data task
	auto task_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	{ auto const taskEndpoint = async::stream_endpoint(stream);
		log("%d: connecting to task endpoint %s\n", stream.id, taskEndpoint.c_str());
		task_socket.bind(taskEndpoint);
	}

	// Wait for stream create acknowledgement
	{ log("%d: waiting for stream create ack\n", stream.id);
		auto reader = net::recv_MessageReader(task_socket);
		auto response = reader.getRoot<NetworkResponse>();
		assert(response.isStream() && (response.getStream().getStreamId() == stream.id));
	}

	while (true) {

		// Receive result from task socket
		log("%d: waiting for result from task socket\n", stream.id);
		auto reader = net::recv_MessageReader(task_socket);
		auto response = reader.getRoot<NetworkResponse>();
		auto const communicatorId = reader.getTag();

		// If exit message, return
		if (response.isExit()) {
			log("%d: exiting\n", stream.id);
			return;

		// If existing stream response, ignore
		} else if (response.isStream()) {
			auto const streamId = response.getStream().getStreamId();
			assert(streamId == stream.id);
			log("%d: existing stream on new communicator %02x\n", stream.id, communicatorId);

			continue;
		}

		// Check stream ID and user result
		assert(response.isUser() && (response.getUser().getStreamId() == stream.id));

		// Run user-provided callback. TODO: pass ArrayPtr instead of new heap array
		callbackFunc(stream.id, zmqnet::ByteArrayPtr{response.getUser().getContent().asBytes()});
	}
}

// Select stream task based on incoming message's stream ID, and send message to stream task socket
// Stream tasks are created by main thread.
// Completed when received an exit message.
// Returns control of input socket upon completion.
static zmq::socket_t response_distributor(zmq::context_t& ctx, zmq::socket_t&& input_socket, const char* ping_endpoint, const lrle_bitset& allDescendants, net::shared_semaphore& blocking_registration, zmqnet::UserCallbacks& uCbs)
{
	auto taskSockets = std::unordered_map<zmqnet::Stream::Id, zmq::socket_t>{};
	auto pingSocket = zmq::socket_t{ctx, zmq::socket_type::push};
	pingSocket.connect(ping_endpoint);

	auto exitedDescendants = lrle_bitset{};

	while (true) {
		// Receive next message
		log("distributor: receiving message...\n");
		auto reader = net::recv_MessageReader(input_socket);
		auto response = reader.getRoot<NetworkResponse>();
		auto const communicatorId = reader.getTag();

		// If exit, record exited nodes and clean up when all have exited
		if (response.isExit()) {
			auto type = response.getExit().getNodeType();
			auto nodes_buf = response.getExit().getNodes().asBytes();
			auto nodes = lrle_bitset(nodes_buf.begin(), nodes_buf.size());
			log("distributor: exit (%d, %s)\n", type, nodes.to_string().c_str());

			if (type == NodeType::LEAF) {
				// Call user callback
				uCbs.invokeCallback<zmqnet::Event::LeafExit>(bitsetToInt(nodes));
			} else if (type == NodeType::COMMNODE) {
				// Record exited nodes
				exitedDescendants |= nodes;
				if (exitedDescendants == allDescendants) {
					log("distributor: all descendants exited. cleaning up.\n");
					// Relay exit message to task sockets
					for (auto&& [streamId, task_socket] : taskSockets) {
						net::send_MessageReader_copy(task_socket, reader);
					}

					// Return control of the upstream control socket back to main thread
					return std::move(input_socket);
				}
			} else {
				// should never happen
				throw std::runtime_error("bad NodeType in exit response");
			}
		}

		// If ping, forward to ping endpoint
		if (response.isPing()) {
			log("distributor: got ping response\n");
			net::send_MessageReader(pingSocket, std::move(reader));
			continue;
		}

		// Stream / communicator messages come from a blocking registration command and should notify main thread
		auto lockRegistration = response.isStream() || response.isCommunicator();
		if (lockRegistration) {

			// Lock registration flag shared with main thread
			auto registration_lock = blocking_registration.acquire_lock();
			assert(blocking_registration.get(registration_lock) == true);
		}

		// Stream / user messages are relayed to the corresponding stream callback processor
		if (response.isStream() || response.isUser()) {

			// Extract stream ID to select stream worker
			auto const streamId = (response.isStream())
				? response.getStream().getStreamId()
				: response.getUser().getStreamId();

			// Get socket for given stream ID
			auto const idSocketPair = taskSockets.find(streamId);

			// Create new stream task connection with known endpoint format, if needed
			if (idSocketPair == taskSockets.end()) {
				log("distributor: new stream %d\n", streamId);
				assert(response.isStream());

				// Reconstitute stream object
				auto const stream = zmqnet::Stream { .communicatorId = communicatorId, .id = streamId };

				// Create task socket and store in stream task socket map
				taskSockets.emplace_hint(idSocketPair, streamId,
					async::make_task_socket(ctx, stream, std::move(reader)));

			// Relay message to task socket
			} else {
				log("distributor: relay message to stream %d\n", streamId);
				net::send_MessageReader(idSocketPair->second, std::move(reader));
			}
		}

		// Stream / communicator messages come from a blocking registration command and should notify main thread
		if (lockRegistration) {
			auto registration_lock = blocking_registration.acquire_lock();

			log("distributor: notifying main thread that registration has completed\n");
			if (blocking_registration.get(registration_lock) == true) {
				blocking_registration.set(registration_lock, false);
				blocking_registration.unlock_and_notify_one(std::move(registration_lock));
			}
		}
	}
}

} // async

zmqnet::AsyncFrontend::AsyncFrontend(int const app_id, long int const session_id, long int const manifest_id,
	Topology const& topology, char const* const* leaf_argv, char const* const* leaf_env,
	UserCallbacks&& cbacks, const std::atomic<bool>* killSwitch)

	: Frontend{app_id, session_id, manifest_id, topology, leaf_argv, leaf_env, std::move(cbacks), killSwitch}
	, m_callbackProcessors{}
	, m_blocking_registration{std::make_unique<net::shared_semaphore>(false)}
	, m_responseDistributor{}
	, m_pingSocket(m_ctx, zmq::socket_type::pull)
{
	// Create socket for distributor task to forward pings to
	m_pingSocket.bind(m_pingEndpoint);

	// Callback distributor thread shouldn't handle any signals. Signal mask will be
	// inherited when thread is launched.
	auto signal_blocker = ThreadSignalBlocker();

	// Launch callback distributor task
	m_responseDistributor = std::async(std::launch::async, workers::response_distributor,
		std::ref(m_ctx), std::move(m_downstream->data_socket), m_pingEndpoint,
		std::ref(*m_allDescendants), std::ref(*m_blocking_registration), std::ref(m_uCallbacks));
}

zmqnet::AsyncFrontend::~AsyncFrontend()
{
	// Generate destructor for forward-declared net::shared_semaphore
}

zmqnet::Communicator zmqnet::AsyncFrontend::getAllNodesCommunicator() const
{
	return Frontend::getAllNodesCommunicator();
}

zmqnet::Communicator zmqnet::AsyncFrontend::registerCommunicator(zmqnet::NodeSet const& nodes)
{
	return registerCommunicator(Frontend::toRankSet(nodes));
}

zmqnet::Communicator zmqnet::AsyncFrontend::registerCommunicator(const RankSet &ranks)
{
	if (ranks.empty()) {
		throw std::runtime_error("expected non-empty ranks set");
	}

	// Lock blocking registration flag shared with distributor task
	log("starting communicator registration\n");
	auto registration_lock = m_blocking_registration->acquire_lock();

	// Set registration flag
	assert(m_blocking_registration->get(registration_lock) == false);
	m_blocking_registration->set(registration_lock, true);

	// Send communicator create command
	auto const communicator = createCommunicator(ranks);

	// Wait for distributor to clear registration flag (registration flag is unlocked during wait)
	log("waiting for communicator %02x registration to complete\n", communicator.id);
	m_blocking_registration->unlock_and_wait_until(std::move(registration_lock), false);

	// Leaves and commnodes have now received the message to subscribe to a
	// new zmq channel and are busy subscribing. Wait some time for them to finish.
	if (!confirmPing(communicator, ranks)) {
		throw std::runtime_error("Nodes failed to connect to communicator");
	}

	log("communicator %02x registration completed\n", communicator.id);

	// Return new communicator handle
	return communicator;
}


void zmqnet::AsyncFrontend::registerStreamCallback(Stream::Id const& streamId, zmqnet::AsyncFrontend::CallbackFunc callbackFunc)
{
	// Ensure stream callback not registered
	if (m_callbackProcessors.find(streamId) != m_callbackProcessors.end()) {
		throw std::runtime_error("asynchronous callback already registered for stream ID " + std::to_string(streamId));
	}

	// Reconstitute stream object
	auto const stream = zmqnet::Stream { .communicatorId = net::communicator_ids::AllNodes, .id = streamId };

	// Worker thread shouldn't handle any signals. Signal mask will be inherited when thread
	// is launched.
	auto signal_blocker = ThreadSignalBlocker();

	// Launch worker thread
	m_callbackProcessors.emplace(streamId, std::async(std::launch::async,
		[](zmq::context_t& ctx, zmqnet::Stream const& stream, CallbackFunc&& callbackFunc) {
			return workers::callback_processor(ctx, stream, std::move(callbackFunc));
		}, std::ref(m_ctx), stream, std::move(callbackFunc)));
}

zmqnet::Stream zmqnet::AsyncFrontend::registerStream(Communicator const& communicator, Stream::Id const& streamId, std::string const& filterLibraryName, std::string const& filterFunctionName,
	zmqnet::StreamSync const& streamSync)
{
	// Ensure stream callback registered
	if (m_callbackProcessors.find(streamId) == m_callbackProcessors.end()) {
		throw std::runtime_error("asynchronous callback not registered for stream ID " + std::to_string(streamId));
	}

	// Lock blocking registration flag shared with distributor task
	log("starting stream %d registration\n", streamId);
	auto registration_lock = m_blocking_registration->acquire_lock();

	// Set registration flag
	assert(m_blocking_registration->get(registration_lock) == false);
	m_blocking_registration->set(registration_lock, true);

	// Send filter create command
	auto const result = createStream(communicator, streamId, filterLibraryName, filterFunctionName, streamSync);

	// Wait for distributor to clear registration flag (registration flag is unlocked during wait)
	log("waiting for stream %d registration to complete\n", streamId);
	m_blocking_registration->unlock_and_wait_until(std::move(registration_lock), false);
	log("stream %d registration completed\n", streamId);

	// Return new stream handle
	return result;
}

void zmqnet::AsyncFrontend::sendCommand(zmqnet::Stream const& stream, char const* command_buffer, size_t command_len)
{
	// Ensure stream registered
	if (m_callbackProcessors.find(stream.id) == m_callbackProcessors.end()) {
		throw std::runtime_error("no asynchronous callback registered for stream ID " + std::to_string(stream.id));
	}

	// Send user command
	Frontend::sendCommand(stream, command_buffer, command_len);
}

void zmqnet::AsyncFrontend::sendCommand(zmqnet::Stream const& stream, std::string const& command)
{
	return sendCommand(stream, command.c_str(), command.length() + 1);
}

zmqnet::RankSet zmqnet::AsyncFrontend::sendPing(Communicator const& comm, int timeoutMs, zmqnet::RankSet const& stopAfter)
{
	// response_distributor thread will forward ping responses to m_pingSocket
	return Frontend::sendPing(comm, m_pingSocket, timeoutMs, stopAfter);
}

bool zmqnet::AsyncFrontend::confirmPing(Communicator const& comm, RankSet const& ranks)
{
	// response_distributor thread will forward ping responses to m_pingSocket
	return Frontend::confirmPing(comm, ranks, m_pingSocket);
}

void zmqnet::AsyncFrontend::runExit()
{
	// Send exit command
	sendExit();

	// Wait for stream workers
	for (auto&& [streamId, callbackWorker] : m_callbackProcessors) {
		(void)callbackWorker.get();
	}

	// Reacquire downstream data socket from exiting callback processor
	m_downstream->data_socket = m_responseDistributor.get();

	// Shut down all sockets
	m_ctx.shutdown();

	log("sync exit completed\n");
}

void zmqnet::AsyncFrontend::forceDisconnect()
{
	// Send exit command to try to get the backends to clean up.
	sendExit();

	// Shut down all sockets. This will cause threads in m_callbackProcessors and
	// m_responseDistributor to throw ETERM exceptions next time they recv and they will
	// exit.
	m_ctx.shutdown();

	// Wait for stream workers. ETERM exception will be rethrown on .get()
	for (auto&& [streamId, callbackWorker] : m_callbackProcessors) {
		try {
			(void)callbackWorker.get();
		} catch (const zmq::error_t& e) {
			if (e.num() != ETERM) {
				// unexpected error
				throw;
			}
		}
	}

	// Reacquire downstream data socket from exiting callback processor. ETERM exception
	// will be thrown on .get()
	try {
		m_downstream->data_socket = m_responseDistributor.get();
	} catch (const zmq::error_t& e) {
		if (e.num() != ETERM) {
			// unexpected error
			throw;
		}
	}

	log("forced exit completed\n");
}
