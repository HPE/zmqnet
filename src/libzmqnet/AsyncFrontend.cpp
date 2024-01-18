/*
 * Copyright 2020 Hewlett Packard Enterprise Development LP.
 */

#include <config.h>

#include <cassert>
#include <thread>

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
static zmq::socket_t response_distributor(zmq::context_t& ctx, zmq::socket_t&& input_socket, std::vector<std::string>&& pingEndpoints, const lrle_bitset& allDescendants, net::shared_semaphore& blocking_registration, zmqnet::UserCallbacks& uCbs)
{
	auto taskSockets = std::unordered_map<zmqnet::Stream::Id, zmq::socket_t>{};

	// set up outgoing ping sockets. this thread expects pings to come in with an endpoint
	// in the ping payload.
	struct PingListener {
		std::string endpoint;
		zmq::socket_t socket;
	};
	auto pingSockets = std::vector<PingListener>{};

	for (const auto& endpoint : pingEndpoints) {
		pingSockets.emplace_back(PingListener{
			.endpoint = endpoint,
			.socket = zmq::socket_t{ctx, zmq::socket_type::push}
		});
		pingSockets.back().socket.bind(endpoint);
	}

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
			log("distributor: got ping response \"%s\"\n", response.getPing().getPayload().cStr());

			const auto payload = response.getPing().getPayload();

			{	// find socket that matches payload and forward it. in practice we expect
				// `pingSockets` to always only have two entries so we just linear search it. this
				// should be made into a [hash]map if that number gets much bigger in the future.
				auto it = std::find_if(pingSockets.begin(), pingSockets.end(), [&](const auto& listener) {
					return std::string_view{listener.endpoint} == std::string_view{payload.begin(), payload.size()};
				});

				if (it != pingSockets.end()) {
					net::send_MessageReader(it->socket, std::move(reader));
				} else {
					log("WARNING: dropped ping with payload \"%s\" - no matching recipients\n",
						response.getPing().getPayload().cStr());
				}
			}
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

// Implementation for heartbeat monitor
class zmqnet::AsyncFrontend::HeartbeatManager
{

	void heartbeatThread(zmq::context_t& ctx,
	                     std::string controlEndpoint, std::string heartbeatPingEndpoint,
	                     zmqnet::Communicator communicator, int interval_ms)
	{
		// Check interval timeout
		if (interval_ms <= 0) {
			m_live.store(false, std::memory_order_relaxed);
			return;
		}

		m_cancel.store(false, std::memory_order_relaxed);

		auto controlSocket = zmq::socket_t{ctx, zmq::socket_type::push};
		controlSocket.connect(controlEndpoint);

		auto pingSocket = zmq::socket_t{ctx, zmq::socket_type::pull};
		pingSocket.connect(heartbeatPingEndpoint);

		try {

			while (!m_cancel.load(std::memory_order_relaxed)) {

				// Next check interval
				auto wait_until = std::chrono::steady_clock::now()
					+ std::chrono::milliseconds{interval_ms};

				// Send ping command on communicator and update state
				auto any_response = !Frontend::sendPing(communicator, controlSocket, pingSocket, heartbeatPingEndpoint, interval_ms, {}).empty();
				m_live.store(any_response, std::memory_order_relaxed);

				// Wait for the rest of the check interval
				std::this_thread::sleep_until(wait_until);
			}

		// Frontend exit commands will reset ZMQ context, which causes the sendPing operation to throw
		// if it is in progress
		} catch (std::exception const& ex) {
			log("Heartbeat exception: %s\n", ex.what());
			m_live.store(false, std::memory_order_relaxed);
			m_cancel.store(true, std::memory_order_relaxed);
		}
	}

	std::atomic<bool> m_live, m_cancel;
	std::mutex m_mtx;
	std::condition_variable m_cv;
	std::thread m_heartbeatThread;

	public: // interface

	HeartbeatManager(zmq::context_t& ctx, std::string controlEndpoint, std::string heartbeatPingEndpoint, zmqnet::Communicator communicator, int interval_ms)
		: m_live{true}
		, m_cancel{true}
		, m_heartbeatThread{&HeartbeatManager::heartbeatThread, this, std::ref(ctx),
		                    std::move(controlEndpoint), std::move(heartbeatPingEndpoint),
		                    communicator, interval_ms}
	{
		// Check timeout interval
		if (interval_ms <= 0) {
			throw std::invalid_argument("Heartbeat interval must be positive non-zero");
		}
	}

	~HeartbeatManager()
	{
		// Cancel and wait for heartbeat thread
		m_cancel.store(true, std::memory_order_relaxed);
		m_heartbeatThread.join();
	}

	bool running()
	{
		return !m_cancel.load(std::memory_order_relaxed);
	}

	bool live()
	{
		return m_live.load(std::memory_order_relaxed);
	}
};

zmqnet::AsyncFrontend::AsyncFrontend(int const app_id, long int const session_id, long int const manifest_id,
	Topology const& topology, char const* const* leaf_argv, char const* const* leaf_env,
	UserCallbacks&& cbacks, const std::atomic<bool>* killSwitch)

	: Frontend{app_id, session_id, manifest_id, topology, leaf_argv, leaf_env, std::move(cbacks), killSwitch}
	, m_callbackProcessors{}
	, m_blocking_registration{std::make_unique<net::shared_semaphore>(false)}
	, m_responseDistributor{}
	, m_pingSocket(m_ctx, zmq::socket_type::pull)
	, m_controlSocket(m_ctx, zmq::socket_type::push)
	, m_heartbeatManager{nullptr}
{
	// Create socket for distributor task to forward pings to
	m_pingSocket.connect(m_pingEndpoint);

	// Callback distributor thread shouldn't handle any signals. Signal mask will be
	// inherited when thread is launched.
	auto signal_blocker = ThreadSignalBlocker();

	// Launch command distributor task. To send commands downstream, send commands to
	// m_commmandEndpoint and they will be forwarded by this thread. (this makes sending
	// commands thread safe)
	m_commandDistributor = std::async(std::launch::async,
		[](zmq::context_t& ctx, const char* input_endpoint, zmq::socket_t&& output_socket) {
			auto input_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
			input_socket.bind(input_endpoint);

			// blocks until ctx is closed, at which point ETERM will be thrown
			try {
				zmq::proxy(input_socket, output_socket);
			} catch (zmq::error_t& e) {
				if (e.num() != ETERM) throw;
				log("command distributor: got ETERM, exiting.\n");
			}
			log("command distributor: returning!\n");
			return std::move(output_socket);
		},
		std::ref(m_ctx), m_commandDistributorEndpoint, std::move(m_downstream->control_socket)
	);

	// Connect command socket
	m_controlSocket.connect(m_commandDistributorEndpoint);

	// Launch callback distributor task
	m_responseDistributor = std::async(std::launch::async, workers::response_distributor,
		std::ref(m_ctx), std::move(m_downstream->data_socket), std::vector<std::string>{m_pingEndpoint, m_heartbeatPingEndpoint},
		std::ref(*m_allDescendants), std::ref(*m_blocking_registration), std::ref(m_uCallbacks));
}

zmqnet::AsyncFrontend::~AsyncFrontend()
{
	// despite calling shutdown in runExit, we also call it here in case the AsyncFrontend
	// is destructed unexpectedly (e.g. after an uncaught exception throw).
	//
	// this is already called in m_ctx's destructor, but since it's declared in the parent
	// Frontend class, it's one of the last destructors to run and AsyncFrontend's destructor
	// tries to collect the worker threads first, which run forever unless told to stop.
	//
	// the shutdown will cause worker threads to die and the destructor can proceed with
	// thread cleanup and then properly destruct m_ctx in the parent destructor.
	m_ctx.shutdown();
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
	Frontend::sendCommand(m_controlSocket, stream, command_buffer, command_len);
}

void zmqnet::AsyncFrontend::sendCommand(zmqnet::Stream const& stream, std::string const& command)
{
	return sendCommand(stream, command.c_str(), command.length() + 1);
}

zmqnet::Communicator zmqnet::AsyncFrontend::createCommunicator(const RankSet& ranks) {
	// forward to generic impl in zmqnet::Frontend
	return Frontend::createCommunicator(m_controlSocket, ranks);
}

zmqnet::Stream zmqnet::AsyncFrontend::createStream(
	Communicator const& communicator, Stream::Id const& streamId,
	std::string const& filterLibraryName, std::string const& filterFunctionName,
	zmqnet::StreamSync const& streamSync)
{
	// forward to generic impl in zmqnet::Frontend
	return Frontend::createStream(
		m_controlSocket, communicator, streamId,
		filterLibraryName, filterFunctionName, streamSync
	);
}

void zmqnet::AsyncFrontend::sendExit() {
	// forward to generic impl in zmqnet::Frontend
	Frontend::sendExit(m_controlSocket);
}

zmqnet::RankSet zmqnet::AsyncFrontend::sendPing(Communicator const& comm, int timeoutMs, zmqnet::RankSet const& stopAfter)
{
	// forward to generic impl in zmqnet::Frontend
	// response_distributor thread is hard coded to forward ping responses to m_pingSocket
	return Frontend::sendPing(comm, m_controlSocket, m_pingSocket, m_pingEndpoint, timeoutMs, stopAfter);
}

bool zmqnet::AsyncFrontend::confirmPing(Communicator const& comm, RankSet const& ranks)
{
	// forward to generic impl in zmqnet::Frontend
	// response_distributor thread is hard coded to forward ping responses to m_pingSocket
	return Frontend::confirmPing(comm, ranks, m_controlSocket, m_pingSocket, m_pingEndpoint);
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

	// Cancel heartbeat - do this before the command distributor thread
	// because the heartbeat uses the command distributor thread
	m_heartbeatManager.reset();

	// Shut down all sockets - causes command distributor thread to exit
	m_ctx.shutdown();

	// Reacquire downstream command socket from exiting command distributor
	m_downstream->control_socket = m_commandDistributor.get();

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

	// Cancel heartbeat
	m_heartbeatManager.reset();

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

	// Reacquire downstream control socket from exiting command distributor.
	// Unlike the others socket threads, the ETERM exception is caught in
	// the command distributor thread.
	m_downstream->control_socket = m_commandDistributor.get();

	log("forced exit completed\n");
}

void zmqnet::AsyncFrontend::startHeartbeat(int interval_ms)
{
	if (!m_heartbeatManager) {
		auto communicator = getAllNodesCommunicator();
		m_heartbeatManager = std::make_unique<HeartbeatManager>(
			m_ctx, m_commandDistributorEndpoint, m_heartbeatPingEndpoint,
			communicator, interval_ms);
	}
}

bool zmqnet::AsyncFrontend::heartbeatRunning()
{
	return (m_heartbeatManager)
		? m_heartbeatManager->running()
		: false;
}

bool zmqnet::AsyncFrontend::heartbeatLive()
{
	return (m_heartbeatManager)
		? m_heartbeatManager->live()
		: false;
}
