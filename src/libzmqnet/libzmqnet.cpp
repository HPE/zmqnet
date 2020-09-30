/*
 * Copyright 2011-2020 Hewlett Packard Enterprise Development LP.
 */

#include <unistd.h>
#include <wait.h>

#include <cassert>

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
	fprintf(stderr, "waiting for command from upstream\n");
	auto const command = m_upstream.recv_command();

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
