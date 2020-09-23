/*
 * Copyright 2011-2020 Hewlett Packard Enterprise Development LP.
 */

#include <cassert>

#include <string>
#include <vector>
#include <future>

#include "zmqnet.hpp"

auto ctx = zmq::context_t{};

int run_commnode(std::string const& parentEndpoint, std::string const& endpoint, size_t const num_children)
{
		int rc = -1;

		auto parent_control_socket = zmq::socket_t{ctx, zmq::socket_type::sub};
		parent_control_socket.connect("inproc://" + parentEndpoint + "-control");
		parent_control_socket.set(zmq::sockopt::subscribe, "");

		auto parent_data_socket = zmq::socket_t{ctx, zmq::socket_type::push};
		parent_data_socket.connect("inproc://" + parentEndpoint + "-data");

		auto children_control_socket = zmq::socket_t{ctx, zmq::socket_type::pub};
		if (num_children > 0) {
				children_control_socket.bind("inproc://" + endpoint + "-control");
		}

		auto children_data_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
		if (num_children > 0) {
				children_data_socket.bind("inproc://" + endpoint + "-data");
		}

		for (size_t connected_children = 0; connected_children < num_children;) {
				auto received_result = zmq::message_t{};
				auto maybe_bytes = children_data_socket.recv(received_result, zmq::recv_flags::none);
				if (!maybe_bytes) {
						continue;
				}

				assert(received_result.to_string_view() == "ready");
				connected_children++;
		}
		parent_data_socket.send(zmq::str_buffer("ready"), zmq::send_flags::none);

		while (true) {
				auto recieved_command = zmq::message_t{};
				auto maybe_bytes = parent_control_socket.recv(recieved_command, zmq::recv_flags::none);

				if (!maybe_bytes) {
						continue;
				}

				auto const command = recieved_command.to_string();

				if (num_children > 0) {
						children_control_socket.send(recieved_command, zmq::send_flags::none);
				}

				if (command == "exit") {
						rc = 0;
						goto cleanup_run_commnode;
				}

				auto children_results = std::vector<std::string>{};

				while (children_results.size() < num_children) {
						auto received_result = zmq::message_t{};
						auto maybe_bytes = children_data_socket.recv(received_result, zmq::recv_flags::none);

						if (!maybe_bytes) {
								continue;
						}

						children_results.emplace_back(received_result.to_string_view());
				}

				auto result = endpoint + ": { ";
				for (auto&& child_result : children_results) {
						result += child_result + " ";
				}
				result += "}";

				parent_data_socket.send(zmq::message_t{result.c_str(), result.length()}, zmq::send_flags::none);
		}

cleanup_run_commnode:
		return rc;
}

static void gen_futures(std::vector<std::future<int>>& futures, int const depth,
		int const fanout, std::string const& parentEndpoint)
{
		if (depth == 0) {
				return;
		}

		for (int i = 0; i < fanout; i++) {
				auto const endpoint = parentEndpoint + std::to_string(i);
				futures.emplace_back(std::async(std::launch::async,
						run_commnode, parentEndpoint, endpoint,
						(depth > 1) ? fanout : 0));
				gen_futures(futures, depth - 1, fanout, endpoint);
		}
}

static void test(int const depth, int const fanout)
{
		ctx = zmq::context_t{};

		auto control_socket = zmq::socket_t{ctx, zmq::socket_type::pub};
		control_socket.bind("inproc://-control");

		auto data_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
		data_socket.bind("inproc://-data");

		auto futures = std::vector<std::future<int>>{};

		gen_futures(futures, depth, fanout, "0");

		futures.emplace_back(std::async(std::launch::async,
				run_commnode, "", "0", fanout));

		while (true) {
				auto received_result = zmq::message_t{};
				auto maybe_bytes = data_socket.recv(received_result, zmq::recv_flags::none);
				if (!maybe_bytes) {
						continue;
				}

				assert(received_result.to_string_view() == "ready");
				break;
		}

		control_socket.send(zmq::str_buffer("send"), zmq::send_flags::none);

		while (true) {
				auto received_result = zmq::message_t{};
				auto maybe_bytes = data_socket.recv(received_result, zmq::recv_flags::none);

				if (!maybe_bytes) {
						continue;
				}

				auto const result = received_result.to_string();
				fprintf(stderr, "result: '%s'\n", result.c_str());
				break;
		}

		control_socket.send(zmq::str_buffer("exit"), zmq::send_flags::none);
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

		return 0;
}