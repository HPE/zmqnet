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

static void log(const char* format, ...)
{
	static const auto start_ms = get_milliseconds();
	va_list argptr;
	va_start(argptr, format);
	fprintf(stderr, "[%06ld] ", get_milliseconds() - start_ms);
	vfprintf(stderr, format, argptr);
	va_end(argptr);
}

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
	auto endpoints = std::vector<std::tuple<std::string, std::string>>{
		{rootControlEndpoint, rootDataEndpoint}
	};
	auto num_nodes = hostsPlacement.numHosts;
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

static void startFanoutTree(zmq::context_t& ctx, TcpDownstreamLink& rootLink, int app_id, int session_id, int manifest_id, int fanout,
	char const* const* leaf_argv, std::string const& commnodePath, cti_hostsList_t const& hostsPlacement)
{
	auto const num_levels = (hostsPlacement.numHosts > 1)
		? int{log(hostsPlacement.numHosts - 1) / log(fanout)} + 2
		: 1;
	log("fanout %d, %d hosts, %d levels\n", fanout, hostsPlacement.numHosts, num_levels);

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
		int cursor = 0;
		int level = 1;

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
			if (nodeLevels[level].size() == (int)pow(fanout, level)) {
				level++;
			}
		}
	}

	// Count total number of children in each node's subtrees
	for (int i = num_levels - 1; i >= 0; i--) {
		for (int j = 0; j < nodeLevels[i].size(); j++) {
			int node = nodeLevels[i][j];
			int parent_node = nodeParentMap[node];
			nodeNumSubchildrenMap[parent_node] += nodeNumSubchildrenMap[node] + 1;
		}
	}

	for (int i = 0; i < num_levels; i++) {
		log("launch level %d\n", i);

		// Create set of parent nodes for this level
		auto parentNodes = std::set<int>{};
		for (int j = 0; j < nodeLevels[i].size(); j++) {
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
		auto nodeConnectionArg = std::string{};
		for (int j = 0; j < nodeLevels[i].size(); j++) {
			int node = nodeLevels[i][j];
			int parent_node = nodeParentMap[node];

			nodeConnectionArg += nodeHostnameMap[node]
				+ " " + std::to_string(parentNodeIndices[parent_node])
				+ " " + std::to_string(nodeNumChildrenMap[node])
				+ " " + std::to_string(nodeNumSubchildrenMap[node])
				+ ",";
		}

		// Separate topology information from connection data
		nodeConnectionArg.back() = ';';

		// Serialize the connection information for each parent node
		for (auto&& parent_node : parentNodes) {
			assert(nodeEndpointsMap.find(parent_node) != nodeEndpointsMap.end());
			auto [parentControlEndpoint, parentDataEndpoint] = nodeEndpointsMap[parent_node];

			nodeConnectionArg += std::move(parentControlEndpoint)
				+ " " + std::move(parentDataEndpoint)
				+ ",";
		}

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

		// Wait for incoming topology messages
		for (int j = 0; j < nodeLevels[i].size(); j++) {
			auto reader = recv_MessageReader(rootLink.data_socket);
			auto response = reader.getRoot<TopologyRequest>();
			auto const node = hostnameNodeMap[response.getNodeHostname()];
			log("node %2d %s register %s %s\n", node, response.getNodeHostname().cStr(), response.getNodeControlEndpoint().cStr(), response.getNodeDataEndpoint().cStr());
			nodeEndpointsMap.emplace(node, std::make_tuple(response.getNodeControlEndpoint(), response.getNodeDataEndpoint()));
		}

		manifest_id = cti_createManifest(session_id);
	}
}

Frontend::Frontend(zmq::context_t& ctx, int const app_id, int const session_id, int const manifest_id, int const fanout,
	char const* const* leaf_argv)
	: m_downstream{ctx}
{
	// Find commnode path
	auto const commnodePath = std::string{LIBZMQNET_PREFIX} + "/libexec/commnode";

	// Fetch node layout from CTI for network topology
	auto hostsPlacement = std::unique_ptr<cti_hostsList_t, decltype(&cti_destroyHostsList)>
		{ cti_getAppHostsPlacement(app_id)
		, cti_destroyHostsList
	};

	// Launch tree
	startFanoutTree(ctx, m_downstream, app_id, session_id, manifest_id, fanout, leaf_argv, commnodePath, *hostsPlacement);

	// Send leaf start command
	log("connection completed, sending leaf start message\n");
	{ auto builder = capnp::MallocMessageBuilder{};
		auto command = builder.initRoot<NetworkCommand>();
		command.setStartLeaves();

		send_MessageBuilder(m_downstream.control_socket, builder);
	}

	// Wait for all children to send ready over data socket
	log("waiting for ready report\n");
	auto reader = recv_MessageReader(m_downstream.data_socket);
	auto response = reader.getRoot<NetworkResponse>();
	assert(response.isReadyAck());
}

void Frontend::registerStream(StreamId const& streamId, std::string const& filterName)
{
	// Create filter create command message
	auto builder = capnp::MallocMessageBuilder{};
	auto filterCommand = builder.initRoot<NetworkCommand>().initNewStream();
	filterCommand.setNewStreamId(streamId);
	filterCommand.setFilterName(filterName);

	// Send filter create command
	log("registering filter %s with id %d\n", filterName.c_str(), streamId);
	send_MessageBuilder(m_downstream.control_socket, builder);

	// Wait for acknowledgement
	auto reader = recv_MessageReader(m_downstream.data_socket);
	auto response = reader.getRoot<NetworkResponse>();
	assert(response.isStreamAck() && (response.getStreamAck().getStreamId() == streamId));
}

std::string Frontend::runCommand(StreamId const& streamId, std::string const& command)
{
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

	auto result = std::string{response.getUserResult().getContent().cStr()};
	log("result: %s\n", result.c_str());

	return result;
}

void Frontend::runExit()
{
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
}

Backend::Backend(zmq::context_t& ctx, std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int rank)
	: m_upstream{ctx, upstreamControlEndpoint, upstreamDataEndpoint}
	, m_rank{rank}
{
	// Notify parent commnode that setup is completed
	fprintf(stderr, "rank %d: notifying parent ready from leaf node\n", m_rank);
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
		fprintf(stderr, "rank %d: waiting for next command...\n", m_rank);
		auto reader = recv_MessageReader(m_upstream.control_socket);
		auto command = reader.getRoot<NetworkCommand>();

		// Detect exit case
		if (command.isExit()) {
			fprintf(stderr, "rank %d: got exit\n", m_rank);

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
			fprintf(stderr, "rank %d: filter response\n", m_rank);
			auto builder = capnp::MallocMessageBuilder{};
			auto response = builder.initRoot<NetworkResponse>();
			auto streamAck = response.initStreamAck();
			streamAck.setStreamId(command.getNewStream().getNewStreamId());

			send_MessageBuilder(m_upstream.data_socket, builder);

			continue;

		} else if (command.isStartLeaves()) {
			fprintf(stderr, "rank %d: unexpected: startLeaves\n", m_rank);
			continue;

		// Extract stream ID and user command
		} else {
			fprintf(stderr, "rank %d: user command\n", m_rank);
			assert(command.isUserCommand());
			auto userCommand = command.getUserCommand();
			return std::make_tuple(userCommand.getStreamId(), std::string{userCommand.getContent().cStr()});
		}
	}
}

void Backend::sendResult(StreamId const& streamId, std::string const& result)
{
	// Notify parent commnode of result
	auto builder = capnp::MallocMessageBuilder{};
	auto response = builder.initRoot<NetworkResponse>();
	auto userResult = response.initUserResult();
	userResult.setStreamId(streamId);
	userResult.setContent(result);

	// Send result
	send_MessageBuilder(m_upstream.data_socket, builder);
}
