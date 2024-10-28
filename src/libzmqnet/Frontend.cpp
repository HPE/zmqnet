/*
 * Copyright 2020 Hewlett Packard Enterprise Development LP.
 */

#include <config.h>

#include <cassert>
#include <cmath>

#include <unordered_map>
#include <set>
#include <numeric>
#include <filesystem>

#include <cstdlib>

#include "lrle_bitset_impl.hpp"

#include "common_tools_fe.h"
#include "common_tools_be.h"

#include "argv_defs.hpp"

// Internal helpers and classes
#include "libzmqnet.hpp"

// Launch strategies' implementations
namespace launch
{

// Simulates a blocking recv on `socket`, checking killSwitch every so often.
// If killSwitch is ever true, throws a std::runtime_error.
auto killSwitchRecv(zmq::socket_t& socket, const std::atomic<bool>* killSwitch) {
	while (true) {
		if (killSwitch && *killSwitch) {
			log("Detected killswitch.\n");
			throw std::runtime_error("Launch interrupted");
		}

		// It is assumed that `socket` has had zmq::sockopt::rcvtimeo already set.
		// Otherwise, this is just a run of the mill blocking recv.
		if (auto maybeReader = net::recv_MessageReader_timeout(socket)) {
			return std::move(*maybeReader);
		}
	}
}

// Build fanout tree in first-come first-served order. Does not require any precomputing of topology
// tree by the frontend.
// Return max node ID, assigned node ID map
static auto fcfsTreeImpl(zmq::context_t& ctx, net::TcpDownstreamLink& rootLink, zmqnet::UserCallbacks& uCbs,
	int app_id, int session_id, int manifest_id, int fanout, zmqnet::Auth const& auth,
	char const* const* leaf_argv, char const* const* leaf_env, std::string const& commnodePath,
	cti_hostsList_t const& hostsPlacement, const std::atomic<bool>* killSwitch, bool packed)
{
	auto nodeIdMap = std::unordered_map<std::string, int>{};

	// Start topology socket to send its connection info to commnode daemon
	auto dealer_socket = zmq::socket_t{ctx, zmq::socket_type::rep};

	// Create auth file for commnode
	auto authFile = temp_file();

	// Fill auth file if CURVE encryption enabled
	if (auth == zmqnet::Auth::Curve) {
#if HAVE_ENCRYPTION
		// Generate and set server keys for topology dealer socket
		auto const dealerPub = net::set_curve_server(dealer_socket);

		// Write dealer public key to file
		::fprintf(authFile.get(), "%s\n", dealerPub.c_str());
		authFile.close();

		// Add auth file to manifest
		if (cti_addManifestFile(manifest_id, authFile.path())) {
			throw std::runtime_error("failed to add authfile to manifest: " +
				std::string{cti_error_str()});
		}
#else
		throw std::runtime_error("ZMQNet library was not built with encryption support.\n"
			"If running without encryption is acceptable, set LIBZMQNET_CURVE=0");
#endif
	}

	// Bind topology dealer socket
	net::retry_bind(dealer_socket, net::generate_tcp_endpoint());
	auto hostname = std::unique_ptr<char, decltype(&::free)> {
		cti_getHostname(), ::free};
	if (hostname == nullptr) {
		throw std::runtime_error("failed to detect hostname.\n"
			"Try setting CTI_HOST_ADDRESS to the external address for this node.\n"
			+ std::string{cti_error_str()});
	}
	auto const dealerEndpoint = net::replace_tcp_endpoint_host(
			dealer_socket.get(zmq::sockopt::last_endpoint), hostname.get());
	log("topology dealer listening on %s\n",
		dealerEndpoint.c_str());

	cti_addManifestBinary(manifest_id, commnodePath.c_str());

	// Set up commnode daemon arguments with topology connection info and leaf process arguments
	{ auto commnodeArgv = cti::OutgoingArgv<CommnodeArgv>{commnodePath};
		commnodeArgv.add(CommnodeArgv::DealerEndpoint, dealerEndpoint);

		// Set authentication type argument
		commnodeArgv.add(CommnodeArgv::AuthType, std::to_string(static_cast<int>(auth)));

		// Add keyfile path if CURVE set
		if (auth == zmqnet::Auth::Curve) {

			// Generate path to auth file
			auto fileDir = std::unique_ptr<char, decltype(&::free)> {
				cti_getSessionFileDir(session_id), ::free };
			auto const authFileName = std::string{
				::basename(::strdup(authFile.path()))};
			auto const authFilePath = std::string{fileDir.get()} + "/" + authFileName;
			commnodeArgv.add(CommnodeArgv::AuthFile, authFilePath);
		}

		// in packed mode, one leaf binary will be launched per *rank* instead of per node
		if (packed) commnodeArgv.add(CommnodeArgv::Packed);

		// Add provided leaf arguments
		for (auto argptr = leaf_argv; *argptr != nullptr; argptr++) {
			commnodeArgv.add(cti::Argv::Argument{*argptr});
		}

		// Before we start anything crazy...
		if (killSwitch && *killSwitch) throw std::runtime_error("Launch interrupted");

		// Launch commnode daemon
		if (cti_execToolDaemon(manifest_id, commnodePath.c_str(), commnodeArgv.get() + 1, leaf_env)) {
			throw std::runtime_error(cti_error_str());
		}
	}

	// Set up fanout topology and wait for all children to connect
	auto const [rootControlEndpoint, rootDataEndpoint] = formatEndpoints(rootLink, hostname.get());
	auto endpoints = std::vector<std::tuple<std::string, std::string, std::string>> {
		{rootControlEndpoint, rootDataEndpoint, rootLink.getPublicKey()}
	};
	auto num_nodes = hostsPlacement.numHosts;
	endpoints.reserve(num_nodes + 1);
	auto next_node_id = int{0};
	auto cursor = int{0};

	int level = 0;
	int levelFilled = 0;
	int levelCap = 1;

	log("waiting for %d nodes to connect...\n", num_nodes);
	for (int seen = 0; seen < num_nodes; seen++) {
		auto const node_id = next_node_id++;

		// Receive topology request from new node
		try {
			// this timeout is not absolute, it's the interval that killSwitch is checked in killSwitchRecv
			dealer_socket.set(zmq::sockopt::rcvtimeo, 3000);
			// throws if killSwitch is flipped to true
			auto requestReader = killSwitchRecv(dealer_socket, killSwitch);
			auto topologyRequest = requestReader.getRoot<TopologyRequest>();

			// Assign node ID
			nodeIdMap[topologyRequest.getNodeHostname().cStr()] = node_id;

			log("topology request: %s %s %s %s -> %d\n",
				topologyRequest.getNodeHostname().cStr(),
				topologyRequest.getNodeControlEndpoint().cStr(),
				topologyRequest.getNodeDataEndpoint().cStr(),
				topologyRequest.getNodePublicKey().cStr(),
				node_id);

			// Register topology information
			endpoints.emplace_back(
				topologyRequest.getNodeControlEndpoint().cStr(),
				topologyRequest.getNodeDataEndpoint().cStr(),
				topologyRequest.getNodePublicKey().cStr());
		} catch (std::runtime_error& e) {
			// Killswitch was flipped.
			// If any commnodes made it to their main loop, shut them down.
			log("Sending kill to started commnodes.\n");
			auto builder = capnp::MallocMessageBuilder{};
			auto killSignal = builder.initRoot<NetworkCommand>();
			killSignal.setKill();
			net::send_MessageBuilder(rootLink.control_socket, net::communicator_ids::AllNodes, builder);
			throw;
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

			auto const& [parentControlEndpoint, parentDataEndpoint, parentPublicKey] = endpoints[cursor];
			topologyResponse.setNodeId(node_id);
			topologyResponse.setParentControlEndpoint(parentControlEndpoint);
			topologyResponse.setParentDataEndpoint(parentDataEndpoint);
			topologyResponse.setParentPublicKey(parentPublicKey);
			topologyResponse.setNumChildren(num_children);

			log("topology response: %s %s %s\n",
				parentControlEndpoint.c_str(),
				parentDataEndpoint.c_str(),
				parentPublicKey.c_str());

			// Send topology response
			net::send_MessageBuilder(dealer_socket, net::communicator_ids::AllNodes, builder);
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

		uCbs.invokeCallback<zmqnet::Event::InitialConnection>(seen);
	}

	if (nodeIdMap.empty()) {
		throw std::runtime_error("no nodes in network");
	}

	assert(next_node_id > 0);
	auto const max_node_id = next_node_id - 1;

	return std::make_tuple(max_node_id, std::move(nodeIdMap));
}

// The only difference between Fcfs and Packed is that in fcfs, each commnode
// (one per node) will fork/exec one leaf process, and in packed, each commnode
// (still one per node) will fork/exec n leaf processes, where n is the number
// of individual ranks on the node that the commnode lives on.

static auto fcfsTree(zmq::context_t& ctx, net::TcpDownstreamLink& rootLink, zmqnet::UserCallbacks& uCbs,
	int app_id, int session_id, int manifest_id, int fanout, zmqnet::Auth const& auth,
	char const* const* leaf_argv, char const* const* leaf_env, std::string const& commnodePath,
	cti_hostsList_t const& hostsPlacement, const std::atomic<bool>* killSwitch)
{
	return fcfsTreeImpl(ctx, rootLink, uCbs, app_id, session_id, manifest_id, fanout, auth,
		leaf_argv, leaf_env, commnodePath, hostsPlacement, killSwitch, /* packed? */ false);
}

// Return max node ID, assigned node ID map
static auto packedTree(zmq::context_t& ctx, net::TcpDownstreamLink& rootLink, zmqnet::UserCallbacks& uCbs,
	int app_id, int session_id, int manifest_id, int fanout, zmqnet::Auth const& auth,
	char const* const* leaf_argv, char const* const* leaf_env, std::string const& commnodePath,
	cti_hostsList_t const& hostsPlacement, const std::atomic<bool>* killSwitch)
{
	return fcfsTreeImpl(ctx, rootLink, uCbs, app_id, session_id, manifest_id, fanout, auth,
		leaf_argv, leaf_env, commnodePath, hostsPlacement, killSwitch, /* packed? */ true);
}

// Return max node ID, assigned node ID map
static auto layeredTree(zmq::context_t& ctx, net::TcpDownstreamLink& rootLink, int app_id, int session_id, int manifest_id, int fanout, zmqnet::Auth const& auth,
	char const* const* leaf_argv, char const* const* leaf_env, std::string const& commnodePath, cti_hostsList_t const& hostsPlacement)
{
	auto const num_levels = (hostsPlacement.numHosts > 1)
		? (int)(log(hostsPlacement.numHosts - 1) / log(fanout)) + 2
		: 1;
	log("fanout %d, %d hosts, %d levels\n", fanout, hostsPlacement.numHosts, num_levels);

	cti_addManifestBinary(manifest_id, commnodePath.c_str());

	auto nodeHostnameMap = std::unordered_map<int, std::string>{};
	auto hostnameNodeMap = std::unordered_map<std::string, int>{};
	auto nodeParentMap = std::unordered_map<int, int>{};
	auto nodeEndpointsMap = std::unordered_map<int, std::tuple<std::string, std::string, std::string>>{};
	auto nodeNumChildrenMap = std::unordered_map<int, int>{};
	auto nodeNumSubchildrenMap = std::unordered_map<int, int>{};

	auto hostname = std::unique_ptr<char, decltype(&::free)> {
		cti_getHostname(), ::free};
	if (hostname == nullptr) {
		throw std::runtime_error("failed to detect hostname.\n"
			"Try setting CTI_HOST_ADDRESS to the external address for this node.\n"
			+ std::string{cti_error_str()});
	}
	auto const [rootControlEndpoint, rootDataEndpoint] = formatEndpoints(rootLink, hostname.get());

	// Initialize tree level arrays
	auto nodeLevels = std::vector<std::vector<int>>{};
	nodeLevels.reserve(num_levels);
	for (int i = 0; i < num_levels; i++) {
		nodeLevels.emplace_back(std::vector<int>{});
	}

	// Set root node connection information
	nodeHostnameMap[0] = hostsPlacement.hosts[0].hostname;
	hostnameNodeMap[hostsPlacement.hosts[0].hostname] = 0;
	nodeEndpointsMap.emplace(-1, std::make_tuple(rootControlEndpoint, rootDataEndpoint, rootLink.getPublicKey()));
	nodeParentMap[0] = -1;
	nodeLevels[0].emplace_back(0);

	// Build fanout tree
	auto const num_nodes = hostsPlacement.numHosts;

	// First level will have `fanout` nodes and node 0 as its parent
	{ int cursor = 0;
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
			auto [parentControlEndpoint, parentDataEndpoint, parentPublicKey] = nodeEndpointsMap[parent_node];

			nodeConnectionStream << std::move(parentControlEndpoint)
				<< " " << std::move(parentDataEndpoint)
				<< " " << std::move(parentPublicKey)
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
		if (cti_execToolDaemon(manifest_id, commnodePath.c_str(), commnodeArgv.get() + 1, leaf_env)) {
			throw std::runtime_error(cti_error_str());
		}

		// Wait for an incoming topology message from each network child
		log("waiting for %lu topology messages\n", nodeLevels[i].size());
		for (int j = 0; j < (int)nodeLevels[i].size(); j++) {
			auto reader = net::recv_MessageReader(rootLink.data_socket);
			auto response = reader.getRoot<TopologyRequest>();
			auto const node = hostnameNodeMap[response.getNodeHostname()];
			log("node %2d %s register %s %s\n", node, response.getNodeHostname().cStr(), response.getNodeControlEndpoint().cStr(), response.getNodeDataEndpoint().cStr());
			nodeEndpointsMap.emplace(node, std::make_tuple(response.getNodeControlEndpoint(), response.getNodeDataEndpoint(), response.getNodePublicKey()));
		}

		manifest_id = cti_createManifest(session_id);
	}

	// Send leaf start command
	log("layered connection completed, sending leaf start message\n");
	{ auto builder = capnp::MallocMessageBuilder{};
		auto command = builder.initRoot<NetworkCommand>();
		command.setStartLeaves();

		net::send_MessageBuilder(rootLink.control_socket, net::communicator_ids::AllNodes, builder);
	}

	if (hostnameNodeMap.empty()) {
		throw std::runtime_error("no nodes in network");
	}

	assert(num_nodes > 0);
	auto const max_node_id = num_nodes - 1;

	return std::make_tuple(max_node_id, std::move(hostnameNodeMap));
}

} // launch

// Common frontend implementation

zmqnet::detail::Frontend::Frontend(int const app_id, long int const session_id, long int const manifest_id,
	zmqnet::Topology const& topology, char const* const* leaf_argv, char const* const* leaf_env,
	UserCallbacks&& cbacks, const std::atomic<bool>* killSwitch)
	: m_max_node_id{}
	, m_nodeIdMap{}
	, m_ctx{}
	, m_downstream{}
	, m_next_communicatorId{1}
	, m_uCallbacks{std::move(cbacks)}
{
	auto const start_ms = get_milliseconds();

	// Find commnode path
	auto const commnodePath = get_zmqnet_install_root() / "libexec/commnode";
	
	log("launching commnode: %s\n", commnodePath.c_str());

	// Fetch node layout from CTI for network topology
	auto hostsPlacement = std::unique_ptr<cti_hostsList_t, decltype(&cti_destroyHostsList)>
		{ cti_getAppHostsPlacement(app_id)
		, cti_destroyHostsList
	};

	// Select network encryption / logging type
	auto auth = zmqnet::Auth::Curve;

	if (auto const libzmqnet_curve = ::getenv(env::CURVE)) {
		try {

			// Check run-time encryption build setting
			if (std::stoi(libzmqnet_curve)) {
				auth = zmqnet::Auth::Curve;

			// Encryption disabled by user
			} else {
				auth = zmqnet::Auth::None;
			}

		} catch (std::exception const& ex) {
			throw std::runtime_error(std::string{"Invalid setting for "} + env::CURVE
				+ " (expected 0 or 1): " + std::string{libzmqnet_curve});
		}
	}

	// If debug enabled, switch to logging mode
	if (debug_enabled()) {
		auth = zmqnet::Auth::Log;
	}

	// Ensure encryption was enabled if supported
	#ifndef HAVE_ENCRYPTION
	if (auth == zmqnet::Auth::Curve) {
		throw std::runtime_error("ZMQNet library was not built with encryption support.\n"
			"If running without encryption is acceptable, set LIBZMQNET_CURVE=0");
	}
	#endif

	// Instantiate downstream network link
	// If encryption is enabled and libsodium cannot be loaded, this will fail
	try {
		m_downstream = std::make_unique<net::TcpDownstreamLink>(m_ctx, auth);
	} catch (std::exception const&) {
		throw std::runtime_error("ZMQNet library could not load encryption support.\n"
			"If running without encryption is acceptable, set LIBZMQNET_CURVE=0");
	}

	// Set linger on downstream socket
	if (auto cleanup_timeout = ::getenv(zmqnet::env::CLEANUP_TIMEOUT)) {
		try {
			m_downstream->set_linger(std::stoi(cleanup_timeout));
		} catch (...) {
			// Ignore parse failure
		}
	} else {
		// Default linger of 10 seconds
		m_downstream->set_linger(10 * 1000);
	}

	auto const startup_ms = get_milliseconds();
	log("launch started: %5ldms\n", startup_ms - start_ms);

	// Launch tree
	try {
		if (topology.launch == Topology::Launch::Fcfs) {
			std::tie(m_max_node_id, m_nodeIdMap) =
				launch::fcfsTree(m_ctx, *m_downstream, m_uCallbacks, app_id, session_id, manifest_id,
				topology.fanout, auth, leaf_argv, leaf_env, commnodePath, *hostsPlacement, killSwitch);
		} else if (topology.launch == Topology::Launch::Layered) {
			std::tie(m_max_node_id, m_nodeIdMap) =
				launch::layeredTree(m_ctx, *m_downstream, app_id, session_id, manifest_id,
				topology.fanout, auth, leaf_argv, leaf_env, commnodePath,*hostsPlacement);
		} else if (topology.launch == Topology::Launch::Packed) {
			std::tie(m_max_node_id, m_nodeIdMap) =
				launch::packedTree(m_ctx, *m_downstream, m_uCallbacks, app_id, session_id, manifest_id,
				topology.fanout, auth, leaf_argv, leaf_env, commnodePath, *hostsPlacement, killSwitch);
		} else {
			// should never happen
			throw std::runtime_error("invalid topology");
		}
	} catch (std::runtime_error& e) {
		m_ctx.shutdown();
		throw;
	}

	auto const launch_ms = get_milliseconds();
	log("all commnodes connected: %5ldms\n", launch_ms - startup_ms);

	// Wait for all children to send ready over data socket
	log("waiting for ready report\n");
	auto reader = net::recv_MessageReader(m_downstream->data_socket);
	auto response = reader.getRoot<NetworkResponse>();
	assert(response.isReady());

	// Wait for all children to subscribe to the control socket
	{
		log("waiting for all nodes/backends to subscribe to control socket\n");
		auto numBackends = [&topology, &hostsPlacement](){
			switch(topology.launch) {
				case Topology::Launch::Fcfs:
				case Topology::Launch::Layered: {
					// one backend per node
					return hostsPlacement->numHosts;
				}
				case Topology::Launch::Packed: {
					// one backend per rank
					return std::accumulate(hostsPlacement->hosts, hostsPlacement->hosts + hostsPlacement->numHosts, 0,
						[](const auto& ranks, const auto& host){ return ranks + host.numPes; });
				}
				default:
					throw std::runtime_error("Invalid launch style");
			}
		}();

		auto success = [this, numBackends]() {
			const auto totalWaitMs = 10000; // 10 seconds
			const auto intervalMs = 200;
			static_assert(totalWaitMs % intervalMs == 0);
			const auto totalTries = totalWaitMs / intervalMs;

			log("pinging backends every %dms with a total wait time of %dms\n", intervalMs, totalWaitMs);

			for (auto triesRemaining = totalTries; triesRemaining > 0; --triesRemaining) {
				log("sending ping (%d tries left)\n", triesRemaining);
				auto responded = sendPing(this->getAllNodesCommunicator(), m_downstream->control_socket, m_downstream->data_socket, "", intervalMs, {});

				if (debug_enabled()) {
					auto oss = std::ostringstream{};
					for (auto rank: responded) {
						oss << rank << ", ";
					}
					auto str = oss.str();
					if (str.size() >= 2) str = str.substr(0, str.size() - 2); // chop last comma
					log("Got %d responses: { %s }\n", responded.size(), str.c_str());
				}

				if (responded.size() == static_cast<size_t>(numBackends)) return true;
			}
			return false;
		}();

		if (!success) {
			// Kill any commnodes that will listen. Commnodes/backends that made it to their
			// main loops will shut down, and stuck backends will hit an assertion failure and die.
			log("sending last ditch kill signal\n");
			auto builder = capnp::MallocMessageBuilder{};
			auto killSignal = builder.initRoot<NetworkCommand>();
			killSignal.setKill();

			net::send_MessageBuilder(m_downstream->control_socket, net::communicator_ids::AllNodes, builder);

			throw std::runtime_error("Failed to connect to all backends during startup.\n");
		}
	}

	// Create broadcast communicator
	{ auto builder = capnp::MallocMessageBuilder{};
		auto communicatorCommand = builder.initRoot<NetworkCommand>().initCommunicator();
		communicatorCommand.setCommunicatorId(net::communicator_ids::AllNodes);

		// Send communicator create command
		log("registering communicator %02x\n", net::communicator_ids::AllNodes);
		net::send_MessageBuilder(m_downstream->control_socket, net::communicator_ids::AllNodes, builder);
	}

	// Wait for communicator acknowledgement
	{ auto reader = net::recv_MessageReader(m_downstream->data_socket);
		auto response = reader.getRoot<NetworkResponse>();
		assert(response.isCommunicator());
		auto communicator = response.getCommunicator();
		assert(communicator.getCommunicatorId() == net::communicator_ids::AllNodes);

		auto descendants_buf = response.getCommunicator().getNodes().asBytes();
		m_allDescendants = std::make_unique<lrle_bitset>(descendants_buf.begin(), descendants_buf.size());
	}

	auto const leaf_ms = get_milliseconds();
	log("all leaves connected: %5ldms\n", leaf_ms - launch_ms);
	log("all descendants: %s\n", m_allDescendants->to_string().c_str());
}

zmqnet::detail::Frontend::~Frontend()
{
	// Allow messages to flush up to linger
	log("~Frontend closing socket\n");
	m_downstream->close();
	m_downstream.reset();

	log("~Frontend resetting context\n");
	m_ctx.shutdown();
}

zmqnet::Communicator zmqnet::detail::Frontend::getAllNodesCommunicator() const {
	return Communicator { .id = net::communicator_ids::AllNodes };
}

zmqnet::Communicator zmqnet::detail::Frontend::createCommunicator(zmq::socket_t& sender, zmqnet::NodeSet const& nodes)
{
	return createCommunicator(sender, toRankSet(nodes));
}

zmqnet::Communicator zmqnet::detail::Frontend::createCommunicator(zmq::socket_t& sender, const std::set<size_t> &ranksSet) {
	if (ranksSet.empty()) {
		throw std::runtime_error("expected non-empty ranks set");
	}

	// Create subset create command message
	auto builder = capnp::MallocMessageBuilder{};
	auto communicatorCommand = builder.initRoot<NetworkCommand>().initCommunicator();
	auto const communicatorId = m_next_communicatorId++;
	communicatorCommand.setCommunicatorId(communicatorId);

	// Compress bitset into lrle bitset
	auto const ranksSet_bitset = lrle_bitset{ranksSet};

	communicatorCommand.setNodes(kj::ArrayPtr<kj::byte const>
		{ (unsigned char const*)ranksSet_bitset.compressed.data()
		, ranksSet_bitset.compressed.size()
	});

	// Send communicator create command
	log("registering communicator %02x (%s)\n", communicatorId, ranksSet_bitset.to_string().c_str());
	net::send_MessageBuilder(sender, net::communicator_ids::AllNodes, builder);

	// Return new communicator handle
	return Communicator { .id = communicatorId };
}

zmqnet::Stream zmqnet::detail::Frontend::createStream(zmq::socket_t& sender,
	zmqnet::Communicator const& communicator, Stream::Id const& streamId,
	std::string const& filterLibraryName, std::string const& filterFunctionName, zmqnet::StreamSync const& streamSync)
{
	// Create stream create command message
	auto builder = capnp::MallocMessageBuilder{};
	auto streamCommand = builder.initRoot<NetworkCommand>().initStream();
	streamCommand.setStreamId(streamId);
	streamCommand.setFilterLibraryName(filterLibraryName);
	streamCommand.setFilterFunctionName(filterFunctionName);

	// Set synchronization type
	if (std::get_if<sync::Immediate>(&streamSync)) {
		streamCommand.initSync().setImmediate();
	} else if (std::get_if<sync::WaitForAll>(&streamSync)) {
		streamCommand.initSync().setWaitForAll();
	} else if (auto const timeout = std::get_if<sync::Timeout>(&streamSync)) {
		streamCommand.initSync().setTimeout(timeout->ms);
	} else if (auto const timeout = std::get_if<sync::TimeoutAfterLast>(&streamSync)) {
		streamCommand.initSync().setTimeoutAfterLast(timeout->grace);
	} else {
		assert(false);
	}

	// Send filter create command
	log("registering stream %d filter %s function %s with comm id %d\n", streamId, filterLibraryName.c_str(), filterFunctionName.c_str(), communicator.id);
	net::send_MessageBuilder(sender, communicator.id, builder);

	// Return new stream handle
	return Stream { .communicatorId = communicator.id, .id = streamId };
}

void zmqnet::detail::Frontend::sendCommand(
	zmq::socket_t& sender, zmqnet::Stream const& stream,
	char const* command_buffer, size_t command_len)
{
	// Create user command
	auto builder = capnp::MallocMessageBuilder{};
	auto userCommand = builder.initRoot<NetworkCommand>().initUser();
	userCommand.setStreamId(stream.id);
	userCommand.setContent(kj::ArrayPtr<kj::byte const>{(unsigned char const*)command_buffer, command_len});

	// Send user command
	log("user command length %zu on stream %d, comm id 0x%02x\n", command_len, stream.id, stream.communicatorId);
	net::send_MessageBuilder(sender, stream.communicatorId, builder);
}

zmqnet::RankSet zmqnet::detail::Frontend::sendPing(
	Communicator const& comm,
	zmq::socket_t& sender, zmq::socket_t &listener, std::string payload,
	int timeoutMs, RankSet const& stopAfter)
{
	// send a ping to `comm`, listen for responses on `listener` until `timeoutMs`
	// milliseconds have passed or all ranks in `stopAfter` have responded.
	// The ping `payload` will be echoed back by the network.
	//
	// Return the set of ranks that responded to the ping in the allotted time.
	//
	// if `timeoutMs` == -1, use no timeout; a standard blocking wait.
	// if `stopAfter` == {}, only stop listening when the timeout has been hit.

	if ((timeoutMs < 0 && stopAfter.empty()) || timeoutMs < -1) {
		// invalid parameters - would result in infinite wait
		log("WARNING: invalid ping parameters: timeout: %d, empty waiting set?: %d\n",
			timeoutMs, stopAfter.empty());
		return {};
	}

	auto builder = capnp::MallocMessageBuilder{};
	auto message = builder.initRoot<NetworkCommand>();
	message.setPing(payload);

	net::send_MessageBuilder(sender, comm.id, builder);
	log("sent ping on communicator %d. payload: \"%s\".\n", comm.id, payload.c_str());

	auto oldTimeout = listener.get(zmq::sockopt::rcvtimeo);
	listener.set(zmq::sockopt::rcvtimeo, timeoutMs);

	auto stopRemaining = stopAfter;
	auto responded = zmqnet::RankSet{};

	while (auto maybeReader = net::recv_MessageReader_timeout(listener)) {
		auto &reader = *maybeReader;
		auto response = reader.getRoot<NetworkResponse>();

		if (!response.isPing()) {
			log("WARNING: got a non-ping response. ignoring.\n");
			continue;
		}
		if (response.getPing().getNodeId() < 0) {
			log("WARNING: node id in ping response is < 0 (%d). ignoring.\n", response.getPing().getNodeId());
			continue;
		}

		auto nodeId = response.getPing().getNodeId();
		auto responsePayload = response.getPing().getPayload();
		log("got ping response from %d. payload: \"%s\"\n", nodeId, responsePayload.cStr());

		if (std::string_view{responsePayload.begin(), responsePayload.size()} != std::string_view{payload}) {
			log("WARNING: ping response payloads don't match (\"%s\" != \"%s\"). ignoring.\n",
				responsePayload.cStr(), payload.c_str());
				continue;
		}

		stopRemaining.erase(nodeId);
		responded.insert(nodeId);

		if (!stopAfter.empty() && stopRemaining.empty()) {
			log("got responses from everything in passed set.\n");
			break;
		}
	}
	log("no more ping responses.\n");

	listener.set(zmq::sockopt::rcvtimeo, oldTimeout);

	return responded;
}

bool zmqnet::detail::Frontend::confirmPing(
	Communicator const& comm, RankSet const& ranks,
	zmq::socket_t& sender, zmq::socket_t &listener, std::string payload)
{
	// Send pings to confirm all `ranks` are listening to `comm` and responding on `listener`.
	// The ping `payload` will be echoed back by the network.
	//
	// listen for responses with an increasing timeout so a single straggler
	// that just misses the nth millisecond but is ready on the n + 1th millisecond
	// doesn't make everyone wait a super long time.
	//
	// timeout sequence is 50 -> 100 -> 200 -> 400 -> 800 milliseconds
	
	auto tries_left = int{0};
	auto timeout = 50; // milliseconds
	
	for (tries_left = 5; tries_left > 0; tries_left--, timeout *= 2) {
		log("confirming ping on communicator %d. (%d tries left, %d ms timeout)\n",
			comm.id, tries_left, timeout);
		auto ping_result = sendPing(comm, sender, listener, payload, timeout, ranks);
		if (ping_result == ranks) break;

		if (debug_enabled()) {
			// log incomplete result
			std::ostringstream ss;
			ss << lrle_bitset(ping_result).to_string();
			ss << " (ping result) != ";
			ss << lrle_bitset(ranks).to_string();
			ss << " (target)";
			log ("%s\n", ss.str().c_str());
		}
	}

	if (tries_left == 0) {
		return false;
	}

	log("ping confirm on communicator %d was successful\n", comm.id);
	return true;
}

void zmqnet::detail::Frontend::sendExit(zmq::socket_t& sender)
{
	// Create exit command
	auto builder = capnp::MallocMessageBuilder{};
	auto command = builder.initRoot<NetworkCommand>();
	command.setExit();

	// Send exit command
	net::send_MessageBuilder(sender, net::communicator_ids::AllNodes, builder);
}

zmqnet::RankSet zmqnet::detail::Frontend::toRankSet(zmqnet::NodeSet const& nodes)
{
	auto node_ids = std::set<size_t>{};
	for (auto&& node : nodes) {
		node_ids.insert(m_nodeIdMap.at(node));
	}

	return node_ids;
}

// Synchronous frontend implementation

zmqnet::SyncFrontend::SyncFrontend(int const app_id, long int const session_id, long int const manifest_id,
	zmqnet::Topology const& topology, char const* const* leaf_argv, char const* const* leaf_env,
	UserCallbacks&& cbacks, const std::atomic<bool>* killSwitch)
	: Frontend{app_id, session_id, manifest_id, topology, leaf_argv, leaf_env, std::move(cbacks), killSwitch}
{}

zmqnet::Communicator zmqnet::SyncFrontend::getAllNodesCommunicator() const
{
	return Frontend::getAllNodesCommunicator();
}

zmqnet::Communicator zmqnet::SyncFrontend::registerCommunicator(zmqnet::NodeSet const& nodes)
{
	return registerCommunicator(Frontend::toRankSet(nodes));
}

zmqnet::Communicator zmqnet::SyncFrontend::registerCommunicator(const RankSet &ranks)
{
	if (ranks.empty()) {
		throw std::runtime_error("expected non-empty ranks set");
	}

	auto const start_ms = get_milliseconds();

	// Send communicator create command
	auto const communicator = Frontend::createCommunicator(m_downstream->control_socket, ranks);

	// Wait for acknowledgement
	auto reader = net::recv_MessageReader(m_downstream->data_socket);
	auto response = reader.getRoot<NetworkResponse>();
	assert(response.isCommunicator() && (response.getCommunicator().getCommunicatorId() == communicator.id));

	auto const communicator_ms = get_milliseconds() - start_ms;
	log("communicator registered RT: %5ldms\n", communicator_ms);

	// Leaves and commnodes have now received the message to subscribe to a
	// new zmq channel and are busy subscribing. Wait some time for them to complete.
	if (!confirmPing(communicator, ranks)) {
		throw std::runtime_error("Nodes failed to connect to communicator");
	}

	// Return new communicator handle
	return communicator;
}

zmqnet::Stream zmqnet::SyncFrontend::registerStream(zmqnet::Communicator const& communicator,
		zmqnet::Stream::Id const& streamId, std::string const& filterLibraryName, std::string const& filterFunctionName, zmqnet::StreamSync const& streamSync)
{
	auto const start_ms = get_milliseconds();

	// Send stream create command
	auto const stream = createStream(communicator, streamId, filterLibraryName, filterFunctionName, streamSync);

	// Wait for acknowledgement
	auto reader = net::recv_MessageReader(m_downstream->data_socket);
	auto response = reader.getRoot<NetworkResponse>();
	assert(response.isStream() && (response.getStream().getStreamId() == streamId));

	auto const stream_ms = get_milliseconds() - start_ms;
	log("stream registered RT: %5ldms\n", stream_ms);

	// Return new stream handle
	return stream;
}

zmqnet::ByteArray zmqnet::SyncFrontend::runCommand(zmqnet::Stream const& stream, char const* command_buffer, size_t command_len)
{
	auto const start_ms = get_milliseconds();

	// Send user command
	Frontend::sendCommand(m_downstream->control_socket, stream, command_buffer, command_len);

	// Receive aggregated result
	log("receiving results...\n");
	auto reader = net::recv_MessageReader(m_downstream->data_socket);
	auto response = reader.getRoot<NetworkResponse>();
	assert_response_type(response, NetworkResponse::Which::USER);
	assert(response.getUser().getStreamId() == stream.id);

	auto const result_ms = get_milliseconds();
	log("result RT: %5ldms\n", result_ms - start_ms);

	return kj::heapArray<kj::byte>(response.getUser().getContent().asBytes());
}

zmqnet::ByteArray zmqnet::SyncFrontend::runCommand(zmqnet::Stream const& stream, std::string const& command)
{
	return runCommand(stream, command.c_str(), command.length() + 1);
}

zmqnet::RankSet zmqnet::SyncFrontend::sendPing(Communicator const& comm, int timeoutMs, RankSet const& stopAfter)
{
	return Frontend::sendPing(comm, m_downstream->control_socket, m_downstream->data_socket, "", timeoutMs, stopAfter);
}

bool zmqnet::SyncFrontend::confirmPing(Communicator const& comm, RankSet const& ranks)
{
	return Frontend::confirmPing(comm, ranks, m_downstream->control_socket, m_downstream->data_socket, "");
}

zmqnet::Communicator zmqnet::SyncFrontend::createCommunicator(const RankSet& ranks) {
	return Frontend::createCommunicator(m_downstream->control_socket, ranks);
}

zmqnet::Stream zmqnet::SyncFrontend::createStream(
	Communicator const& communicator, Stream::Id const& streamId,
	std::string const& filterLibraryName, std::string const& filterFunctionName,
	zmqnet::StreamSync const& streamSync)
{
	return Frontend::createStream(
		m_downstream->control_socket, communicator, streamId,
		filterLibraryName, filterFunctionName, streamSync
	);
}

void zmqnet::SyncFrontend::sendExit() {
	Frontend::sendExit(m_downstream->control_socket);
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
	assert(response.isExit());

	log("exit completed\n");

	auto const exit_ms = get_milliseconds();
	log("exit RT: %5ldms\n", exit_ms - start_ms);
}


void zmqnet::SyncFrontend::forceDisconnect()
{
	// Send exit command to try to get the backends to clean up.
	sendExit();

	// Wait for downstream sockets to flush
	m_downstream->close();
	m_downstream.reset();

	// Shut down all sockets.
	m_ctx.shutdown();

	log("forced exit completed\n");
}
