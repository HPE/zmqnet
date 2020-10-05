/*
 * Copyright 2011-2020 Hewlett Packard Enterprise Development LP.
 */

#include <cassert>

#include <string>
#include <vector>
#include <future>
#include <thread>

#include "zmqnet.hpp"

#include "cti_dlopen.hpp"
#include "cti_split.hpp"

#include "argv_defs.hpp"

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

int stream_worker(std::string const& taskEndpoint, std::string const& resultEndpoint,
	StreamId const& streamId, int numChildren,
	std::string const& filterPath)
{
	int rc = -1;

	auto ctx = zmq::context_t{};

	auto task_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	task_socket.connect(taskEndpoint);

	auto result_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	result_socket.connect(resultEndpoint);

	using FilterRunType = std::string(std::vector<std::string> const&);
	auto filterHandle = cti::Dlopen::Handle{filterPath};
	auto filterRun = filterHandle.load<FilterRunType>("run");

	auto messages = std::vector<std::string>{};
	messages.reserve(numChildren);

	while (true) {
		auto const [recievedStreamId, message] = cti::split::string<2>(recv_string(task_socket, zmq::recv_flags::none));

		if (streamId < 0) {
			break;
		}
		if (streamId != recievedStreamId) {
			goto cleanup_stream_worker;
		}

		messages.emplace_back(std::move(message));
		if (messages.size() == numChildren) {
			auto result = filterRun(std::move(messages));
			messages.clear();
			messages.reserve(numChildren);

			auto message = std::to_string(streamId) + " " + std::move(result);
			send_string(result_socket, std::move(message), zmq::send_flags::none);
		}
	}

	rc = 0;

cleanup_stream_worker:
	return rc;
}

std::vector<std::string> Commnode::collectResults(StreamId const& streamId, std::string const& command)
{
	// Relay command to children
	fprintf(stderr, "relay command downstream %s\n", command.c_str());
	m_downstream.send_command(std::to_string(streamId) + " " + command);

	// Remove and verify stream ID
	auto results = m_downstream.recv_results(m_numChildren);
	for (auto&& result : results) {
		auto [streamString, command] = cti::split::string<2>(std::move(result));
		assert(std::stoi(streamString) == streamId);
		result = std::move(command);
	}

	return results;
}

std::string Commnode::filterResults(StreamId const& streamId, std::vector<std::string> const& results)
{
	return m_filters.at(streamId)->run(results);
}

void Commnode::sendResult(StreamId const& streamId, std::string const& result)
{
	m_upstream.send_result(std::to_string(streamId) + " " + result);
}

int main(int argc, char* const argv[])
{
	int rc = -1;
	auto ctx = zmq::context_t{};

	// Read arguments from frontend
	auto const [dealerEndpoint, leaf_argv] = parse_argv(argc, argv);

	// Create upstream and downstream sockets
	auto upstream = ProducerNode{ctx};
	auto downstream = ConsumerNode<TcpDownstreamLink>{};

	// Create stream task work sockets
	auto streamSockets = std::unordered_map<StreamId, zmq::socket_t>{};
	auto result_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	auto const resultEndpoint = result_socket.get(zmq::sockopt::last_endpoint);
	auto streamWorkers = std::vector<std::future<int>>{};

	// Topology and child counting
	auto numChildren = int{};
	auto localChildPids = std::vector<pid_t>{};

	// Start network connection process

	// Format externally-accessible endpoints
	auto const [controlEndpoint, dataEndpoint] = formatEndpoints(downstream, cti_be_getNodeHostname());
	fprintf(stderr, "listening on %s / %s\n", controlEndpoint.c_str(), dataEndpoint.c_str());

	// Send endpoints to topology server and receive parent's endpoints and number of network children
	auto const [parentControlEndpoint, parentDataEndpoint, numNetworkChildren] =
		exchangeTopologyData(ctx, dealerEndpoint, controlEndpoint, dataEndpoint);
	numChildren += numNetworkChildren;

	// Connect to parent's endpoints
	fprintf(stderr, "connecting to parent %s / %s\n", parentControlEndpoint.c_str(), parentDataEndpoint.c_str());
	upstream.connect(parentControlEndpoint, parentDataEndpoint);

	// Proxy local worker result socket to upstream socket
	auto proxyTask = std::async(std::launch::async, [&]() {
		zmq::proxy(result_socket, upstream.data_socket);
		return 0;
	});

	// Determine number of local children, one per PE on node
	auto const firstPE = cti_be_getNodeFirstPE();
	auto const localPEs = cti_be_getNodePEs();
	numChildren += localPEs;

	// Launch local children with provided leaf binary
	auto const leafPath = std::string{cti_be_getBinDir()} + "/" + leaf_argv[0];
	fprintf(stderr, "forking %d local leaf processes\n", localPEs);
	for (int rank = firstPE; rank < firstPE + localPEs; rank++) {

		if (auto const forkedPid = fork()) {
			// Add launched process to cleanup list
			localChildPids.emplace_back(forkedPid);

		} else {
			// Prepend endpoints and rank number to provided leaf arguments
			auto completeLeafArgv = cti::ManagedArgv
				{ leaf_argv[0]
				, controlEndpoint
				, dataEndpoint
				, std::to_string(rank)
			};

			// Copy provided arguments
			for (auto argptr = leaf_argv + 1; *argptr != nullptr; argptr++) {
				completeLeafArgv.add(*argptr);
			}

			// Exec leaf binary
			execvp(leafPath.c_str(), completeLeafArgv.get());
			perror("execvp");
			_exit(-1);
		}
	}

	// Wait for all children to connect and send ready
	fprintf(stderr, "waiting for %d children\n", numChildren);
	downstream.children_ready_loop(numChildren);

	// Send ready to parent
	fprintf(stderr, "sending ready to parent\n");
	upstream.send_ready();

	// Result relay thread
	auto resultRelayTask = std::launch(std::launch::async, [](std::string const& 
		// Receive next result from children
		fprintf(stderr, "waiting for result from downstream\n");
		auto message = recv_string(downstream.data_socket, zmq::recv_flags::none);

		// Remove stream ID
		auto [streamString, result] = cti::split::string<2>(message);
		auto const streamId = std::stoi(streamString);

		// Determine if exit confirmation was received
		if (streamId < 0) {
			// Send exit confirmation message to parent
			for (auto&& task_socket : taskSockets) {
				upstream.send_result(message);
			}

			break;

		// Relay result to stream worker
		} else {
			send_string(taskSockets.at(streamId), message);
		}
	}

	// Command relay loop

	while (true) {
		// Receive next command from parent
		fprintf(stderr, "waiting for command from upstream\n");
		auto message = upstream.recv_command();

		// Relay message to children
		send_string(downstream.control_socket, message, zmq::send_flags::none);

		// Remove stream ID
		auto [streamString, command] = cti::split::string<2>(message);
		auto const streamId = std::stoi(streamString);

		// Determine if exit command was received
		if (streamId < 0) {
			// Send exit message to all stream workers
			for (auto&& task_socket : taskSockets) {
				send_string(task_socket, message, zmq::send_flags::none);
			}

			break;

		// Determine if new stream is to be registered
		} else if (taskSockets.find(streamId) == taskSockets.end()) {
			// Filter path is send as command body
			auto const& filterPath = command;

			// Create new task distribution socket
			auto task_socket = zmq::socket_t{ctx, zmq::socket_type::push};
			task_socket.bind("inproc://" + gen_uuid());

			// Create new stream task worker
			streamWorkers.emplace_back(std::async(std::launch::async, 
				stream_worker, task_socket.get(zmq::sockopt::last_endpoint), resultEndpoint,
					streamId, numChildren, filterPath));

			taskSockets.emplace(streamId, std::move(task_socket));
		}
	}

	rc = 0;

cleanup_main:
	// Wait on stream worker threads
	fprintf(stderr, "waiting for stream worker threads\n");
	for (auto&& streamWorker : streamWorkers) {
		auto const stream_rc = streamWorker.get();
		if (stream_rc) {
			fprintf(stderr, "stream worker failed with code %d\n", stream_rc);
			rc = -1;
		}
	}

	// Close network connections to end proxy
	taskSockets.clear();
	result_socket.close();

	// Wait for proxy task to complete
	fprintf(stderr, "waiting for proxy task\n");
	(void)proxyTask.get();

	// Wait on all local child PIDs
	fprintf(stderr, "waiting for child pids\n");
	for (auto&& pid : localChildPids) {
		int status;
		if (waitpid(pid, &status, 0) || (WIFEXITED(status) && WEXITSTATUS(status))) {
			fprintf(stderr, "pid %d failed!\n", pid);
			rc = -1;
		}
	}

	return rc;
}
