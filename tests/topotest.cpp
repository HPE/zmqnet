/*
 * Copyright 2011-2020 Hewlett Packard Enterprise Development LP.
 */

#include <cassert>
#include <cmath>

#include <unistd.h>
#include <wait.h>

#include <string>
#include <vector>
#include <future>
#include <unordered_map>

#include "zmqnet.hpp"

#include "common_tools_fe.h"
#include "common_tools_be.h"

#include "cti_split.hpp"

extern char **environ;

int run_leafnode(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int rank)
{
	int rc = -1;

	fprintf(stderr, "running leafnode rank %d, connect parent %s %s\n", rank, upstreamControlEndpoint.c_str(), upstreamDataEndpoint.c_str());

	auto ctx = zmq::context_t{};

	auto leafnode = LeafNode{ctx, upstreamControlEndpoint, upstreamDataEndpoint};

	fprintf(stderr, "%d send ready\n", rank);
	leafnode.send_ready();

	while (true) {
		auto const command = leafnode.recv_command();
		fprintf(stderr, "%d got command %s\n", rank, command.c_str());
		if (command == "exit") {
			rc = 0;
			goto cleanup_run_leafnode;
		}

		leafnode.send_result(std::to_string(rank));
	}

	rc = 0;

cleanup_run_leafnode:
	return rc;
}

int run_commnode(std::string const& dealer_endpoint)
{
	int rc = -1;

	auto ctx = zmq::context_t{};

	auto request_socket = zmq::socket_t{ctx, zmq::socket_type::req};
	request_socket.connect(dealer_endpoint);
	fprintf(stderr, "connected to topology server %s\n", dealer_endpoint.c_str());

	auto commnode = CommNode<TcpDownstreamLink>{ctx};
	auto const [controlEndpoint, dataEndpoint] = TcpDownstreamLink::formatEndpoints(commnode, cti_be_getNodeHostname());
	fprintf(stderr, "listening on %s / %s\n", controlEndpoint.c_str(), dataEndpoint.c_str());

	auto const downstreamEndpointsMessage = controlEndpoint + " " + dataEndpoint;
	send_string(request_socket, downstreamEndpointsMessage, zmq::send_flags::none);
	fprintf(stderr, "registered endpoints with topology server\n");

	auto const [parentControlEndpoint, parentDataEndpoint, rawNumChildren] =
		cti::split::string<3>(recv_string(request_socket, zmq::recv_flags::none));
	auto numChildren = std::stoi(rawNumChildren);
	fprintf(stderr, "topology server reported parent %s / %s, %d children\n",
		parentControlEndpoint.c_str(), parentDataEndpoint.c_str(), numChildren);

	commnode.connect(parentControlEndpoint, parentDataEndpoint);
	fprintf(stderr, "connected to parent %s / %s\n", parentControlEndpoint.c_str(), parentDataEndpoint.c_str());

	auto localChildPids = std::vector<pid_t>{};
	auto const firstPE = cti_be_getNodeFirstPE();
	auto const localPEs = cti_be_getNodePEs();
	numChildren += localPEs;
	fprintf(stderr, "forking %d local leaf processes\n", localPEs);
	for (int i = firstPE; i < firstPE + localPEs; i++) {
		if (auto const forkedPid = fork()) {
		localChildPids.emplace_back(forkedPid);
		} else {
			return run_leafnode(controlEndpoint, dataEndpoint, i);
		}
	}

	fprintf(stderr, "waiting for %d children\n", numChildren);
	commnode.children_ready_loop(numChildren);
	fprintf(stderr, "send ready\n");
	commnode.send_ready();

	while (true) {
		auto const command = commnode.recv_command();
		fprintf(stderr, "relay command %s\n", command.c_str());
		commnode.send_command(command);

		if (command == "exit") {
			return 0;
		}

		auto const children_results = commnode.recv_results(numChildren);

		auto result = std::string{"{ "};
		for (auto&& child_result : children_results) {
			assert(child_result != "ready");
			result += child_result + " ";
		}
		result += "}";

		commnode.send_result(result);
	}

cleanup_run_client:
	for (auto&& pid : localChildPids) {
		int status;
		if (waitpid(pid, &status, 0) || (WIFEXITED(status) && WEXITSTATUS(status))) {
			fprintf(stderr, "pid %d failed!\n", pid);
		}
	}

	return rc;
}

int main(int argc, char** argv)
{
	int rc = -1;
	int app_id = 0;

	if (argc < 2) {
		goto cleanup_main;
	}

	fprintf(stderr, "mode: %s\n", argv[1]);

	if (!strcmp("dealer", argv[1])) {
		auto ctx = zmq::context_t{};
		auto reply_socket = zmq::socket_t{ctx, zmq::socket_type::rep};
		reply_socket.bind("tcp://*:*");
		auto const dealer_endpoint = replace_endpoint_host(reply_socket.get(zmq::sockopt::last_endpoint), cti_getHostname());
		printf("topology server listening on %s\n", dealer_endpoint.c_str());

		char const* launcher_args[(argc - 2) + 1];
		for (int i = 0; i < argc - 2; i++) {
			launcher_args[i] = argv[2 + i];
		}
		launcher_args[argc - 2 + 0] = "./stop";
		launcher_args[argc - 2 + 1] = nullptr;

		app_id = cti_launchApp(launcher_args, -1, -1, nullptr, nullptr, nullptr);
		if (!app_id) {
			throw std::runtime_error(cti_error_str());
		}

		char const* daemon_args[3] =
			{ "client"
			, dealer_endpoint.c_str()
			, nullptr
		};

		auto const session_id = cti_createSession(app_id);
		auto const manifest_id = cti_createManifest(session_id);
		if (cti_execToolDaemon(manifest_id, argv[0], daemon_args, nullptr)) {
			throw std::runtime_error(cti_error_str());
		}

		auto hostsPlacement = std::unique_ptr<cti_hostsList_t, decltype(&cti_destroyHostsList)>
			{ cti_getAppHostsPlacement(app_id)
			, cti_destroyHostsList
		};
		fprintf(stderr, "got hosts placement\n");

		auto const fanout = 5;

		auto rootnode = RootNode<TcpDownstreamLink>{ctx};

		auto const [controlEndpoint, dataEndpoint] = TcpDownstreamLink::formatEndpoints(rootnode, cti_getHostname());

		auto endpoints = std::vector<std::pair<std::string, std::string>>{};
		endpoints.emplace_back(controlEndpoint, dataEndpoint);
		auto cursor = 0;

		int const N = hostsPlacement->numHosts;
		int level = 0;
		int levelFilled = 0;
		int levelCap = 1;

		for (int seen = 0; seen < N;) {
			auto [controlEndpoint, dataEndpoint] =
				cti::split::string<2>(recv_string(reply_socket, zmq::recv_flags::none));
			endpoints.emplace_back(std::move(controlEndpoint), std::move(dataEndpoint));

			auto const remain = N - (seen + 1);
			levelFilled++;
			auto const rightFill = levelCap - levelFilled;
			auto const leftFill = (levelFilled - 1) * fanout;
			auto const numChildren = std::min(fanout, std::max(0, remain - (rightFill + leftFill)));

			auto const upstreamEndpointsMessage = endpoints[cursor].first + " " + endpoints[cursor].second + " " + std::to_string(numChildren);
			send_string(reply_socket, upstreamEndpointsMessage, zmq::send_flags::none);

			if (seen % fanout == 0) {
				cursor++;
			}
			if (levelFilled == (int)pow(fanout, level)) {
				level++;
				levelFilled = 0;
				levelCap = (int)pow(fanout, level);
			}
			seen++;
			fprintf(stderr, "%d connected\n", seen);
		}

		fprintf(stderr, "waiting for children\n");
		rootnode.children_ready_loop(1);
		fprintf(stderr, "send command\n");
		rootnode.send_command("send");
		fprintf(stderr, "recv result\n");
		auto const children_results = rootnode.recv_results(1);

		for (auto&& result : children_results) {
			fprintf(stderr, "result: '%s'\n", result.c_str());
		}

		fprintf(stderr, "exit command\n");
		rootnode.send_command("exit");

		cti_deregisterApp(app_id);

	} else if (!strcmp("client", argv[1])) {
		auto const dealer_endpoint = argv[2];

		rc = run_commnode(dealer_endpoint);
	}

cleanup_main:
	if (app_id) {
		cti_deregisterApp(app_id);
	}

	return rc;
}