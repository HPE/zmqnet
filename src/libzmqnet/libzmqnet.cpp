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

#include "zmqnet.hpp"

#include "libzmqnet.hpp"

// Build fanout tree in first-come first-served order. Does not require any precomputing of topology
// tree by the frontend.
static void startFcfsTree(zmq::context_t& ctx, TcpDownstreamLink& rootLink, int app_id, int session_id, int manifest_id, int fanout,
	char const* const* leaf_argv, std::string const& commnodePath, cti_hostsList_t const& hostsPlacement)
{
	// Start topology socket to send its connection info to commnode daemon
	auto dealer_socket = zmq::socket_t{ctx, zmq::socket_type::rep};
	dealer_socket.bind("tcp://*:*");
	auto const dealerEndpoint = replace_endpoint_host(dealer_socket.get(zmq::sockopt::last_endpoint), cti_getHostname());
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
		{ auto requestReader = recv_MessageReader(dealer_socket, zmq::recv_flags::none);
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
			send_MessageBuilder(dealer_socket, builder);
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

static void startPackedTree(zmq::context_t& ctx, TcpDownstreamLink& rootLink, int app_id, int session_id, int manifest_id, int fanout,
	char const* const* leaf_argv, std::string const& commnodePath, cti_hostsList_t const& hostsPlacement)
{
	// Start topology socket to send its connection info to commnode daemon
	auto dealer_socket = zmq::socket_t{ctx, zmq::socket_type::rep};
	dealer_socket.bind("tcp://*:*");
	auto const dealerEndpoint = replace_endpoint_host(dealer_socket.get(zmq::sockopt::last_endpoint), cti_getHostname());
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
		{ auto requestReader = recv_MessageReader(dealer_socket, zmq::recv_flags::none);
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
			send_MessageBuilder(dealer_socket, builder);
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

static void startLayeredTree(zmq::context_t& ctx, TcpDownstreamLink& rootLink, int app_id, int session_id, int manifest_id, int fanout,
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
			auto reader = recv_MessageReader(rootLink.data_socket);
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

		send_MessageBuilder(rootLink.control_socket, builder);
	}
}

Frontend::Frontend(zmq::context_t& ctx, int const app_id, long int const session_id, long int const manifest_id,
	char const* const* leaf_argv, Topology const& topology)
	: m_downstream{ctx}
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
	fprintf(stderr, "launch started: %5ldms\n", startup_ms - start_ms);

	// Launch tree
	if (topology.launch == Topology::Fcfs) {
		startFcfsTree(ctx, m_downstream, app_id, session_id, manifest_id, topology.fanout, leaf_argv, commnodePath, *hostsPlacement);
	} else if (topology.launch == Topology::Layered) {
		startLayeredTree(ctx, m_downstream, app_id, session_id, manifest_id, topology.fanout, leaf_argv, commnodePath, *hostsPlacement);
	} else if (topology.launch == Topology::Packed) {
		startPackedTree(ctx, m_downstream, app_id, session_id, manifest_id, topology.fanout, leaf_argv, commnodePath, *hostsPlacement);
	}

	auto const launch_ms = get_milliseconds();
	fprintf(stderr, "all commnodes connected: %5ldms\n", launch_ms - startup_ms);

	// Wait for all children to send ready over data socket
	log("waiting for ready report\n");
	auto reader = recv_MessageReader(m_downstream.data_socket);
	auto response = reader.getRoot<NetworkResponse>();
	assert(response.isReadyAck());

	auto const leaf_ms = get_milliseconds();
	fprintf(stderr, "all leaves connected: %5ldms\n", leaf_ms - launch_ms);
}

void Frontend::registerStream(StreamId const& streamId, std::string const& filterLibraryName, std::string const& filterFunctionName)
{
	auto const start_ms = get_milliseconds();

	// Create filter create command message
	auto builder = capnp::MallocMessageBuilder{};
	auto streamCommand = builder.initRoot<NetworkCommand>().initNewStream();
	streamCommand.setNewStreamId(streamId);
	streamCommand.setFilterLibraryName(filterLibraryName);
	streamCommand.setFilterFunctionName(filterFunctionName);

	// Send filter create command
	log("registering filter %s function %s with id %d\n", filterLibraryName.c_str(), filterFunctionName.c_str(), streamId);
	send_MessageBuilder(m_downstream.control_socket, builder);

	// Wait for acknowledgement
	auto reader = recv_MessageReader(m_downstream.data_socket);
	auto response = reader.getRoot<NetworkResponse>();
	assert(response.isStreamAck() && (response.getStreamAck().getStreamId() == streamId));

	auto const filter_ms = get_milliseconds() - start_ms;
	fprintf(stderr, "filter registered RT: %5ldms\n", filter_ms);
}

kj::Array<kj::byte> Frontend::runCommand(StreamId const& streamId, std::string const& command)
{
	auto const start_ms = get_milliseconds();

	// Create user command
	auto builder = capnp::MallocMessageBuilder{};
	auto userCommand = builder.initRoot<NetworkCommand>().initUserCommand();
	userCommand.setStreamId(streamId);
	userCommand.setContent(command);

	// Send user command
	log("user command %s on stream %d\n", command.c_str(), streamId);
	send_MessageBuilder(m_downstream.control_socket, builder);

	// Receive aggregated result
	log("receiving results...\n");
	auto reader = recv_MessageReader(m_downstream.data_socket);
	auto response = reader.getRoot<NetworkResponse>();
	assert(response.isUserResult() && (response.getUserResult().getStreamId() == streamId));

	auto const result_ms = get_milliseconds();
	fprintf(stderr, "result RT: %5ldms\n", result_ms - start_ms);

	return kj::heapArray<kj::byte>(response.getUserResult().getContent().asBytes());
}

void Frontend::runExit()
{
	auto const start_ms = get_milliseconds();
	log("sending exit command\n");

	// Create exit command
	auto builder = capnp::MallocMessageBuilder{};
	auto command = builder.initRoot<NetworkCommand>();
	command.setExit();

	// Send exit command
	send_MessageBuilder(m_downstream.control_socket, builder);

	// Wait for exit response
	auto reader = recv_MessageReader(m_downstream.data_socket);
	auto response = reader.getRoot<NetworkResponse>();
	assert(response.isExitAck());

	log("exit completed\n");

	auto const exit_ms = get_milliseconds();
	fprintf(stderr, "exit RT: %5ldms\n", exit_ms - start_ms);
}

Backend::Backend(zmq::context_t& ctx, std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int rank)
	: m_upstream{ctx, upstreamControlEndpoint, upstreamDataEndpoint}
	, m_rank{rank}
{
	// Notify parent commnode that setup is completed
	log("rank %d: notifying parent ready from leaf node\n", m_rank);
	auto builder = capnp::MallocMessageBuilder{};
	auto response = builder.initRoot<NetworkResponse>();
	response.setReadyAck();

	// Send acknowledgement
	send_MessageBuilder(m_upstream.data_socket, builder);
}

std::tuple<StreamId, std::string> Backend::nextCommand()
{
	while (true) {
		// Receive next network command
		log("rank %d: waiting for next command...\n", m_rank);
		auto reader = recv_MessageReader(m_upstream.control_socket);
		auto command = reader.getRoot<NetworkCommand>();

		// Detect exit case
		if (command.isExit()) {
			log("rank %d: got exit\n", m_rank);

			// Send exit acknowledgement to parent
			{ auto builder = capnp::MallocMessageBuilder{};
				auto response = builder.initRoot<NetworkResponse>();
				response.setExitAck();

				send_MessageBuilder(m_upstream.data_socket, builder);
			}
			m_upstream.control_socket.close();
			m_upstream.data_socket.close();

			return std::make_tuple(-1, std::string{});

		// Acknowledge filter case
		} else if (command.isNewStream()) {
			log("rank %d: filter response\n", m_rank);
			auto builder = capnp::MallocMessageBuilder{};
			auto response = builder.initRoot<NetworkResponse>();
			auto streamAck = response.initStreamAck();
			streamAck.setStreamId(command.getNewStream().getNewStreamId());

			send_MessageBuilder(m_upstream.data_socket, builder);

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

void Backend::sendResult(StreamId const& streamId, char const* result_buffer, size_t result_len)
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
	send_MessageBuilder(m_upstream.data_socket, builder);
}
