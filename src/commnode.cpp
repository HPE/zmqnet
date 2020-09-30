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
	auto controlEndpoint = std::string{};
	auto dataEndpoint = std::string{};
	auto numChildren = int{-1};

	using CA = CommnodeArgv;
	auto parsedArgv = cti::IncomingArgv<CA>{argc, argv};

	// Read FE-passed options
	while (true) {
		auto const [c, value] = parsedArgv.get_next();
		if (c <= 0) { break; }

		switch (c) {

		case CA::ControlEndpoint.val:
			controlEndpoint = value;
			break;

		case CA::DataEndpoint.val:
			dataEndpoint = value;
			break;

		case CA::NumChildren.val:
			numChildren = std::stoi(value);
			break;

		case '?':
		default:
			throw std::runtime_error("bad arg");
		}
	}

	if (controlEndpoint.empty() || dataEndpoint.empty()) {
		throw std::runtime_error("control / data endpoints not provided");
	}
	if (numChildren < 0) {
		throw std::runtime_error("topology not provided");
	}
	return std::make_tuple(controlEndpoint, dataEndpoint, numChildren);
}

int main(int argc, char** argv)
{
	int rc = -1;

	auto const [controlEndpoint, dataEndpoint, numChildren] = parse_argv(argc, argv);

cleanup_main:

	return rc;
}
