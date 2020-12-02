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

int run_sync_leafnode(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int rank)
{
	fprintf(stderr, "running leafnode rank %d, connect parent %s %s\n", rank, upstreamControlEndpoint.c_str(), upstreamDataEndpoint.c_str());

	auto leafnode = zmqnet::SyncBackend{upstreamControlEndpoint, upstreamDataEndpoint, rank};

	while (true) {
		auto const [streamId, command] = leafnode.nextCommand();

		if (streamId < 0) {
			return 0;

		} else {
			fprintf(stderr, "%d got command %s from stream %d\n", rank, command.begin(), streamId);
			assert(streamId == 0);
			auto const result = std::to_string(rank);
			leafnode.sendResult(streamId, result.c_str(), result.length() + 1);
		}
	}
}

int run_async_leafnode(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int rank)
{
	fprintf(stderr, "running leafnode rank %d, connect parent %s %s\n", rank, upstreamControlEndpoint.c_str(), upstreamDataEndpoint.c_str());

	auto leafnode = zmqnet::AsyncBackend{upstreamControlEndpoint, upstreamDataEndpoint, rank};

	leafnode.registerCallback(0, [rank](zmqnet::Stream::Id const& streamId, zmqnet::ByteArray command) {
		fprintf(stderr, "%d got command %s from stream %d\n", rank, command.begin(), streamId);
		assert(streamId == 0);
		auto const result = std::to_string(rank);
		return zmqnet::make_ByteArray(result.c_str(), result.length() + 1);
	});

	leafnode.notifyReady();

	leafnode.waitForExit();

	return 0;
}

int main(int argc, char** argv)
{
	int rc = -1;
	int app_id = 0;

	if (argc < 2) {
		goto cleanup_main;
	}

	fprintf(stderr, "mode: %s\n", argv[1]);

	if ((argc > 4) && !strcmp("leaf", argv[4])) {
		auto const [upstreamControlEndpoint, upstreamDataEndpoint, rank,
			rest_argc, rest_argv] = zmqnet::Backend::getParams(argc, argv);
		assert(rest_argc == argc - 3);
		assert(!strcmp("leaf", rest_argv[1]));

		auto const synchronousMode = !strcmp(argv[3], "sync");
		if (synchronousMode) {
			rc = run_sync_leafnode(upstreamControlEndpoint, upstreamDataEndpoint, rank);
		} else {
			rc = run_async_leafnode(upstreamControlEndpoint, upstreamDataEndpoint, rank);
		}

		goto cleanup_main;

	} else {

		char const* launcher_args[(argc - 4) + 1];
		for (int i = 0; i < argc - 4; i++) {
			launcher_args[i] = argv[4 + i];
		}
		launcher_args[argc - 4 + 0] = "./stop";
		launcher_args[argc - 4 + 1] = nullptr;

		app_id = cti_launchApp(launcher_args, -1, -1, nullptr, nullptr, nullptr);
		if (!app_id) {
			throw std::runtime_error(cti_error_str());
		}

		auto const session_id = cti_createSession(app_id);
		auto const manifest_id = cti_createManifest(session_id);
		cti_addManifestBinary(manifest_id, "./topotest");
		cti_addManifestLibrary(manifest_id, "./libfilter.so");

		char const* leaf_argv[] = { "topotest", "leaf", nullptr };

		auto topology = zmqnet::Topology{};
		if (!strcmp("fcfs", argv[1])) {
			topology.launch = zmqnet::Topology::Fcfs;
		} else if (!strcmp("layered", argv[1])) {
			topology.launch = zmqnet::Topology::Layered;
		} else {
			fprintf(stderr, "bad args: ");
			for (int i = 0; i < argc; i++) {
				fprintf(stderr, "%s ", argv[i]);
			}
			fprintf(stderr, "\n");
			goto cleanup_main;
		}
		topology.fanout = std::stoi(argv[2]);

		auto const synchronousMode = !strcmp(argv[3], "sync");

		if (synchronousMode) {
			auto rootnode = zmqnet::SyncFrontend{app_id, session_id, manifest_id, leaf_argv, topology};
			auto stream = rootnode.registerStream(0, "libfilter.so", "run");

			auto const result = rootnode.runCommand(stream, "send", 5);
			fprintf(stderr, "result: '%s'\n", result.asBytes().begin());

			rootnode.runExit();

		} else {
			auto result_mutex = std::mutex{};
			auto result_cv = std::condition_variable{};
			auto result_received = std::atomic<bool>{false};

			auto rootnode = zmqnet::AsyncFrontend{app_id, session_id, manifest_id, leaf_argv, topology};
			auto stream = rootnode.registerStream(0, "libfilter.so", "run",
				[&result_received, &result_cv](zmqnet::Stream::Id const& streamId, kj::Array<kj::byte> result) {
					assert(streamId == 0);
					fprintf(stderr, "result: '%s'\n", result.asBytes().begin());

					result_received = true;
					result_cv.notify_one();
				});

			rootnode.sendCommand(stream, "send", 5);

			{ auto result_lock = std::unique_lock<std::mutex>{result_mutex};
				result_cv.wait(result_lock,
					[&result_received] { return result_received.load(); });
			}

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