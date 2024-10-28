/*
 * Copyright 2021 Hewlett Packard Enterprise Development LP.
 */

#include <cassert>

#include <unistd.h>
#include <wait.h>

#include <string>
#include <vector>
#include <future>
#include <unordered_map>
#include <thread>

#include "libzmqnet_tests.hpp"

template <typename Params>
int run_sync_leafnode(Params const& beParams)
{
	fprintf(stderr, "running leafnode node %d, connect parent %s %s\n",
		beParams.node_id, beParams.upstreamControlEndpoint.c_str(), beParams.upstreamDataEndpoint.c_str());

	auto leafnode = zmqnet::SyncBackend{beParams};

	while (true) {
		auto const [stream, command] = leafnode.nextCommand();

		if (stream.id < 0) {
			return 0;

		} else {
			fprintf(stderr, "%d got command %s from stream %d\n", beParams.node_id, command.begin(), stream.id);
			assert(stream.id == TestStreamId);
			auto const result = std::to_string(beParams.node_id);
			leafnode.sendResult(stream, result.c_str(), result.length() + 1);
		}
	}
}

template <typename Params>
int run_async_leafnode(Params const& beParams)
{
	fprintf(stderr, "running leafnode node %d, connect parent %s %s\n",
		beParams.node_id, beParams.upstreamControlEndpoint.c_str(), beParams.upstreamDataEndpoint.c_str());

	auto leafnode = zmqnet::AsyncBackend{beParams};

	auto const node_id = beParams.node_id;
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

template <typename Params>
int empty_sync_leafnode(Params const& beParams)
{
	fprintf(stderr, "running empty node %d, connect parent %s %s. drops all messages\n",
		beParams.node_id, beParams.upstreamControlEndpoint.c_str(), beParams.upstreamDataEndpoint.c_str());

	auto leafnode = zmqnet::SyncBackend{beParams};

	while (true) {
		auto const [stream, command] = leafnode.nextCommand();

		if (stream.id < 0) {
			return 0;

		} else {
			fprintf(stderr, "%d dropping command %s from stream %d\n",
				beParams.node_id, command.begin(), stream.id);
		}
	}
}

template <typename Params>
int empty_async_leafnode(Params const& beParams)
{
	fprintf(stderr, "running leafnode node %d, connect parent %s %s. drops all messages\n",
		beParams.node_id, beParams.upstreamControlEndpoint.c_str(), beParams.upstreamDataEndpoint.c_str());

	auto leafnode = zmqnet::AsyncBackend{beParams};

	auto const node_id = beParams.node_id;
	leafnode.registerCallback(TestStreamId, [node_id](zmqnet::Stream::Id const& streamId, zmqnet::ByteArray command) {
		fprintf(stderr, "%d dropping command %s from stream %d\n",
			node_id, command.begin(), streamId);
		return zmqnet::ByteArray{};
	});

	leafnode.notifyReady();

	leafnode.waitForExit();

	return 0;
}

template <typename Params>
void run_sync_leafnode_linger_test(Params const& beParams)
{
	fprintf(stderr, "running leafnode node %d linger test, connect parent %s %s\n",
		beParams.node_id, beParams.upstreamControlEndpoint.c_str(), beParams.upstreamDataEndpoint.c_str());

	auto leafnode = zmqnet::SyncBackend{beParams};

	auto const [stream, command] = leafnode.nextCommand();

	if (stream.id < 0) {
		return;
	}

	fprintf(stderr, "%d got command %s from stream %d\n", beParams.node_id, command.begin(), stream.id);
	assert(stream.id == TestStreamId);

	// Generate long result
	auto result = []() {
		auto ss = std::stringstream{};
		for (int i = 0; i < 4096; i++) {
			for (int j = 0; j < 64; j++) {
				ss << i << " ";
			}
		}
		return ss.str();
	}();
	fprintf(stderr, "result should have strlen %zu\n", result.length());
	leafnode.sendResult(stream, result.c_str(), result.length() + 1);

	// From the perspective of the frontend, exit unexpectedly
	// Message still should be enqueued to send
	leafnode.forceDisconnect();
}

// Parse user-provided arguments
static auto parse_argv(int argc, char* const argv[])
{
	auto test_linger = false;
	auto synchronous_mode = false;
	auto timeout_node_id = int{-1};

	using BA = BackendArgv;
	auto parsedArgv = cti::IncomingArgv<BA>{argc, argv};

	// Read options
	while (true) {
		auto [c, value] = parsedArgv.get_next();
		if (c <= 0) { break; }

		switch (c) {

		case BA::TestLinger.val:
			test_linger = true;
			break;

		case BA::Sync.val:
			synchronous_mode = (value == "yes");
			break;

		case BA::TimeoutNodeId.val:
			timeout_node_id = std::stoi(value);
			break;

		case '?':
		default:
			throw std::runtime_error("bad arg");
		}
	}

	// Rest of arguments are launcher / leaf arguments
	auto const [rest_argc, rest_argv] = parsedArgv.get_rest();
	return std::make_tuple(test_linger, synchronous_mode, timeout_node_id, rest_argc, rest_argv);
}

int main(int argc, char** argv)
{
	int rc = -1;

	if (argc < 2) {
		return -1;
	}

	fprintf(stderr, "topotest: leaf mode\n");

	auto const [beParams, leaf_argc, leaf_argv] = zmqnet::SyncBackend::getParams(argc, argv);

	auto const [test_linger, synchronous_mode, timeout_node_id, binary_argc, binary_argv]
		= parse_argv(leaf_argc, leaf_argv);
	assert((binary_argc == 0) && (binary_argv[0] == nullptr));

	if (test_linger) {
		// Run test and exit
		run_sync_leafnode_linger_test(beParams);
		return 0;
	}

	// Normal test, send node ID in response to commands
	if (timeout_node_id != beParams.node_id) {

		if (synchronous_mode) {
			rc = run_sync_leafnode(beParams);
		} else {
			rc = run_async_leafnode(beParams);
		}

	// Timeout test, this is the node that will ignore incoming commands
	} else {

		if (synchronous_mode) {
			rc = empty_sync_leafnode(beParams);
		} else {
			rc = empty_async_leafnode(beParams);
		}

	}

	return rc;
}