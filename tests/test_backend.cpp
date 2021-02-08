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

#include "zmqnet.hpp"

#include "src/cti_argv.hpp"

struct BackendArgv : public cti::Argv {
	using Option    = cti::Argv::Option;
	using Parameter = cti::Argv::Parameter;

	static constexpr Parameter Sync { "sync", 's' };

	static constexpr GNUOption long_options[] = {
		Sync,
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
	auto synchronous_mode = false;

	using BA = BackendArgv;
	auto parsedArgv = cti::IncomingArgv<BA>{argc, argv};

	// Read options
	while (true) {
		auto [c, value] = parsedArgv.get_next();
		if (c <= 0) { break; }

		switch (c) {

		case BA::Sync.val:
			synchronous_mode = (value == "yes");
			break;

		case '?':
		default:
			throw std::runtime_error("bad arg");
		}
	}

	// Rest of arguments are launcher / leaf arguments
	auto const [rest_argc, rest_argv] = parsedArgv.get_rest();
	return std::make_tuple(synchronous_mode, rest_argc, rest_argv);
}

int main(int argc, char** argv)
{
	int rc = -1;

	if (argc < 2) {
		return -1;
	}

	fprintf(stderr, "topotest: leaf mode\n");

	auto const [upstreamControlEndpoint, upstreamDataEndpoint, node_id,
		leaf_argc, leaf_argv] = zmqnet::Backend::getParams(argc, argv);

	auto const [synchronous_mode, binary_argc, binary_argv] = parse_argv(leaf_argc, leaf_argv);
	assert((binary_argc == 0) && (binary_argv[0] == nullptr));

	if (synchronous_mode) {
		rc = run_sync_leafnode(upstreamControlEndpoint, upstreamDataEndpoint, node_id);
	} else {
		rc = run_async_leafnode(upstreamControlEndpoint, upstreamDataEndpoint, node_id);
	}

	return rc;
}