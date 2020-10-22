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
#include <thread>

#include "zmqnet.hpp"

#include "common_tools_fe.h"
#include "common_tools_be.h"

int run_leafnode(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int rank)
{
	fprintf(stderr, "running leafnode rank %d, connect parent %s %s\n", rank, upstreamControlEndpoint.c_str(), upstreamDataEndpoint.c_str());

	auto ctx = zmq::context_t{};

	auto leafnode = Backend{ctx, upstreamControlEndpoint, upstreamDataEndpoint, rank};

	while (true) {
		auto const [streamId, command] = leafnode.nextCommand();

		if (streamId < 0) {
			return 0;

		} else {
			fprintf(stderr, "%d got command %s from stream %d\n", rank, command.c_str(), streamId);
			assert(streamId == 0);
			leafnode.sendResult(streamId, std::to_string(rank));
		}
	}
}

int main(int argc, char** argv)
{
	int rc = -1;
	int app_id = 0;

	if (argc < 2) {
		goto cleanup_main;
	}

	fprintf(stderr, "mode: %s\n", argv[1]);

	if (!strcmp("leaf", argv[4])) {
		auto const [upstreamControlEndpoint, upstreamDataEndpoint, rank,
			rest_argc, rest_argv] = Backend::getParams(argc, argv);
		assert(rest_argc == argc - 3);
		assert(!strcmp("leaf", rest_argv[1]));

		rc = run_leafnode(upstreamControlEndpoint, upstreamDataEndpoint, rank);

		goto cleanup_main;

	} else {

		auto ctx = zmq::context_t{};

		char const* launcher_args[(argc - 3) + 1];
		for (int i = 0; i < argc - 3; i++) {
			launcher_args[i] = argv[3 + i];
		}
		launcher_args[argc - 3 + 0] = "./stop";
		launcher_args[argc - 3 + 1] = nullptr;

		app_id = cti_launchApp(launcher_args, -1, -1, nullptr, nullptr, nullptr);
		if (!app_id) {
			throw std::runtime_error(cti_error_str());
		}

		auto const session_id = cti_createSession(app_id);
		auto const manifest_id = cti_createManifest(session_id);
		cti_addManifestBinary(manifest_id, "./topotest");
		cti_addManifestLibrary(manifest_id, "./libfilter.so");

		char const* leaf_argv[] = { "topotest", "leaf", nullptr };

		auto topology = Topology{};
		if (!strcmp("fcfs", argv[1])) {
			topology.launch = Topology::Fcfs;
		} else if (!strcmp("layered", argv[1])) {
			topology.launch = Topology::Layered;
		} else {
			fprintf(stderr, "bad args: ");
			for (int i = 0; i < argc; i++) {
				fprintf(stderr, "%s ", argv[i]);
			}
			fprintf(stderr, "\n");
			goto cleanup_main;
		}
		topology.fanout = std::stoi(argv[2]);

		{ auto rootnode = Frontend{ctx, app_id, session_id, manifest_id, leaf_argv, topology};
			rootnode.registerStream(0, "libfilter.so");

			auto const result = rootnode.runCommand(0, "send");
			fprintf(stderr, "result: '%s'\n", result.c_str());

			rootnode.runExit();
		}

		rc = 0;
	}

cleanup_main:
	if (app_id) {
		cti_deregisterApp(app_id);
	}

	return rc;
}