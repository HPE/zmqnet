/*
 * Copyright 2011-2020 Hewlett Packard Enterprise Development LP.
 */

#include <cassert>

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

void run_leafnode(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int rank)
{
	fprintf(stderr, "running leafnode rank %d, connect parent %s %s\n", rank, upstreamControlEndpoint.c_str(), upstreamDataEndpoint.c_str());

	auto ctx = zmq::context_t{};

	auto leafnode = LeafNode{ctx, upstreamControlEndpoint, upstreamDataEndpoint};

	fprintf(stderr, "%d send ready\n", rank);
	leafnode.send_ready();

	while (true) {
		auto const command = leafnode.recv_command();
		fprintf(stderr, "%d got command %s\n", rank, command.c_str());

		if (command == "exit") {
			return;

		} else {
			leafnode.send_result(std::to_string(rank));
		}
	}
}

int run_commnode(std::string const& dealer_endpoint)
{
	int rc = -1;

	auto ctx = zmq::context_t{};

	auto commnode = Commnode{ctx, dealer_endpoint, run_leafnode};

	while (true) {
		auto const command = commnode.nextCommand();

		auto const children_results = commnode.collectResults();

		if (command == "exit") {
			return 0;

		} else {
			auto result = std::string{"{ "};
			for (auto&& child_result : children_results) {
				assert(child_result != "ready");
				result += child_result + " ";
			}
			result += "}";

			commnode.sendResult(result);
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

		auto rootnode = Frontend{ctx, app_id, 5};

		auto const result = rootnode.runCommand("send");
		fprintf(stderr, "result: '%s'\n", result.c_str());

		rootnode.runExit();

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