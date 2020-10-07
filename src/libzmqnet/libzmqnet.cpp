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

#include "zmqnet.hpp"

#include "libzmqnet.hpp"

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
		// Receive topology request from new node
		{ auto [request, requestReader, topologyRequest] = recv_MessageRoot<TopologyRequest>(dealer_socket, zmq::recv_flags::none);

			// Register topology information
			endpoints.emplace_back(topologyRequest.getNodeControlEndpoint().cStr(), topologyRequest.getNodeDataEndpoint().cStr());
		}

		// Determine the number of children of current node
		auto const remain = num_nodes - (seen + 1);
		levelFilled++;
		auto const rightFill = levelCap - levelFilled;
		auto const leftFill = (levelFilled - 1) * fanout;
		auto const numChildren = std::min(fanout, std::max(0, remain - (rightFill + leftFill)));

		// Create topology response
		{ auto builder = capnp::MallocMessageBuilder{};
			auto topologyResponse = builder.initRoot<TopologyResponse>();

			auto const& [parentControlEndpoint, parentDataEndpoint] = endpoints[cursor];
			topologyResponse.setParentControlEndpoint(parentControlEndpoint);
			topologyResponse.setParentDataEndpoint(parentDataEndpoint);
			topologyResponse.setNumChildren(numChildren);

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
	auto [reader, responseReader, response] = recv_MessageRoot<NetworkResponse>(m_downstream.data_socket);
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
	fprintf(stderr, "registering filter %s with id %d\n", filterName.c_str(), streamId);
	send_MessageBuilder(m_downstream.control_socket, builder);

	// Wait for acknowledgement
	auto [reader, responseReader, response] = recv_MessageRoot<NetworkResponse>(m_downstream.data_socket);
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
	fprintf(stderr, "user command %s on stream %d\n", command.c_str(), streamId);
	send_MessageBuilder(m_downstream.control_socket, builder);

	// Receive aggregated result
	fprintf(stderr, "receiving results...\n");
	auto [reader, responseReader, response] = recv_MessageRoot<NetworkResponse>(m_downstream.data_socket);
	assert(response.isUserResult() && (response.getUserResult().getStreamId() == streamId));

	auto result = std::string{response.getUserResult().getContent().cStr()};
	fprintf(stderr, "result: %s\n", result.c_str());

	return result;
}

void Frontend::runExit()
{
	fprintf(stderr, "sending exit command\n");

	// Create exit command
	auto builder = capnp::MallocMessageBuilder{};
	auto command = builder.initRoot<NetworkCommand>();
	command.setExit();

	// Send exit command
	send_MessageBuilder(m_downstream.control_socket, builder);

	// Wait for exit response
	auto [reader, responseReader, response] = recv_MessageRoot<NetworkResponse>(m_downstream.data_socket);
	assert(response.isExitAck());

	fprintf(stderr, "exit completed\n");
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
		auto [reader, commandReader, command] = recv_MessageRoot<NetworkCommand>(m_upstream.control_socket);

		// Detect exit case
		if (command.isExit()) {
			fprintf(stderr, "rank %d: exit\n", m_rank);
			auto builder = capnp::MallocMessageBuilder{};
			auto response = builder.initRoot<NetworkResponse>();
			response.setExitAck();

			send_MessageBuilder(m_upstream.data_socket, builder);

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
