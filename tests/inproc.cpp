/*
 * Copyright 2011-2020 Hewlett Packard Enterprise Development LP.
 */

#include <cassert>

#include <string>
#include <vector>
#include <future>

#include "zmqnet.hpp"

auto ctx = zmq::context_t{};

template <typename CommnodeImpl>
int run_commnode(CommnodeImpl&& commnode, size_t const num_children)
{
	int rc = -1;

	commnode.children_ready_loop(num_children);
	commnode.send_ready();

	while (true) {
		auto command = commnode.recv_command();
		if (num_children > 0) {
			commnode.send_command(command);
		}

		if (command == "exit") {
			rc = 0;
			goto cleanup_run_commnode;
		}

		auto const children_results = commnode.recv_results(num_children);

		auto result = std::string{"{ "};
		for (auto&& child_result : children_results) {
			assert(child_result != "ready");
			result += child_result + " ";
		}
		result += "}";

		commnode.send_result(result);
	}

cleanup_run_commnode:
	return rc;
}

static void gen_futures(std::vector<std::future<int>>& futures,
	int const fanout, int const depth, std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint)
{
	if (depth == 0) {
		return;
	}

	for (int i = 0; i < fanout; i++) {
		auto commnode = Commnode<InprocDownstreamLink>{ctx, upstreamControlEndpoint, upstreamDataEndpoint};
		auto const downstreamControlEndpoint = commnode.getControlEndpoint();
		auto const downstreamDataEndpoint = commnode.getDataEndpoint();

		futures.emplace_back(std::async(std::launch::async,
			run_commnode<decltype(commnode)>, std::move(commnode),
			(depth > 1) ? fanout : 0));

		gen_futures(futures, fanout, depth - 1, downstreamControlEndpoint, downstreamDataEndpoint);
	}
}

static void test(int const depth, int const fanout)
{
	ctx = zmq::context_t{};

	auto rootnode = Rootnode<InprocDownstreamLink>{ctx};
	auto const downstreamControlEndpoint = rootnode.getControlEndpoint();
	auto const downstreamDataEndpoint = rootnode.getDataEndpoint();

	auto futures = std::vector<std::future<int>>{};
	gen_futures(futures, fanout, depth, downstreamControlEndpoint, downstreamDataEndpoint);

	auto const num_children = fanout;
	rootnode.children_ready_loop(num_children);

	rootnode.send_command("send");

	auto const children_results = rootnode.recv_results(fanout);

	for (auto&& result : children_results) {
		fprintf(stderr, "result: '%s'\n", result.c_str());
	}

	rootnode.send_command("exit");
	for (auto&& future : futures) {
		if (future.get()) {
			fprintf(stderr, "failure!\n");
		}
	}
}

int main(int argc, char const* const* argv)
{
	for (int depth = 1; depth < 4; depth++) {
		for (int fanout = 1; fanout < 5; fanout++) {
			test(depth, fanout);
		}
	}

	auto downstreamLink = TcpDownstreamLink{ctx};
	fprintf(stderr, "control: %s data: %s\n", downstreamLink.controlEndpoint.c_str(), downstreamLink.dataEndpoint.c_str());

	return 0;
}