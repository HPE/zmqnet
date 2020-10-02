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

#include "argv_defs.hpp"
#include "../tests/cti_split.hpp"
#include "cti_dlopen.hpp"

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

Frontend::Frontend(zmq::context_t& ctx, int const app_id, int const manifest_id, int const fanout,
	std::string const& filterName, char const* const* leaf_argv)
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
		commnodeArgv.add(CommnodeArgv::FilterName, filterName);

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

Backend::Backend(zmq::context_t& ctx, std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint)
	: m_upstream{ctx, upstreamControlEndpoint, upstreamDataEndpoint}
{
	// Notify parent commnode that setup is completed
	m_upstream.send_ready();
}

std::string Backend::nextCommand()
{
	return m_upstream.recv_command();
}

void Backend::sendResult(std::string const& result)
{
	m_upstream.send_result(result);
}

class Filter
{
	using RunType = std::string(std::vector<std::string> const&);

	cti::Dlopen::Handle m_handle;
	std::function<RunType> m_run;

public:
	Filter(std::string const& filterPath)
		: m_handle{filterPath}
		, m_run{m_handle.load<RunType>("run")}
	{}

	std::string run(std::vector<std::string> const& inputs) const
	{
		return m_run(inputs);
	}
};

Commnode::Commnode(zmq::context_t& ctx, std::string const& dealerEndpoint,
	std::string const& filterName, char const* const* leaf_argv)
	: m_upstream{ctx}
	, m_downstream{ctx}
	, m_numChildren{0}
	, m_localChildPids{}
	, m_filter{std::make_unique<Filter>(std::string{cti_be_getLibDir()} + "/" + filterName)}
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

	// Determine number of local children, one per PE on node
	auto const firstPE = cti_be_getNodeFirstPE();
	auto const localPEs = cti_be_getNodePEs();
	m_numChildren += localPEs;

	// Launch local children with provided leaf binary
	auto const leafPath = std::string{cti_be_getBinDir()} + "/" + leaf_argv[0];
	fprintf(stderr, "forking %d local leaf processes\n", localPEs);
	for (int rank = firstPE; rank < firstPE + localPEs; rank++) {

		if (auto const forkedPid = fork()) {
			// Add launched process to cleanup list
			m_localChildPids.emplace_back(forkedPid);

		} else {
			// Prepend endpoints and rank number to provided leaf arguments
			auto completeLeafArgv = cti::ManagedArgv
				{ leaf_argv[0]
				, controlEndpoint
				, dataEndpoint
				, std::to_string(rank)
			};

			// Copy provided arguments
			for (auto argptr = leaf_argv + 1; *argptr != nullptr; argptr++) {
				completeLeafArgv.add(*argptr);
			}

			// Exec leaf binary
			execvp(leafPath.c_str(), completeLeafArgv.get());
			perror("execvp");
			_exit(-1);
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

std::string Commnode::filterResults(std::vector<std::string> const& results)
{
	return m_filter->run(results);
}

std::vector<std::string> Commnode::collectResults()
{
	return m_downstream.recv_results(m_numChildren);
}

void Commnode::sendResult(std::string const& result)
{
	m_upstream.send_result(result);
}
