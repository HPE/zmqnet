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

#include "src/cti_argv.hpp"

struct TopotestArgv : public cti::Argv {
	using Option    = cti::Argv::Option;
	using Parameter = cti::Argv::Parameter;

	static constexpr Option Leaf { "leaf", 'l' };

	static constexpr Parameter LaunchType { "launch-type", 't' };
	static constexpr Parameter Fanout     { "fanout",      'f' };
	static constexpr Parameter Sync       { "sync",        's' };

	static constexpr GNUOption long_options[] = {
		Leaf,
		LaunchType, Fanout, Sync,
		long_options_done
	};
};

static constexpr auto TestStreamId = zmqnet::Stream::Id{1};

int run_sync_leafnode(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int node_id)
{
	fprintf(stderr, "running leafnode node %d, connect parent %s %s\n", node_id, upstreamControlEndpoint.c_str(), upstreamDataEndpoint.c_str());

	auto leafnode = zmqnet::SyncBackend{upstreamControlEndpoint, upstreamDataEndpoint, node_id};

	while (true) {
		auto const [stream, command] = leafnode.nextCommand();

		if (stream.id < 0) {
			return 0;

		} else {
			fprintf(stderr, "%d got command %s from stream %d\n", node_id, command.begin(), stream.id);
			assert(stream.id == TestStreamId);
			auto const result = std::to_string(node_id);
			leafnode.sendResult(stream, result.c_str(), result.length() + 1);
		}
	}
}

int run_async_leafnode(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int node_id)
{
	fprintf(stderr, "running leafnode node %d, connect parent %s %s\n", node_id, upstreamControlEndpoint.c_str(), upstreamDataEndpoint.c_str());

	auto leafnode = zmqnet::AsyncBackend{upstreamControlEndpoint, upstreamDataEndpoint, node_id};

	leafnode.registerCallback(TestStreamId, [node_id](zmqnet::Stream::Id const& streamId, zmqnet::ByteArray command) {
		fprintf(stderr, "%d got command %s from stream %d\n", node_id, command.begin(), streamId);
		assert(streamId == TestStreamId);
		auto const result = std::to_string(node_id);
		return zmqnet::make_ByteArray(result.c_str(), result.length() + 1);
	});

	leafnode.notifyReady();

	leafnode.waitForExit();

	return 0;
}

// Parse user-provided arguments
static auto parse_argv(int argc, char* const argv[])
{
	auto leaf_mode = false;
	auto launchType = std::string{};
	auto fanout = int{1};
	auto synchronous_mode = false;

	using TA = TopotestArgv;
	auto parsedArgv = cti::IncomingArgv<TA>{argc, argv};

	// Read options
	while (true) {
		auto [c, value] = parsedArgv.get_next();
		if (c <= 0) { break; }

		switch (c) {

		case TA::Leaf.val:
			leaf_mode = true;
			break;

		case TA::LaunchType.val:
			launchType = std::move(value);
			break;

		case TA::Fanout.val:
			fanout = std::stoi(value);
			break;

		case TA::Sync.val:
			synchronous_mode = (value == "yes");
			break;

		case '?':
		default:
			throw std::runtime_error("bad arg");
		}
	}

	// Rest of arguments are launcher / leaf arguments
	auto const [rest_argc, rest_argv] = parsedArgv.get_rest();
	return std::make_tuple(leaf_mode, std::move(launchType), fanout, synchronous_mode, rest_argc, rest_argv);
}

int main(int argc, char** argv)
{
	int rc = -1;
	int app_id = 0;

	if (argc < 2) {
		return -1;
	}

	auto const on_backend = (getenv("CTI_IAMBACKEND") != nullptr);

	if (on_backend) {
		fprintf(stderr, "topotest: leaf mode\n");

		auto const [upstreamControlEndpoint, upstreamDataEndpoint, node_id,
			leaf_argc, leaf_argv] = zmqnet::SyncBackend::getParams(argc, argv);

		auto const [leaf_mode, launchType, fanout, synchronous_mode, binary_argc, binary_argv] = parse_argv(leaf_argc, leaf_argv);

		assert(leaf_mode == true);
		assert((binary_argc == 0) && (binary_argv[0] == nullptr));

		if (synchronous_mode) {
			rc = run_sync_leafnode(upstreamControlEndpoint, upstreamDataEndpoint, node_id);
		} else {
			rc = run_async_leafnode(upstreamControlEndpoint, upstreamDataEndpoint, node_id);
		}

		goto cleanup_main;

	} else {
		fprintf(stderr, "topotest: frontend mode\n");

		auto const [leaf_mode, launchType, fanout, synchronous_mode, rest_argc, rest_argv] = parse_argv(argc, argv);

		assert(leaf_mode == false);

		auto topology = zmqnet::Topology
			{ .launch = {}
			, .fanout = fanout
		};
		if (launchType == "fcfs") {
			topology.launch = zmqnet::Topology::Fcfs;
		} else if (launchType == "layered") {
			topology.launch = zmqnet::Topology::Layered;
		} else if (launchType == "packed") {
			topology.launch = zmqnet::Topology::Packed;
		} else {
			throw std::runtime_error("bad launch type");
		}

		char const* launcher_args[rest_argc + 1];
		for (int i = 0; i < rest_argc; i++) {
			launcher_args[i] = rest_argv[i];
		}
		launcher_args[rest_argc + 0] = "./stop";
		launcher_args[rest_argc + 1] = nullptr;

		app_id = cti_launchApp(launcher_args, -1, -1, nullptr, nullptr, nullptr);
		if (!app_id) {
			throw std::runtime_error(cti_error_str());
		}

		auto const session_id = cti_createSession(app_id);
		auto const manifest_id = cti_createManifest(session_id);
		cti_addManifestBinary(manifest_id, "./topotest");
		cti_addManifestLibrary(manifest_id, "./libfilter.so");

		auto leafArgv = cti::OutgoingArgv<TopotestArgv>{"topotest"};
		leafArgv.add(TopotestArgv::Leaf);

		auto hostsPlacement = std::unique_ptr<cti_hostsList_t, decltype(&cti_destroyHostsList)>
			{ cti_getAppHostsPlacement(app_id)
			, cti_destroyHostsList
		};

		auto some_nodes = std::set<std::string>{};
		for (int i = 0; i < hostsPlacement->numHosts; i += 2) {
			some_nodes.emplace(hostsPlacement->hosts[i].hostname);
		}

		if (synchronous_mode) {
			leafArgv.add(TopotestArgv::Sync, "yes");

			auto const raw_leaf_argv = (const char* const*)leafArgv.get();
			auto rootnode = zmqnet::SyncFrontend{app_id, session_id, manifest_id, topology, raw_leaf_argv};

			auto allNodesCommunicator = rootnode.getAllNodesCommunicator();
			auto stream = rootnode.registerStream(allNodesCommunicator, TestStreamId, "libfilter.so", "run", zmqnet::sync::WaitForAll{});

			{ auto someNodesCommunicator = rootnode.registerCommunicator(some_nodes);
				auto someNodesStream = rootnode.registerStream(someNodesCommunicator, TestStreamId, "libfilter.so", "run", zmqnet::sync::WaitForAll{});

				auto const result = rootnode.runCommand(someNodesStream, "send", 5);
				fprintf(stderr, "some nodes result: '%s'\n", result.asBytes().begin());
			}

			{ auto const result = rootnode.runCommand(stream, "send", 5);
				fprintf(stderr, "all nodes result: '%s'\n", result.asBytes().begin());
			}

			{ auto someNodesCommunicator = rootnode.registerCommunicator(some_nodes);
				auto someNodesStream = rootnode.registerStream(someNodesCommunicator, TestStreamId, "libfilter.so", "run", zmqnet::sync::WaitForAll{});

				auto const result = rootnode.runCommand(someNodesStream, "send", 5);
				fprintf(stderr, "some nodes result: '%s'\n", result.asBytes().begin());
			}

			{ auto const result = rootnode.runCommand(stream, "send", 5);
				fprintf(stderr, "all nodes result: '%s'\n", result.asBytes().begin());
			}

			rootnode.runExit();

		} else {
			leafArgv.add(TopotestArgv::Sync, "no");

			auto result_mutex = std::mutex{};
			auto result_cv = std::condition_variable{};
			auto results_received = std::atomic<int>{0};
			auto expected_results = int{0};

			auto const raw_leaf_argv = (const char* const*)leafArgv.get();
			auto rootnode = zmqnet::AsyncFrontend{app_id, session_id, manifest_id, topology, raw_leaf_argv};

			auto print_callback = [&results_received, &result_cv](zmqnet::Stream::Id const& streamId, zmqnet::ByteArrayPtr result) {
				assert(streamId == TestStreamId);
				fprintf(stderr, "result: '%s'\n", result.begin());

				results_received++;
				result_cv.notify_one();
			};

			auto allNodesCommunicator = rootnode.getAllNodesCommunicator();
			rootnode.registerStreamCallback(TestStreamId, print_callback);
			auto stream = rootnode.registerStream(allNodesCommunicator, TestStreamId, "libfilter.so", "run", zmqnet::sync::WaitForAll{});

			{ auto someNodesCommunicator = rootnode.registerCommunicator(some_nodes);
				auto someNodesStream = rootnode.registerStream(someNodesCommunicator, TestStreamId, "libfilter.so", "run", zmqnet::sync::WaitForAll{});

				rootnode.sendCommand(someNodesStream, "send", 5);
				expected_results++;
			}

			rootnode.sendCommand(stream, "send", 5);
			expected_results++;

			{ auto someNodesCommunicator = rootnode.registerCommunicator(some_nodes);
				auto someNodesStream = rootnode.registerStream(someNodesCommunicator, TestStreamId, "libfilter.so", "run", zmqnet::sync::WaitForAll{});

				rootnode.sendCommand(someNodesStream, "send", 5);
				expected_results++;
			}

			rootnode.sendCommand(stream, "send", 5);
			expected_results++;

			{ auto result_lock = std::unique_lock<std::mutex>{result_mutex};
				result_cv.wait(result_lock, [&results_received, expected_results]() {
					return results_received.load() == expected_results;
				});
			}

			rootnode.runExit();
		}

		rc = 0;

		goto cleanup_main;
	}

cleanup_main:
	if (app_id) {
		cti_deregisterApp(app_id);
	}

	return rc;
}