/*
 * Copyright 2011-2020 Hewlett Packard Enterprise Development LP.
 */

#include <unistd.h>
#include <wait.h>

#include <cassert>
#include <cmath>

#include "common_tools_fe.h"
#include "common_tools_be.h"

#include "../tests/cti_split.hpp"

#include "zmqnet.hpp"


// Replace the sockets' endpoints with e.g. known-accessible hostname from CTI
static auto formatEndpoints(ConsumerNode<TcpDownstreamLink> const& node, std::string const& hostname)
{
	auto [controlEndpoint, dataEndpoint] = node.getEndpoints();

	return std::make_pair(
		replace_endpoint_host(std::move(controlEndpoint), hostname),
		replace_endpoint_host(std::move(dataEndpoint),    hostname)
	);
}

static auto exchangeTopologyData(zmq::context_t& ctx, std::string const& dealerEndpoint,
	std::string const& controlEndpoint, std::string const& dataEndpoint)
{
	// Connect to topology server
	auto request_socket = zmq::socket_t{ctx, zmq::socket_type::req};
	request_socket.connect(dealerEndpoint);
	fprintf(stderr, "connected to topology server %s\n", dealerEndpoint.c_str());

	// Send this node's endpoints to topology server
	auto const downstreamEndpointsMessage = controlEndpoint + " " + dataEndpoint;
	send_string(request_socket, downstreamEndpointsMessage, zmq::send_flags::none);
	fprintf(stderr, "registered endpoints with topology server\n");

	// Receive this node's parents and number of network children from topology server
	auto [parentControlEndpoint, parentDataEndpoint, rawNumChildren] =
		cti::split::string<3>(recv_string(request_socket, zmq::recv_flags::none));
	fprintf(stderr, "topology server reported parent %s / %s, %s children\n",
		parentControlEndpoint.c_str(), parentDataEndpoint.c_str(), rawNumChildren.c_str());

	return std::make_tuple(std::move(parentControlEndpoint), std::move(parentDataEndpoint), std::stoi(rawNumChildren));
}

// Build fanout tree in first-come first-served order. Does not require any precomputing of topology
// tree by the frontend.
static void connectFanoutTopology(zmq::socket_t&& dealer_socket,
	std::string const& rootControlEndpoint, std::string const& rootDataEndpoint,
	int const fanout, int const num_nodes)
{
	auto endpoints = std::vector<std::pair<std::string, std::string>>{
		{rootControlEndpoint, rootDataEndpoint}
	};
	endpoints.reserve(num_nodes + 1);
	auto cursor = 0;

	int level = 0;
	int levelFilled = 0;
	int levelCap = 1;

	for (int seen = 0; seen < num_nodes; seen++) {
		auto [controlEndpoint, dataEndpoint] =
			cti::split::string<2>(recv_string(dealer_socket, zmq::recv_flags::none));
		endpoints.emplace_back(std::move(controlEndpoint), std::move(dataEndpoint));

		auto const remain = num_nodes - (seen + 1);
		levelFilled++;
		auto const rightFill = levelCap - levelFilled;
		auto const leftFill = (levelFilled - 1) * fanout;
		auto const numChildren = std::min(fanout, std::max(0, remain - (rightFill + leftFill)));

		auto const upstreamEndpointsMessage = endpoints[cursor].first + " " + endpoints[cursor].second + " " + std::to_string(numChildren);
		send_string(dealer_socket, upstreamEndpointsMessage, zmq::send_flags::none);

		if (seen % fanout == 0) {
			cursor++;
		}
		if (levelFilled == (int)pow(fanout, level)) {
			level++;
			levelFilled = 0;
			levelCap = (int)pow(fanout, level);
		}
		fprintf(stderr, "%d connected\n", seen);
	}
}

Frontend::Frontend(zmq::context_t& ctx, int const app_id, int const fanout)
	: m_downstream{ctx}
{
	// Fetch node layout from CTI for network topology
	auto hostsPlacement = std::unique_ptr<cti_hostsList_t, decltype(&cti_destroyHostsList)>
		{ cti_getAppHostsPlacement(app_id)
		, cti_destroyHostsList
	};

	// Start topology socket to send its connection info to commnode daemon
	auto dealer_socket = zmq::socket_t{ctx, zmq::socket_type::rep};
	dealer_socket.bind("tcp://*:*");
	auto const dealer_endpoint = replace_endpoint_host(dealer_socket.get(zmq::sockopt::last_endpoint), cti_getHostname());
	printf("topology dealer listening on %s\n", dealer_endpoint.c_str());

	// Set up commnode daemon arguments with topology connection info
	char const* daemon_args[3] =
		{ "client"
		, dealer_endpoint.c_str()
		, nullptr
	};

	// Launch commnode daemon
	auto const session_id = cti_createSession(app_id);
	auto const manifest_id = cti_createManifest(session_id);
	if (cti_execToolDaemon(manifest_id, "./topotest", daemon_args, nullptr)) {
		throw std::runtime_error(cti_error_str());
	}

	// Set up fanout topology and wait for all children to connect
	auto const [controlEndpoint, dataEndpoint] = formatEndpoints(m_downstream, cti_getHostname());
	connectFanoutTopology(std::move(dealer_socket), controlEndpoint, dataEndpoint,
		fanout, hostsPlacement->numHosts);

	// Wait for all children to send ready over data socket
	fprintf(stderr, "waiting for ready report\n");
	m_downstream.children_ready_loop(1);
}

std::string Frontend::runCommand(std::string const& command)
{
	// Send command to network
	fprintf(stderr, "sending command %s\n", command.c_str());
	m_downstream.send_command(command);

	// Receive aggregated result
	fprintf(stderr, "receiving results...\n");
	auto const children_results = m_downstream.recv_results(1);
	assert(children_results.size() == 1);

	fprintf(stderr, "result: %s\n", children_results[0].c_str());

	return children_results[0];
}

void Frontend::runExit()
{
	// Send exit command to network, no response
	fprintf(stderr, "sending exit command\n");
	m_downstream.send_command("exit");
}

Commnode::Commnode(zmq::context_t& ctx, std::string const& dealerEndpoint, void(*leafFunc)(std::string const&, std::string const&, int))
	: m_upstream{ctx}
	, m_downstream{ctx}
	, m_numChildren{0}
	, m_localChildPids{}
{
	// Format externally-accessible endpoints
	auto const [controlEndpoint, dataEndpoint] = formatEndpoints(m_downstream, cti_be_getNodeHostname());
	fprintf(stderr, "listening on %s / %s\n", controlEndpoint.c_str(), dataEndpoint.c_str());

	// Send endpoints to topology server and receive parent's endpoints and number of network children
	auto const [parentControlEndpoint, parentDataEndpoint, numNetworkChildren] =
		exchangeTopologyData(ctx, dealerEndpoint, controlEndpoint, dataEndpoint);
	m_numChildren += numNetworkChildren;

	// Connect to parent's endpoints
	fprintf(stderr, "connecting to parent %s / %s\n", parentControlEndpoint.c_str(), parentDataEndpoint.c_str());
	m_upstream.connect(parentControlEndpoint, parentDataEndpoint);

	// Launch local children, one per PE on node
	auto const firstPE = cti_be_getNodeFirstPE();
	auto const localPEs = cti_be_getNodePEs();
	m_numChildren += localPEs;
	fprintf(stderr, "forking %d local leaf processes\n", localPEs);
	for (int i = firstPE; i < firstPE + localPEs; i++) {
		if (auto const forkedPid = fork()) {
		m_localChildPids.emplace_back(forkedPid);
		} else {
			leafFunc(controlEndpoint, dataEndpoint, i);
		}
	}

	// Wait for all children to connect and send ready
	fprintf(stderr, "waiting for %d children\n", m_numChildren);
	m_downstream.children_ready_loop(m_numChildren);

	// Send ready to parent
	fprintf(stderr, "sending ready to parent\n");
	m_upstream.send_ready();
}

Commnode::~Commnode()
{
	// Wait on all local child PIDs
	for (auto&& pid : m_localChildPids) {
		int status;
		if (waitpid(pid, &status, 0) || (WIFEXITED(status) && WEXITSTATUS(status))) {
			fprintf(stderr, "pid %d failed!\n", pid);
		}
	}
}

std::string Commnode::nextCommand()
{
	// Receive command from parent
	fprintf(stderr, "waiting for command from upstream\n");
	auto const command = m_upstream.recv_command();

	// Relay command to children
	fprintf(stderr, "relay command downstream %s\n", command.c_str());
	m_downstream.send_command(command);

	return command;
}

std::vector<std::string> Commnode::collectResults()
{
	return m_downstream.recv_results(m_numChildren);
}

void Commnode::sendResult(std::string const& result)
{
	m_upstream.send_result(result);
}
