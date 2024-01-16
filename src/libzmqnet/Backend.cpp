/*
 * Copyright 2020 Hewlett Packard Enterprise Development LP.
 */

#include <config.h>

// External interface
#include "zmqnet.hpp"

// Internal helpers and classes
#include "libzmqnet.hpp"

#include "lrle_bitset_impl.hpp"

zmqnet::detail::Backend::Backend(BackendParams const& backendParams)
	: m_ctx{}
	, m_upstream{std::make_unique<net::UpstreamLink>(m_ctx,
		backendParams.upstreamControlEndpoint,
		backendParams.upstreamDataEndpoint,
		backendParams.authType,
		backendParams.upstreamPublicKey)}
	, m_node_id{backendParams.node_id}
	, m_rank_id{backendParams.rank_id}
	, m_target_pid{backendParams.target_pid}
	, m_node_bitset{std::move(lrle_bitset{(size_t)m_node_id}.compressed)}
{
	net::subscribe_to_tag(m_upstream->control_socket, net::communicator_ids::AllNodes);
}

zmqnet::detail::Backend::~Backend()
{
	m_ctx.shutdown();
}

int zmqnet::detail::Backend::getRank() const
{
	if (m_rank_id < 0) {
		throw std::logic_error("Used Backend::getRank, but backend was not started in packed mode");
	}

	return m_rank_id;
}

pid_t zmqnet::detail::Backend::getTargetPid() const
{
	if (m_target_pid < 0) {
		throw std::logic_error("Used Backend::getTargetPid, but backend was not started in packed mode");
	}

	return m_target_pid;
}

void zmqnet::detail::Backend::notifyReady()
{
	{   // Notify parent commnode that setup is completed
		log("backend %d: notifying parent ready from leaf node\n", m_node_id);
		auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		auto ready = response.initReady();

		// Set node bitset
		ready.setNodes(kj::ArrayPtr<kj::byte const>
			{ (unsigned char const*)m_node_bitset.data()
			, m_node_bitset.size()
		});
		
		// Send ready
		net::send_MessageBuilder(m_upstream->data_socket, net::communicator_ids::AllNodes, builder);
	}

	// Process initial commands:
	// First, the frontend will send pings until all nodes report back. If one of our sibling
	// backends misses the initial ping, the frontend will keep sending pings until they are
	// online. Once all nodes report back, the frontend will create the broadcast communicator.

	// We temporarily set a timeout on the upstream control socket just in case something goes
	// wrong and we never get a single ping command.
	auto oldTimeout = m_upstream->control_socket.get(zmq::sockopt::rcvtimeo);
	// default 300000 ms, 5 minutes.
	if (auto timeoutSec = startup_handshake_timeout(); timeoutSec > 0) {
		m_upstream->control_socket.set(zmq::sockopt::rcvtimeo, timeoutSec * 1000);
	}

	while (true) {
		log("backend %d: waiting for startup command...\n", m_node_id);
		if (auto maybeReader = net::recv_MessageReader_timeout(m_upstream->control_socket)) {
			auto& reader = *maybeReader;
			auto command = reader.getRoot<NetworkCommand>();

			if (command.isPing()) {
				log("backend %d: got startup ping \"%s\"\n", m_node_id, command.getPing().cStr());

				auto builder = capnp::MallocMessageBuilder{};
				auto response = builder.initRoot<NetworkResponse>();
				auto pong = response.initPing();

				pong.setNodeId(m_node_id);
				pong.setPayload(command.getPing());
				net::send_MessageBuilder(m_upstream->data_socket, reader.getTag(), builder);
			} else if (command.isCommunicator()) {
				auto const communicatorId = command.getCommunicator().getCommunicatorId();
				log("backend %d: new communicator %02x\n", m_node_id, communicatorId);
				assert(communicatorId == net::communicator_ids::AllNodes);

				// subscribe to subset tag
				log("backend %d: in communicator %02x, subscribing\n", m_node_id, communicatorId);

				// Subscribe to subset tag
				net::subscribe_to_tag(m_upstream->control_socket, communicatorId);

				// Create acknowledgement
				auto builder = capnp::MallocMessageBuilder{};
				auto response = builder.initRoot<NetworkResponse>();
				auto communicatorAck = response.initCommunicator();
				communicatorAck.setCommunicatorId(communicatorId);

				// Set node bitset
				communicatorAck.setNodes(kj::ArrayPtr<kj::byte const>
					{ (unsigned char const*)m_node_bitset.data()
					, m_node_bitset.size()
				});

				// Send acknowledgement
				net::send_MessageBuilder(m_upstream->data_socket, net::communicator_ids::AllNodes, builder);

				break;
			} else if (command.isKill()) {
				log("backend %d: Killed\n", m_node_id);
				assert(false);
				std::exit(-1);
			} else {
				log("backend %d: Invalid command sent during startup\n", m_node_id);
				assert(false);
				std::exit(-1);
			}
		} else {
			log("backend %d: Timed out waiting for startup commands\n", m_node_id);
			assert(false);
			std::exit(-1);
		}
	}

	// Restore old timeout
	m_upstream->control_socket.set(zmq::sockopt::rcvtimeo, oldTimeout);
}

void zmqnet::detail::Backend::sendResult(zmqnet::Stream const& stream, char const* result_buffer, size_t result_len)
{
	// Notify parent commnode of result
	auto builder = capnp::MallocMessageBuilder{};
	auto response = builder.initRoot<NetworkResponse>();
	auto user = response.initUser();
	user.setStreamId(stream.id);
	if (result_len > 0) {
		user.setContent(kj::ArrayPtr<kj::byte const>{(unsigned char const*)result_buffer, result_len});
	}

	// Set node bitset
	user.setNodes(kj::ArrayPtr<kj::byte const>
		{ (unsigned char const*)m_node_bitset.data()
		, m_node_bitset.size()
	});

	// Send result
	net::send_MessageBuilder(m_upstream->data_socket, stream.communicatorId, builder);
}

// Synchronous Backend

zmqnet::SyncBackend::SyncBackend(detail::Backend::BackendParams const& backendParams)
	: Backend{backendParams}
{
	// Backend instantiated, notify network ready
	notifyReady();
}

std::tuple<zmqnet::Stream, zmqnet::ByteArray> zmqnet::SyncBackend::nextCommand()
{
	while (true) {
		// Receive next network command
		log("backend %d: waiting for next command...\n", m_node_id);
		auto reader = net::recv_MessageReader(m_upstream->control_socket);
		auto command = reader.getRoot<NetworkCommand>();

		// Detect exit case
		if (command.isExit()) {
			log("backend %d: got exit\n", m_node_id);

			// Send exit acknowledgement to parent
			{ auto builder = capnp::MallocMessageBuilder{};
				auto response = builder.initRoot<NetworkResponse>();
				auto exit = response.initExit();

				// Set exit information
				exit.setNodeType(NodeType::LEAF);
				exit.setNodes(kj::ArrayPtr<kj::byte const>
					{ (unsigned char const*)m_node_bitset.data()
					, m_node_bitset.size()
				});

				net::send_MessageBuilder(m_upstream->data_socket, net::communicator_ids::AllNodes, builder);
			}

			// Shut down all sockets
			m_ctx.shutdown();

			return std::make_tuple(zmqnet::Stream { .id = -1 }, ByteArray{});

		// Last ditch kill signal
		} else if (command.isKill()) {
			log("backend %d: got kill.\n", m_node_id);
			std::exit(-1);

		// If ping, respond with local node bitset
		} else if (command.isPing()) {
			log("backend %d: got ping on communicator %d \"%s\".\n", m_node_id, reader.getTag(), command.getPing().cStr());
			auto builder = capnp::MallocMessageBuilder{};
			auto response = builder.initRoot<NetworkResponse>();
			auto pong = response.initPing();

			pong.setNodeId(m_node_id);
			pong.setPayload(command.getPing());

			net::send_MessageBuilder(m_upstream->data_socket, reader.getTag(), builder);

			continue;

		// Acknowledge and subscribe to communicator if node is in node set
		} else if (command.isCommunicator()) {
			auto const communicatorId = command.getCommunicator().getCommunicatorId();
			log("backend %d: new communicator %02x\n", m_node_id, communicatorId);

			// Get communicator node mask
			auto const communicator_nodes = command.getCommunicator().getNodes().asBytes();

			// If this node matches, subscribe to subset tag
			if ((communicatorId == net::communicator_ids::AllNodes)
			 || lrle_contains(communicator_nodes.begin(), communicator_nodes.size(), (size_t)m_node_id)) {

				log("backend %d: in communicator %02x, subscribing\n", m_node_id, communicatorId);

				// Subscribe to subset tag
				net::subscribe_to_tag(m_upstream->control_socket, communicatorId);

				// Create acknowledgement
				auto builder = capnp::MallocMessageBuilder{};
				auto response = builder.initRoot<NetworkResponse>();

				auto communicatorAck = response.initCommunicator();
				communicatorAck.setCommunicatorId(communicatorId);

				// Set node bitset
				communicatorAck.setNodes(kj::ArrayPtr<kj::byte const>
					{ (unsigned char const*)m_node_bitset.data()
					, m_node_bitset.size()
				});

				// Send acknowledgement
				net::send_MessageBuilder(m_upstream->data_socket, net::communicator_ids::AllNodes, builder);
			}

			continue;

		// Acknowledge stream
		} else if (command.isStream()) {
			auto const streamId = command.getStream().getStreamId();
			auto const communicatorId = reader.getTag();

			log("backend %d: new stream %d on communicator %02x\n", m_node_id, streamId, communicatorId);
			auto builder = capnp::MallocMessageBuilder{};
			auto response = builder.initRoot<NetworkResponse>();

			// Create acknowledgement
			auto streamAck = response.initStream();
			streamAck.setStreamId(streamId);

			// Set node bitset
			streamAck.setNodes(kj::ArrayPtr<kj::byte const>
				{ (unsigned char const*)m_node_bitset.data()
				, m_node_bitset.size()
			});

			net::send_MessageBuilder(m_upstream->data_socket, communicatorId, builder);

			continue;

		// Extract stream ID and user command
		} else {
			assert(command.isUser());

			// Reconstitute stream object
			auto const stream = zmqnet::Stream
				{ .communicatorId = reader.getTag()
				, .id = command.getUser().getStreamId()
			};

			log("backend %d: user command on communicator %02x stream %d\n",
				m_node_id, stream.communicatorId, stream.id);

			return std::make_tuple(stream, kj::heapArray(command.getUser().getContent().asBytes()));
		}
	}
}

void zmqnet::SyncBackend::sendResult(zmqnet::Stream const& stream, char const* result_buffer, size_t result_len)
{
	Backend::sendResult(stream, result_buffer, result_len);
}

// Asynchronous Backend

// Static asynchronous backend functions to be run in own threads
namespace workers
{

static void callback_processor(zmq::context_t& ctx, std::string resultEndpoint, int node_id, std::vector<unsigned char> const& node_bitset, zmqnet::Stream const& stream, zmqnet::AsyncBackend::CallbackFunc&& callbackFunc)
{
	// Open task socket to receive this stream's command from upstream control task
	auto task_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	{ auto const taskEndpoint = async::stream_endpoint(stream);
		log("backend %d %d: connecting to task endpoint %s\n", node_id, stream.id, taskEndpoint.c_str());
		task_socket.bind(taskEndpoint);
	}

	// Open result socket to send packets upstream
	log("backend %d %d: connecting to upstream proxy %s\n", node_id, stream.id, resultEndpoint.c_str());
	auto result_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	result_socket.connect(resultEndpoint);

	// Wait for stream create acknowledgement and acknowledge to proxy
	log("backend %d %d: waiting for stream create ack\n", node_id, stream.id);
	{ auto reader = net::recv_MessageReader(task_socket);
		auto command = reader.getRoot<NetworkCommand>();
		assert(command.isStream() && (command.getStream().getStreamId() == stream.id));
	}

	// Create acknowledgement
	log("backend %d %d: sending stream ack\n", node_id, stream.id);
	{ auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		auto streamAck = response.initStream();
		streamAck.setStreamId(stream.id);

		// Set node bitset
		streamAck.setNodes(kj::ArrayPtr<kj::byte const>
			{ (unsigned char const*)node_bitset.data()
			, node_bitset.size()
		});

		// Relay acknowledgement to proxy
		net::send_MessageBuilder(result_socket, net::communicator_ids::AllNodes, builder);
	}

	while (true) {

		// Receive result from task socket
		log("backend %d %d: waiting for command from task socket\n", node_id, stream.id);
		auto reader = net::recv_MessageReader(task_socket);
		auto command = reader.getRoot<NetworkCommand>();
		auto const communicatorId = reader.getTag();

		// If exit command, return
		if (command.isExit()) {
			log("backend %d %d: exiting\n", node_id, stream.id);
			return;

		// If stream command, send acknowledgement
		} else if (command.isStream()) {
			auto const streamId = command.getStream().getStreamId();
			log("backend %d: existing stream %d on new communicator %02x\n", node_id, streamId, communicatorId);

			// Create acknowledgement
			auto builder = capnp::MallocMessageBuilder{};
			auto response = builder.initRoot<NetworkResponse>();
			auto streamAck = response.initStream();
			streamAck.setStreamId(streamId);

			// Set node bitset
			streamAck.setNodes(kj::ArrayPtr<kj::byte const>
				{ (unsigned char const*)node_bitset.data()
				, node_bitset.size()
			});

			net::send_MessageBuilder(result_socket, communicatorId, builder);

			continue;
		}

		// Command is user command
		assert(command.isUser());

		// Check stream ID and user result
		assert(command.getUser().getStreamId() == stream.id);

		// Run user-provided callback. TODO: pass ArrayPtr instead of new heap array
		auto result = callbackFunc(stream.id, kj::heapArray(command.getUser().getContent().asBytes()));

		// Create outgoing result message
		if (result.size() > 0) {
			auto builder = capnp::MallocMessageBuilder{};
			auto response = builder.initRoot<NetworkResponse>();
			auto userResponse = response.initUser();
			userResponse.setStreamId(stream.id);
			auto const result_size = result.size();
			userResponse.setContent(std::move(result));

			// Set node bitset
			userResponse.setNodes(kj::ArrayPtr<kj::byte const>
				{ (unsigned char const*)node_bitset.data()
				, node_bitset.size()
			});

			// Send result
			log("backend %d %d: sending result size %lu\n", node_id, stream.id, result_size);
			net::send_MessageBuilder(result_socket, communicatorId, builder);
		}
	}
}

static zmq::socket_t command_distributor(zmq::context_t& ctx, zmq::socket_t&& input_socket,
	std::string resultEndpoint, int node_id,
	std::vector<unsigned char> const& node_bitset,
	zmqnet::detail::CallbackProcessorManager &callbackProcManager)
{
	auto taskSockets = std::unordered_map<zmqnet::Stream::Id, zmq::socket_t>{};

	// Open result socket to send acknowledgements to upstream proxy task
	log("backend %d: connecting to task result %s\n",
		node_id, resultEndpoint.c_str());
	auto result_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	result_socket.connect(resultEndpoint);

	while (true) {
		// Receive next message
		log("backend %d: receiving message...\n", node_id);
		auto reader = net::recv_MessageReader(input_socket);
		auto command = reader.getRoot<NetworkCommand>();

		// If exit, clean up
		if (command.isExit()) {
			log("backend %d: exit\n", node_id);

			// Relay exit message to task sockets
			for (auto&& [streamId, task_socket] : taskSockets) {
				net::send_MessageReader_copy(task_socket, reader);
			}

			// Return control of the upstream control socket back to main thread
			return std::move(input_socket);

		// Last ditch kill signal
		} else if (command.isKill()) {
			log("backend %d: got kill.\n", node_id);
			std::exit(-1);

		// If ping, respond with local node bitset
		} else if (command.isPing()) {
			log("backend %d: got ping on communicator %d. \"%s\"\n", node_id, reader.getTag(), command.getPing().cStr());
			auto builder = capnp::MallocMessageBuilder{};
			auto response = builder.initRoot<NetworkResponse>();
			auto pong = response.initPing();

			pong.setNodeId(node_id);
			pong.setPayload(command.getPing());

			net::send_MessageBuilder(result_socket, reader.getTag(), builder);

			continue;

		// Acknowledge and subscribe to communicator if node is in node set
		} else if (command.isCommunicator()) {
			auto const communicatorId = command.getCommunicator().getCommunicatorId();
			log("backend %d: new communicator %02x\n", node_id, communicatorId);

			// Get communicator node mask
			auto const communicator_nodes = command.getCommunicator().getNodes().asBytes();

			// If this node matches, subscribe to subset tag
			if ((communicatorId == net::communicator_ids::AllNodes)
			 || lrle_contains(communicator_nodes.begin(), communicator_nodes.size(), (size_t)node_id)) {

				log("backend %d: in communicator %02x, subscribing\n", node_id, communicatorId);

				// Subscribe to subset tag
				net::subscribe_to_tag(input_socket, communicatorId);

				// Create acknowledgement
				auto builder = capnp::MallocMessageBuilder{};
				auto response = builder.initRoot<NetworkResponse>();

				auto communicatorAck = response.initCommunicator();
				communicatorAck.setCommunicatorId(communicatorId);

				// Set node bitset
				communicatorAck.setNodes(kj::ArrayPtr<kj::byte const>
					{ (unsigned char const*)node_bitset.data()
					, node_bitset.size()
				});

				// Send acknowledgement
				net::send_MessageBuilder(result_socket, net::communicator_ids::AllNodes, builder);
			}

			continue;
		}

		assert(command.isStream() || command.isUser());

		// Extract stream ID to select stream worker
		auto const communicatorId = reader.getTag();
		auto const streamId = (command.isStream())
			? command.getStream().getStreamId()
			: command.getUser().getStreamId();

		// Get socket for given stream ID
		auto const idSocketPair = taskSockets.find(streamId);

		// Create new stream task connection with known endpoint format, if needed
		if (idSocketPair == taskSockets.end()) {
			assert(command.isStream());
			log("backend %d: new stream %d on communicator %02x\n", node_id, streamId, communicatorId);

			// Reconstitute stream object
			auto const stream = zmqnet::Stream { .communicatorId = net::communicator_ids::AllNodes, .id = streamId };

			// Activate thread
			// It is assumed the callback has already been registered by the user.
			log("backend %d: launching thread\n", node_id);
			callbackProcManager.launchCallbackProcessor(streamId);

			log("backend %d: recording task socket and sending ack to thread\n", node_id);
			// Create task socket and store in stream task socket map
			taskSockets.emplace_hint(idSocketPair, streamId,
				async::make_task_socket(ctx, stream, std::move(reader)));

		// Relay to socket for given stream ID
		} else {
			log("backend %d: relay message to stream %d\n", node_id, streamId);
			net::send_MessageReader(taskSockets.at(streamId), std::move(reader));
		}
	}
}

} // workers

zmqnet::detail::CallbackProcessorManager::CallbackProcessorManager(zmq::context_t &ctx, const std::string &collectorEndpoint, const int &nodeId, const std::vector<unsigned char> &nodeBitset)
	: m_ctx{ctx}
	, m_collectorEndpoint{collectorEndpoint}
	, m_nodeId{nodeId}
	, m_nodeBitset{nodeBitset}
{}

void zmqnet::detail::CallbackProcessorManager::registerCallback(Stream::Id const& streamId, CallbackFunc callbackFunc) {
	// Ensure stream not registered
	if (callbackProcessorExists(streamId)) {
		throw std::runtime_error("asynchronous callback already registered for stream ID " + std::to_string(streamId));
	}

	auto lock = std::scoped_lock<std::mutex>(m_callbackLock);

	// Set callback processor as ready
	m_waitingCallbackProcs.emplace(streamId, std::move(callbackFunc));
}

bool zmqnet::detail::CallbackProcessorManager::callbackProcessorExists(const Stream::Id &id) {
	auto lock = std::scoped_lock<std::mutex>(m_callbackLock);
	return m_waitingCallbackProcs.find(id) != m_waitingCallbackProcs.end()
		|| m_runningCallbackProcs.find(id) != m_runningCallbackProcs.end();
}

void zmqnet::detail::CallbackProcessorManager::launchCallbackProcessor(const Stream::Id &id) {
	auto lock = std::scoped_lock<std::mutex>(m_callbackLock);

	if (m_waitingCallbackProcs.find(id) == m_waitingCallbackProcs.end()) {
		throw std::runtime_error("tried to launch a callback processor with a streamId that was never registered.");
	}

	// Remove from waiting
	auto func = m_waitingCallbackProcs.at(id);
	m_waitingCallbackProcs.erase(id);

	// Reconstitute stream object
	auto const stream = zmqnet::Stream { .communicatorId = net::communicator_ids::AllNodes, .id = id };

	// Launch worker thread
	m_runningCallbackProcs.emplace(id, std::async(std::launch::async,
		[](zmq::context_t& ctx, std::string collectorEndpoint, int node_id, std::vector<unsigned char> const& node_bitset, zmqnet::Stream stream, CallbackFunc &&callbackFunc) {
			return workers::callback_processor(ctx, std::move(collectorEndpoint), node_id, node_bitset, stream, std::move(callbackFunc));
		}, std::ref(m_ctx), m_collectorEndpoint, m_nodeId, m_nodeBitset, stream, std::move(func)));
}

void zmqnet::detail::CallbackProcessorManager::waitForExit() {
	auto lock = std::scoped_lock<std::mutex>(m_callbackLock);

	for (auto&& [streamId, callback_processor] : m_runningCallbackProcs) {
		(void)callback_processor.get();
	}
}

bool zmqnet::detail::CallbackProcessorManager::streamIsActive(const Stream::Id &id) const {
	auto lock = std::scoped_lock<std::mutex>(m_callbackLock);
	return m_runningCallbackProcs.find(id) != m_runningCallbackProcs.end();
}

zmqnet::AsyncBackend::AsyncBackend(detail::Backend::BackendParams const& backendParams)
	: Backend{backendParams}
	, m_ctx{}

	, m_collectorEndpoint{"inproc://" + net::generate_uuid()}
	, m_collectorSocket{m_ctx, zmq::socket_type::push}
	, m_responseCollector{}
	, m_commandDistributor{}

	, m_callbackProcManager(m_ctx, m_collectorEndpoint, m_node_id, m_node_bitset)
{
	// Connect for message injection by main thread
	m_collectorSocket.connect(m_collectorEndpoint);
}

zmqnet::AsyncBackend::~AsyncBackend()
{
	// Generate destructor for forward-declared net::shared_semaphore
}

void zmqnet::AsyncBackend::registerCallback(zmqnet::Stream::Id const& streamId, zmqnet::AsyncBackend::CallbackFunc callbackFunc)
{
	m_callbackProcManager.registerCallback(streamId, callbackFunc);
}

void zmqnet::AsyncBackend::notifyReady()
{
	// Notify parent commnode that setup is completed
	Backend::notifyReady();

	// Launch response collector, moving control of upstream data socket
	m_responseCollector = std::async(std::launch::async,
		[](zmq::context_t& ctx, std::string collectorEndpoint, zmq::socket_t&& output_socket) {
			return async::collect_network_responses(ctx, std::move(collectorEndpoint), std::move(output_socket));
		}, std::ref(m_ctx), m_collectorEndpoint, std::move(m_upstream->data_socket));

	// Launch response distributor task, moving control of upstream control socket
	m_commandDistributor = std::async(std::launch::async,
		[](zmq::context_t& ctx, zmq::socket_t&& input_socket, std::string resultEndpoint, int node_id, std::vector<unsigned char> const& node_bitset, zmqnet::detail::CallbackProcessorManager &cpm) {
			return workers::command_distributor(ctx, std::move(input_socket), std::move(resultEndpoint), node_id, node_bitset, cpm);
		}, std::ref(m_ctx), std::move(m_upstream->control_socket), m_collectorEndpoint, m_node_id,
			m_node_bitset, std::ref(m_callbackProcManager));
}

void zmqnet::AsyncBackend::sendResult(const Stream::Id &streamId, zmqnet::ByteArray &&result) 
{
	// ensure streamId is valid
	if (!m_callbackProcManager.streamIsActive(streamId)) {
		log("backend %d: tried to directly send result to stream %d, but it doesn't exist. sending nothing.\n", m_node_id, streamId);
		return;
	}

	// package message
	auto builder = capnp::MallocMessageBuilder{};
	auto response = builder.initRoot<NetworkResponse>();
	auto userResponse = response.initUser();
	userResponse.setStreamId(streamId);
	auto const result_size = result.size();
	userResponse.setContent(std::move(result));

	// set node bitset
	userResponse.setNodes(kj::ArrayPtr<kj::byte const>
		{ (unsigned char const*)m_node_bitset.data()
		, m_node_bitset.size()
	});

	// send result
	log("backend %d %d: injecting result of size %lu\n", m_node_id, streamId, result_size);
	net::send_MessageBuilder(m_collectorSocket, net::communicator_ids::AllNodes, builder);
}

void zmqnet::AsyncBackend::waitForExit()
{
	// Wait for distributor to exit after receiving network exit command, reacquiring upstream data socket
	log("backend %d: waiting for distributor\n", m_node_id);
	m_upstream->control_socket = m_commandDistributor.get();

	// Wait for stream workers
	log("backend %d: waiting for stream workers\n", m_node_id);
	m_callbackProcManager.waitForExit();

	// Send exit acknowledgement to parent
	auto builder = capnp::MallocMessageBuilder{};
	auto response = builder.initRoot<NetworkResponse>();
	auto exit = response.initExit();

	// Set exit information
	// Collector proxy is rigged to shut down when it sees UNSET
	exit.setNodeType(NodeType::UNSET);
	exit.setNodes(kj::ArrayPtr<kj::byte const>
		{ (unsigned char const*)m_node_bitset.data()
		, m_node_bitset.size()
	});

	// Send exit message to proxy
	net::send_MessageBuilder(m_collectorSocket, net::communicator_ids::AllNodes, builder);

	// Reacquire upstream data socket from exiting proxy
	log("backend %d: waiting for proxy\n", m_node_id);
	m_upstream->data_socket = m_responseCollector.get();

	// Change to correct LEAF value, for upstream
	exit.setNodeType(NodeType::LEAF);

	// Send exit acknowledgement to parent
	log("backend %d: notifying parent of exit\n", m_node_id);
	net::send_MessageBuilder(m_upstream->data_socket, net::communicator_ids::AllNodes, builder);

	// Shut down all sockets
	m_ctx.shutdown();
}
