/*
 * Copyright 2011-2020 Hewlett Packard Enterprise Development LP.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <unistd.h>
#include <wait.h>

#include <cassert>
#include <cmath>

#include <filesystem>

#include "common_tools_fe.h"
#include "common_tools_be.h"

#include "cti_split.hpp"
#include "cti_dlopen.hpp"

#include "argv_defs.hpp"

#include "zmqnet.hpp"

// Replace the sockets' endpoints with e.g. known-accessible hostname from CTI
static auto formatEndpoints(ConsumerNode<TcpDownstreamLink> const& node, std::string const& hostname)
{
	auto [controlEndpoint, dataEndpoint] = node.getEndpoints();

	return std::make_tuple(
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
	auto endpoints = std::vector<std::tuple<std::string, std::string>>{
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

		auto const& [upstreamControlEndpoint, upstreamDataEndpoint] = endpoints[cursor];
		auto const upstreamEndpointsMessage = upstreamControlEndpoint + " " + upstreamDataEndpoint + " " + std::to_string(numChildren);
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

Frontend::Frontend(zmq::context_t& ctx, int const app_id, int const manifest_id, int const fanout,
	char const* const* leaf_argv)
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
	auto const dealerEndpoint = replace_endpoint_host(dealer_socket.get(zmq::sockopt::last_endpoint), cti_getHostname());
	printf("topology dealer listening on %s\n", dealerEndpoint.c_str());

	// Find commnode path
	auto const commnodePath = std::string{LIBZMQNET_PREFIX} + "/libexec/commnode";

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
	auto const [controlEndpoint, dataEndpoint] = formatEndpoints(m_downstream, cti_getHostname());
	connectFanoutTopology(std::move(dealer_socket), controlEndpoint, dataEndpoint,
		fanout, hostsPlacement->numHosts);

	// Wait for all children to send ready over data socket
	fprintf(stderr, "waiting for ready report\n");
	m_downstream.children_ready_loop(1);
}

void Frontend::registerStream(StreamId const& streamId, std::string const& filterName)
{
	// Send filter register command to network
	fprintf(stderr, "registering filter %s\n", filterName.c_str());
	auto const registerFilterMessage = std::to_string(streamId) + " " + filterName;
	m_downstream.send_command(registerFilterMessage);

	auto const children_results = m_downstream.recv_results(1);
	assert(children_results.size() == 1);
	assert(children_results[0] == registerFilterMessage);
}

std::string Frontend::runCommand(StreamId const& streamId, std::string const& command)
{
	// Send command to network
	fprintf(stderr, "sending command %s\n", command.c_str());
	m_downstream.send_command(command);

	// Receive aggregated result
	fprintf(stderr, "receiving results...\n");
	auto const children_results = m_downstream.recv_results(1);
	assert(children_results.size() == 1);

	auto [streamString, result] = cti::split::string<2>(std::move(children_results[0]));
	assert(std::stoi(streamString) == streamId);

	fprintf(stderr, "result: %s\n", result.c_str());

	return result;
}

void Frontend::runExit()
{
	// Send exit command to network, no response
	fprintf(stderr, "sending exit command\n");
	m_downstream.send_command("-1 exit");
}

Backend::Backend(zmq::context_t& ctx, std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint)
	: m_upstream{ctx, upstreamControlEndpoint, upstreamDataEndpoint}
{
	// Notify parent commnode that setup is completed
	m_upstream.send_ready();
}

std::tuple<StreamId, std::string> Backend::nextCommand()
{
	auto [streamString, command] = cti::split::string<2>(m_upstream.recv_command());
	return std::make_tuple(std::stoi(streamString), std::move(command));
}

void Backend::sendResult(StreamId const& streamId, std::string const& result)
{
	m_upstream.send_result(std::to_string(streamId) + " " + result);
}
