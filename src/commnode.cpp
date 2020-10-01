/*
 * Copyright 2011-2020 Hewlett Packard Enterprise Development LP.
 */

#include <cassert>

#include <string>
#include <vector>
#include <future>

#include "zmqnet.hpp"

#include "argv_defs.hpp"

auto ctx = zmq::context_t{};

// Parse user-provided arguments
static auto parse_argv(int argc, char* const argv[])
{
	auto dealerEndpoint = std::string{};
	auto leaf_argv = argv + argc;

	using CA = CommnodeArgv;
	auto parsedArgv = cti::IncomingArgv<CA>{argc, argv};

	// Read FE-passed options
	while (true) {
		auto const [c, value] = parsedArgv.get_next();
		if (c <= 0) { break; }

		switch (c) {

		case CA::DealerEndpoint.val:
			dealerEndpoint = value;
			break;

		case '?':
		default:
			throw std::runtime_error("bad arg");
		}
	}

	// Rest of arguments are leaf arguments
	leaf_argv = parsedArgv.get_rest();
	if (leaf_argv[0] == nullptr) {
		throw std::runtime_error("leaf binary not provided");
	}

	if (dealerEndpoint.empty()) {
		throw std::runtime_error("topology server endpoint not provided");
	}
	return std::make_tuple(dealerEndpoint, leaf_argv);
}

int main(int argc, char* const argv[])
{
	// Read arguments from frontend
	auto const [dealerEndpoint, leaf_argv] = parse_argv(argc, argv);

	// Start commnode
	auto commnode = Commnode{ctx, dealerEndpoint, leaf_argv};

	// Command relay loop
	while (true) {
		// Receive and relay next command from parent node
		auto const command = commnode.nextCommand();

		// Collect results from child nodes
		auto const childrenResults = commnode.collectResults();

		// Exit if commanded
		if (command == "exit") {
			return 0;
		}

		// Run aggregation
		auto result = std::string{"{ "};
		for (auto&& child_result : childrenResults) {
			assert(child_result != "ready");
			result += child_result + " ";
		}
		result += "}";

		// Send aggregated result
		commnode.sendResult(result);
	}
}
