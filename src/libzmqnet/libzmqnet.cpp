/*
 * Copyright 2011-2020 Hewlett Packard Enterprise Development LP.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <unistd.h>
#include <stdarg.h>
#include <wait.h>

#include <cassert>
#include <cmath>

#include <filesystem>
#include <unordered_map>
#include <set>

#include "common_tools_fe.h"
#include "common_tools_be.h"

#include "argv_defs.hpp"

// External interface
#include "zmqnet.hpp"

// Internal helpers and classes
#include "libzmqnet.hpp"

// Build fanout tree in first-come first-served order. Does not require any precomputing of topology
// tree by the frontend.
static void startFcfsTree(zmq::context_t& ctx, net::TcpDownstreamLink& rootLink, int app_id, int session_id, int manifest_id, int fanout,
	char const* const* leaf_argv, std::string const& commnodePath, cti_hostsList_t const& hostsPlacement)
{
	// Start topology socket to send its connection info to commnode daemon
	auto dealer_socket = zmq::socket_t{ctx, zmq::socket_type::rep};
	dealer_socket.bind("tcp://*:*");
	auto const dealerEndpoint = net::replace_endpoint_host(dealer_socket.get(zmq::sockopt::last_endpoint), cti_getHostname());
	log("topology dealer listening on %s\n", dealerEndpoint.c_str());

	cti_addManifestBinary(manifest_id, commnodePath.c_str());

	// Set up commnode daemon arguments with topology connection info and leaf process arguments
	{ auto commnodeArgv = cti::OutgoingArgv<CommnodeArgv>{commnodePath};
		commnodeArgv.add(CommnodeArgv::DealerEndpoint, dealerEndpoint);

		// Add provided leaf arguments
		for (auto argptr = leaf_argv; *argptr != nullptr; argptr++) {
			commnodeArgv.add(cti::Argv::Argument{*argptr});
		}

		// Launch commnode daemon
		if (cti_execToolDaemon(manifest_id, commnodePath.c_str(), commnodeArgv.get() + 1, nullptr)) {
			throw std::runtime_error(cti_error_str());
		}
	}

	// Set up fanout topology and wait for all children to connect
	auto const [rootControlEndpoint, rootDataEndpoint] = formatEndpoints(rootLink, cti_getHostname());
	auto endpoints = std::vector<std::tuple<std::string, std::string>> {
		{rootControlEndpoint, rootDataEndpoint}
	};
	auto num_nodes = hostsPlacement.numHosts;
	endpoints.reserve(num_nodes + 1);
	auto cursor = 0;

	int level = 0;
	int levelFilled = 0;
	int levelCap = 1;

	log("waiting for %d nodes to connect...\n", num_nodes);
	for (int seen = 0; seen < num_nodes; seen++) {
		// Receive topology request from new node
		{ auto requestReader = net::recv_MessageReader(dealer_socket, zmq::recv_flags::none);
			auto topologyRequest = requestReader.getRoot<TopologyRequest>();

			// Register topology information
			endpoints.emplace_back(topologyRequest.getNodeControlEndpoint().cStr(), topologyRequest.getNodeDataEndpoint().cStr());
		}

		// Determine the number of children of current node
		auto const remain = num_nodes - (seen + 1);
		levelFilled++;
		auto const rightFill = levelCap - levelFilled;
		auto const leftFill = (levelFilled - 1) * fanout;
		auto const num_children = std::min(fanout, std::max(0, remain - (rightFill + leftFill)));

		// Create topology response
		{ auto builder = capnp::MallocMessageBuilder{};
			auto topologyResponse = builder.initRoot<TopologyResponse>();

			auto const& [parentControlEndpoint, parentDataEndpoint] = endpoints[cursor];
			topologyResponse.setParentControlEndpoint(parentControlEndpoint);
			topologyResponse.setParentDataEndpoint(parentDataEndpoint);
			topologyResponse.setNumChildren(num_children);

			// Send topology response
			net::send_MessageBuilder(dealer_socket, builder);
		}

		// Advance to next node
		if (seen % fanout == 0) {
			cursor++;
		}
		if (levelFilled == (int)pow(fanout, level)) {
			level++;
			levelFilled = 0;
			levelCap = (int)pow(fanout, level);
		}
		log("%d connected\n", seen);
	}
}

static void startPackedTree(zmq::context_t& ctx, net::TcpDownstreamLink& rootLink, int app_id, int session_id, int manifest_id, int fanout,
	char const* const* leaf_argv, std::string const& commnodePath, cti_hostsList_t const& hostsPlacement)
{
	// Start topology socket to send its connection info to commnode daemon
	auto dealer_socket = zmq::socket_t{ctx, zmq::socket_type::rep};
	dealer_socket.bind("tcp://*:*");
	auto const dealerEndpoint = net::replace_endpoint_host(dealer_socket.get(zmq::sockopt::last_endpoint), cti_getHostname());
	log("topology dealer listening on %s\n", dealerEndpoint.c_str());

	cti_addManifestBinary(manifest_id, commnodePath.c_str());

	// Set up commnode daemon arguments with topology connection info and leaf process arguments
	{ auto commnodeArgv = cti::OutgoingArgv<CommnodeArgv>{commnodePath};
		commnodeArgv.add(CommnodeArgv::DealerEndpoint, dealerEndpoint);
		commnodeArgv.add(CommnodeArgv::Packed);

		// Add provided leaf arguments
		for (auto argptr = leaf_argv; *argptr != nullptr; argptr++) {
			commnodeArgv.add(cti::Argv::Argument{*argptr});
		}

		// Launch commnode daemon
		if (cti_execToolDaemon(manifest_id, commnodePath.c_str(), commnodeArgv.get() + 1, nullptr)) {
			throw std::runtime_error(cti_error_str());
		}
	}

	// Set up fanout topology and wait for all children to connect
	auto const [rootControlEndpoint, rootDataEndpoint] = formatEndpoints(rootLink, cti_getHostname());
	auto endpoints = std::vector<std::tuple<std::string, std::string>> {
		{rootControlEndpoint, rootDataEndpoint}
	};
	auto num_nodes = cti_getNumAppPEs(app_id);
	endpoints.reserve(num_nodes + 1);
	auto cursor = 0;

	int level = 0;
	int levelFilled = 0;
	int levelCap = 1;

	for (int seen = 0; seen < num_nodes; seen++) {
		// Receive topology request from new node
		{ auto requestReader = net::recv_MessageReader(dealer_socket, zmq::recv_flags::none);
			auto topologyRequest = requestReader.getRoot<TopologyRequest>();

			// Register topology information
			endpoints.emplace_back(topologyRequest.getNodeControlEndpoint().cStr(), topologyRequest.getNodeDataEndpoint().cStr());
		}

		// Determine the number of children of current node
		auto const remain = num_nodes - (seen + 1);
		levelFilled++;
		auto const rightFill = levelCap - levelFilled;
		auto const leftFill = (levelFilled - 1) * fanout;
		auto const num_children = std::min(fanout, std::max(0, remain - (rightFill + leftFill)));

		// Create topology response
		{ auto builder = capnp::MallocMessageBuilder{};
			auto topologyResponse = builder.initRoot<TopologyResponse>();

			auto const& [parentControlEndpoint, parentDataEndpoint] = endpoints[cursor];
			topologyResponse.setParentControlEndpoint(parentControlEndpoint);
			topologyResponse.setParentDataEndpoint(parentDataEndpoint);
			topologyResponse.setNumChildren(num_children);

			// Send topology response
			net::send_MessageBuilder(dealer_socket, builder);
		}

		// Advance to next node
		if (seen % fanout == 0) {
			cursor++;
		}
		if (levelFilled == (int)pow(fanout, level)) {
			level++;
			levelFilled = 0;
			levelCap = (int)pow(fanout, level);
		}
		log("%d connected\n", seen);
	}
}

static void startLayeredTree(zmq::context_t& ctx, net::TcpDownstreamLink& rootLink, int app_id, int session_id, int manifest_id, int fanout,
	char const* const* leaf_argv, std::string const& commnodePath, cti_hostsList_t const& hostsPlacement)
{
	auto const num_levels = (hostsPlacement.numHosts > 1)
		? (int)(log(hostsPlacement.numHosts - 1) / log(fanout)) + 2
		: 1;
	log("fanout %d, %d hosts, %d levels\n", fanout, hostsPlacement.numHosts, num_levels);

	cti_addManifestBinary(manifest_id, commnodePath.c_str());

	auto nodeHostnameMap = std::unordered_map<int, std::string>{};
	auto hostnameNodeMap = std::unordered_map<std::string, int>{};
	auto nodeParentMap = std::unordered_map<int, int>{};
	auto nodeEndpointsMap = std::unordered_map<int, std::tuple<std::string, std::string>>{};
	auto nodeNumChildrenMap = std::unordered_map<int, int>{};
	auto nodeNumSubchildrenMap = std::unordered_map<int, int>{};

	auto const [rootControlEndpoint, rootDataEndpoint] = formatEndpoints(rootLink, cti_getHostname());

	// Initialize tree level arrays
	auto nodeLevels = std::vector<std::vector<int>>{};
	nodeLevels.reserve(num_levels);
	for (int i = 0; i < num_levels; i++) {
		nodeLevels.emplace_back(std::vector<int>{});
	}

	// Set root node connection information
	nodeHostnameMap[0] = hostsPlacement.hosts[0].hostname;
	hostnameNodeMap[hostsPlacement.hosts[0].hostname] = 0;
	nodeEndpointsMap.emplace(-1, std::make_tuple(rootControlEndpoint, rootDataEndpoint));
	nodeParentMap[0] = -1;
	nodeLevels[0].emplace_back(0);

	// Build fanout tree
	{ int num_nodes = hostsPlacement.numHosts;
		// First level will have `fanout` nodes and node 0 as its parent
		int cursor = 0;
		int level = 1;
		size_t level_size = fanout;

		// Assign parent nodes for every node ID in sequence
		for (int i = 1; i < num_nodes; i++) {
			nodeHostnameMap[i] = hostsPlacement.hosts[i].hostname;
			hostnameNodeMap[hostsPlacement.hosts[i].hostname] = i;

			nodeNumChildrenMap[cursor]++;
			nodeParentMap[i] = cursor;
			nodeLevels[level].emplace_back(i);

			// Advance to next node if parent full
			if (nodeNumChildrenMap[cursor] == fanout) {
				cursor++;
			}
			// Advance to next level if level full
			if (nodeLevels[level].size() == level_size) {
				level++;
				level_size *= fanout;
			}
		}
	}

	// Count total number of children in each node's subtrees
	for (int i = num_levels - 1; i >= 0; i--) {
		for (int j = 0; j < (int)nodeLevels[i].size(); j++) {
			int node = nodeLevels[i][j];
			int parent_node = nodeParentMap[node];
			nodeNumSubchildrenMap[parent_node] += nodeNumSubchildrenMap[node] + 1;
		}
	}

	for (int i = 0; i < num_levels; i++) {
		log("launch level %d\n", i);

		// Create set of parent nodes for this level
		auto parentNodes = std::set<int>{};
		for (int j = 0; j < (int)nodeLevels[i].size(); j++) {
			int node = nodeLevels[i][j];
			int parent_node = nodeParentMap[node];
			parentNodes.emplace(parent_node);
		}

		// Map unique index back into parent node set
		auto parentNodeIndices = std::unordered_map<int, int>{};
		{ int parent_node_index = 0;
			for (auto&& parent_node : parentNodes) {
				parentNodeIndices[parent_node] = parent_node_index;
				parent_node_index++;
			}
		}

		// Serialize node topology data
		auto nodeConnectionStream = std::ostringstream{};
		for (int j = 0; j < (int)nodeLevels[i].size(); j++) {
			int node = nodeLevels[i][j];
			int parent_node = nodeParentMap[node];

			nodeConnectionStream << nodeHostnameMap[node]
				<< " " << parentNodeIndices[parent_node]
				<< " " << nodeNumChildrenMap[node]
				<< " " << nodeNumSubchildrenMap[node]
				<< ",";
		}

		// Separate topology information from connection data
		// Replace last character "," of stream with separator ";"
		nodeConnectionStream.seekp(-1, std::ios_base::end);
		nodeConnectionStream << ";";

		// Serialize the connection information for each parent node
		for (auto&& parent_node : parentNodes) {
			assert(nodeEndpointsMap.find(parent_node) != nodeEndpointsMap.end());
			auto [parentControlEndpoint, parentDataEndpoint] = nodeEndpointsMap[parent_node];

			nodeConnectionStream << std::move(parentControlEndpoint)
				<< " " << std::move(parentDataEndpoint)
				<< ",";
		}

		// Create serialized string argument
		auto nodeConnectionArg = nodeConnectionStream.str();

		// Remove final separator
		nodeConnectionArg.pop_back();

		// Set up commnode daemon arguments with topology connection info and leaf process arguments
		auto commnodeArgv = cti::OutgoingArgv<CommnodeArgv>{commnodePath};
		commnodeArgv.add(CommnodeArgv::NodeConnectionMap, std::move(nodeConnectionArg));

		// Add provided leaf arguments
		for (auto argptr = leaf_argv; *argptr != nullptr; argptr++) {
			commnodeArgv.add(cti::Argv::Argument{*argptr});
		}

		// Launch commnode daemon
		if (cti_execToolDaemon(manifest_id, commnodePath.c_str(), commnodeArgv.get() + 1, nullptr)) {
			throw std::runtime_error(cti_error_str());
		}

		// Wait for an incoming topology message from each network child
		log("waiting for %lu topology messages\n", nodeLevels[i].size());
		for (int j = 0; j < (int)nodeLevels[i].size(); j++) {
			auto reader = net::recv_MessageReader(rootLink.data_socket);
			auto response = reader.getRoot<TopologyRequest>();
			auto const node = hostnameNodeMap[response.getNodeHostname()];
			log("node %2d %s register %s %s\n", node, response.getNodeHostname().cStr(), response.getNodeControlEndpoint().cStr(), response.getNodeDataEndpoint().cStr());
			nodeEndpointsMap.emplace(node, std::make_tuple(response.getNodeControlEndpoint(), response.getNodeDataEndpoint()));
		}

		manifest_id = cti_createManifest(session_id);
	}

	// Send leaf start command
	log("layered connection completed, sending leaf start message\n");
	{ auto builder = capnp::MallocMessageBuilder{};
		auto command = builder.initRoot<NetworkCommand>();
		command.setStartLeaves();

		net::send_MessageBuilder(rootLink.control_socket, builder);
	}
}

// Common frontend implementation

zmqnet::Frontend::Frontend(int const app_id, long int const session_id, long int const manifest_id,
	char const* const* leaf_argv, zmqnet::Topology const& topology)
	: m_ctx{}
	, m_downstream{std::make_unique<net::TcpDownstreamLink>(m_ctx)}
{
	auto const start_ms = get_milliseconds();

	// Find commnode path
	auto const commnodePath = std::string{LIBZMQNET_PREFIX} + "/libexec/commnode";

	// Fetch node layout from CTI for network topology
	auto hostsPlacement = std::unique_ptr<cti_hostsList_t, decltype(&cti_destroyHostsList)>
		{ cti_getAppHostsPlacement(app_id)
		, cti_destroyHostsList
	};

	auto const startup_ms = get_milliseconds();
	log("launch started: %5ldms\n", startup_ms - start_ms);

	// Launch tree
	if (topology.launch == Topology::Fcfs) {
		startFcfsTree(m_ctx, *m_downstream, app_id, session_id, manifest_id, topology.fanout, leaf_argv, commnodePath, *hostsPlacement);
	} else if (topology.launch == Topology::Layered) {
		startLayeredTree(m_ctx, *m_downstream, app_id, session_id, manifest_id, topology.fanout, leaf_argv, commnodePath, *hostsPlacement);
	} else if (topology.launch == Topology::Packed) {
		startPackedTree(m_ctx, *m_downstream, app_id, session_id, manifest_id, topology.fanout, leaf_argv, commnodePath, *hostsPlacement);
	}

	auto const launch_ms = get_milliseconds();
	log("all commnodes connected: %5ldms\n", launch_ms - startup_ms);

	// Wait for all children to send ready over data socket
	log("waiting for ready report\n");
	auto reader = net::recv_MessageReader(m_downstream->data_socket);
	auto response = reader.getRoot<NetworkResponse>();
	assert(response.isReadyAck());

	auto const leaf_ms = get_milliseconds();
	log("all leaves connected: %5ldms\n", leaf_ms - launch_ms);
}

zmqnet::Frontend::~Frontend()
{
	// Generate destructor for forward-declared net::TcpDownstreamLink
}

void zmqnet::Frontend::createStream(zmqnet::StreamId const& streamId, std::string const& filterLibraryName, std::string const& filterFunctionName)
{
	// Create filter create command message
	auto builder = capnp::MallocMessageBuilder{};
	auto streamCommand = builder.initRoot<NetworkCommand>().initNewStream();
	streamCommand.setNewStreamId(streamId);
	streamCommand.setFilterLibraryName(filterLibraryName);
	streamCommand.setFilterFunctionName(filterFunctionName);

	// Send filter create command
	log("registering filter %s function %s with id %d\n", filterLibraryName.c_str(), filterFunctionName.c_str(), streamId);
	net::send_MessageBuilder(m_downstream->control_socket, builder);
}

void zmqnet::Frontend::sendCommand(zmqnet::StreamId const& streamId, std::string const& command)
{
	// Create user command
	auto builder = capnp::MallocMessageBuilder{};
	auto userCommand = builder.initRoot<NetworkCommand>().initUserCommand();
	userCommand.setStreamId(streamId);
	userCommand.setContent(command);

	// Send user command
	log("user command %s on stream %d\n", command.c_str(), streamId);
	net::send_MessageBuilder(m_downstream->control_socket, builder);
}


void zmqnet::Frontend::sendExit()
{
	// Create exit command
	auto builder = capnp::MallocMessageBuilder{};
	auto command = builder.initRoot<NetworkCommand>();
	command.setExit();

	// Send exit command
	net::send_MessageBuilder(m_downstream->control_socket, builder);
}

// Synchronous frontend implementation

void zmqnet::SyncFrontend::registerStream(zmqnet::StreamId const& streamId, std::string const& filterLibraryName, std::string const& filterFunctionName)
{
	auto const start_ms = get_milliseconds();

	// Send filter create command
	createStream(streamId, filterLibraryName, filterFunctionName);

	// Wait for acknowledgement
	auto reader = net::recv_MessageReader(m_downstream->data_socket);
	auto response = reader.getRoot<NetworkResponse>();
	assert(response.isStreamAck() && (response.getStreamAck().getStreamId() == streamId));

	auto const filter_ms = get_milliseconds() - start_ms;
	log("filter registered RT: %5ldms\n", filter_ms);
}


zmqnet::ByteArray zmqnet::SyncFrontend::runCommand(zmqnet::StreamId const& streamId, std::string const& command)
{
	auto const start_ms = get_milliseconds();

	// Send user command
	sendCommand(streamId, command);

	// Receive aggregated result
	log("receiving results...\n");
	auto reader = net::recv_MessageReader(m_downstream->data_socket);
	auto response = reader.getRoot<NetworkResponse>();
	assert(response.isUserResult() && (response.getUserResult().getStreamId() == streamId));

	auto const result_ms = get_milliseconds();
	log("result RT: %5ldms\n", result_ms - start_ms);

	return kj::heapArray<kj::byte>(response.getUserResult().getContent().asBytes());
}

void zmqnet::SyncFrontend::runExit()
{
	auto const start_ms = get_milliseconds();
	log("sending exit command\n");

	// Send exit command
	sendExit();

	// Wait for exit response
	auto reader = net::recv_MessageReader(m_downstream->data_socket);
	auto response = reader.getRoot<NetworkResponse>();
	assert(response.isExitAck());

	log("exit completed\n");

	auto const exit_ms = get_milliseconds();
	log("exit RT: %5ldms\n", exit_ms - start_ms);
}

static bool callback_worker(zmq::context_t& ctx, zmqnet::StreamId streamId, zmqnet::AsyncFrontend::CallbackFunc&& callbackFunc)
{
	// Open task socket to receive this stream's input from downstream data task
	auto callback_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	{ auto const callbackEndpoint = "inproc://stream-" + std::to_string(streamId);
		log("%d: connecting to callback endpoint %s\n", streamId, callbackEndpoint.c_str());
		callback_socket.connect(callbackEndpoint);
	}

#if 0
	// Get forwarded stream create ack
	{ log("%d: waiting for stream create ack\n", streamId);
		auto reader = net::recv_MessageReader(callback_socket);
		auto response = reader.getRoot<NetworkResponse>();
		assert(response.isStreamAck() && (response.getStreamAck().getStreamId() == streamId));
	}
#endif

	while (true) {

		// Receive result from task socket
		log("%d: waiting for result from callback socket\n", streamId);
		auto reader = net::recv_MessageReader(callback_socket);
		auto response = reader.getRoot<NetworkResponse>();

		// If exit message, return
		if (response.isExitAck()) {
			return true;

		// Ignore possible race stream ack
		} else if (response.isStreamAck()) {
			continue;

		} else {
			// Check stream ID and user result
			assert(response.isUserResult() && (response.getUserResult().getStreamId() == streamId));

			// Run user-provided callback. TODO: pass ArrayPtr instead of new heap array
			callbackFunc(kj::heapArray(response.getUserResult().getContent().asBytes()));
		}
	}
}

static bool callback_distributor(zmq::context_t& ctx, std::string const& syncEndpoint, net::TcpDownstreamLink& downstream, std::unordered_map<zmqnet::StreamId, zmqnet::AsyncFrontend::CallbackFunc>& callbacks, std::unordered_map<zmqnet::StreamId, std::future<bool>>& callbackWorkers, std::unordered_map<zmqnet::StreamId, zmq::socket_t>& callbackSockets)
{
	// Set up sync socket to coordinate stream launches with main thread
	auto sync_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	sync_socket.connect(syncEndpoint);

	while (true) {
		// Receive aggregated result
		log("dist: receiving results...\n");
		auto result = net::recv_message(downstream.data_socket);
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

#if 0
		// If new stream, create worker thread
		if (response.isStreamAck()) {
			log("dist: streamAck\n"); // Needs to be sent back to frontend to finish async registerStream somehow

			// Find callback corresponding to this stream
			auto const streamId = response.getStreamAck().getStreamId();

			// Get callback function object and launch worker thread
			{ auto streamIdCallbackPair = callbacks.find(streamId);
				if (streamIdCallbackPair == callbacks.end()) {
					throw std::runtime_error("no asynchronous callback registered for stream ID " + std::to_string(streamId));
				}

				// Launch worker thread
				callbackWorkers.emplace(streamId, std::async(std::launch::async,
					[](zmq::context_t& ctx, zmqnet::StreamId streamId, zmqnet::AsyncFrontend::CallbackFunc&& callbackFunc) {
						return callback_worker(ctx, streamId, std::move(callbackFunc));
					}, std::ref(ctx), streamId, std::move(streamIdCallbackPair->second)));

				// Erase from pending callback map
				callbacks.erase(streamIdCallbackPair);
			}

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

			continue;
		}
#else
		// Notify main thread that entire network has started stream
		if (response.isStreamAck()) {
			sync_socket.send(result, zmq::send_flags::none);

			continue;
		}
#endif

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

	// Wait for stream workers
	for (auto&& [streamId, callbackWorker] : callbackWorkers) {
		(void)callbackWorker.get();
	}

	return true;
}

zmqnet::AsyncFrontend::AsyncFrontend(int const app_id, long int const session_id, long int const manifest_id,
	char const* const* leaf_argv, zmqnet::Topology const& topology)
	: Frontend{app_id, session_id, manifest_id, leaf_argv, topology}
	, m_ctx{}
	, m_callbacks{}
	, m_callbackWorkers{}
	, m_callbackSockets{}
	, m_distributor_sync_socket{m_ctx, zmq::socket_type::pull}
	, m_distributorSyncSocketEndpoint{"inproc://frontend-" + std::to_string(app_id)}
	, m_callbackDistributor{}
{
	// Bind callback distributor task sync socket
	m_distributor_sync_socket.bind(m_distributorSyncSocketEndpoint);

	// Launch callback distributor task
	m_callbackDistributor = std::async(std::launch::async,
		callback_distributor, std::ref(m_ctx), m_distributorSyncSocketEndpoint, std::ref(*m_downstream),
		std::ref(m_callbacks), std::ref(m_callbackWorkers), std::ref(m_callbackSockets));
}

void zmqnet::AsyncFrontend::registerStream(zmqnet::StreamId const& streamId, std::string const& filterLibraryName, std::string const& filterFunctionName,
	zmqnet::AsyncFrontend::CallbackFunc callbackFunc)
{
	// Ensure no callback corresponding to this stream
	auto streamIdCallbackPair = m_callbacks.find(streamId);
	if (streamIdCallbackPair != m_callbacks.end()) {
		throw std::runtime_error("asynchronous callback already registered for stream ID " + std::to_string(streamId));
	}

#if 0
	auto const start_ms = get_milliseconds();

	// Send filter create command
	createStream(streamId, filterLibraryName, filterFunctionName);

	// Wait for acknowledgement
	auto reader = net::recv_MessageReader(m_downstream->data_socket);
	auto response = reader.getRoot<NetworkResponse>();
	assert(response.isStreamAck() && (response.getStreamAck().getStreamId() == streamId));

	auto const filter_ms = get_milliseconds() - start_ms;
	log("filter registered RT: %5ldms\n", filter_ms);

	// Register callback for stream
	m_callbacks.emplace_hint(streamIdCallbackPair, streamId, std::move(callbackFunc));
#else

	// Launch worker thread
	m_callbackWorkers.emplace(streamId, std::async(std::launch::async,
		[](zmq::context_t& ctx, zmqnet::StreamId streamId, zmqnet::AsyncFrontend::CallbackFunc&& callbackFunc) {
			return callback_worker(ctx, streamId, std::move(callbackFunc));
		}, std::ref(m_ctx), streamId, std::move(callbackFunc)));

	// Launch callback socket (endpoint is based on stream ID)
	{ auto callback_socket = zmq::socket_t{m_ctx, zmq::socket_type::push};
		auto callbackEndpoint = "inproc://stream-" + std::to_string(streamId);
		log("creating new task socket %s\n", callbackEndpoint.c_str());
		callback_socket.bind(callbackEndpoint);

		// Store in stream callback socket map
		m_callbackSockets.emplace(streamId, std::move(callback_socket));
	}

	// Send filter create command
	createStream(streamId, filterLibraryName, filterFunctionName);

	// Wait for callback processor to receive stream acknowledgement
	{ auto reader = net::recv_MessageReader(m_distributor_sync_socket);
		auto response = reader.getRoot<NetworkResponse>();
		assert(response.isStreamAck() && (response.getStreamAck().getStreamId() == streamId));
	}
#endif
}

void zmqnet::AsyncFrontend::sendCommand(zmqnet::StreamId const& streamId, std::string const& command)
{
#if 0
	// Find callback corresponding to this stream
	auto streamIdCallbackPair = m_callbacks.find(streamId);
	if (streamIdCallbackPair == m_callbacks.end()) {
		throw std::runtime_error("no asynchronous callback registered for stream ID " + std::to_string(streamId));
	}
#else
	// Ensure stream registered
	if (m_callbackSockets.find(streamId) == m_callbackSockets.end()) {
		throw std::runtime_error("no asynchronous callback registered for stream ID " + std::to_string(streamId));
	}
#endif

	// Send user command
	Frontend::sendCommand(streamId, command);
}

void zmqnet::AsyncFrontend::runExit()
{
	auto const start_ms = get_milliseconds();
	log("sending exit command\n");

	// Send exit command
	sendExit();

	// Wait for callback processor to exit
	(void)m_callbackDistributor.get();

	log("exit completed\n");

	auto const exit_ms = get_milliseconds();
	log("exit RT: %5ldms\n", exit_ms - start_ms);
}

zmqnet::Backend::Backend(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int rank)
	: m_ctx{}
	, m_upstream{std::make_unique<net::UpstreamLink>(m_ctx, upstreamControlEndpoint, upstreamDataEndpoint)}
	, m_rank{rank}
{
	// Notify parent commnode that setup is completed
	log("rank %d: notifying parent ready from leaf node\n", m_rank);
	auto builder = capnp::MallocMessageBuilder{};
	auto response = builder.initRoot<NetworkResponse>();
	response.setReadyAck();

	// Send acknowledgement
	net::send_MessageBuilder(m_upstream->data_socket, builder);
}

 zmqnet::Backend::~Backend()
 {
 	// Generate destructor for forward-declared net::UpstreamLink
 }

std::tuple<zmqnet::StreamId, std::string> zmqnet::Backend::nextCommand()
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

			return std::make_tuple(-1, std::string{});

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
			return std::make_tuple(userCommand.getStreamId(), std::string{userCommand.getContent().cStr()});
		}
	}
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
