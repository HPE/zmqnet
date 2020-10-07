/*
 * Copyright 2011-2020 Hewlett Packard Enterprise Development LP.
 */

#include <unistd.h>
#include <wait.h>

#include <cassert>

#include <string>
#include <vector>
#include <future>
#include <thread>

#include "zmqnet.hpp"

#include "cti_dlopen.hpp"

#include "common_tools_be.h"

#include "argv_defs.hpp"

#include "libzmqnet/libzmqnet.hpp"

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

namespace workers
{

// Select stream task based on incoming message's stream ID, and send message to stream task socket
// Returns control of downstream data socket upon completion
static zmq::socket_t distribute_stream_inputs(zmq::context_t& ctx, zmq::socket_t&& data_socket, int numChildren)
{
	auto taskSockets = std::unordered_map<StreamId, zmq::socket_t>{};

	while (true) {
		// Receive next result from children
		fprintf(stderr, "waiting for result from downstream\n");
		auto result = recv_message(data_socket);
		auto resultReader = make_MessageReader(result);
		auto response = resultReader.getRoot<NetworkResponse>();

		// If child exited, forward exit message to every rank and continue
		if (response.isExitAck()) {
			fprintf(stderr, "sending exit message to every stream\n");
			for (auto&& [streamId, task_socket] : taskSockets) {
				task_socket.send(result, zmq::send_flags::none);
			}

			// Remove one child
			numChildren--;

			// If no children left, exit
			if (numChildren == 0) {
				break;
			}

			continue;
		}

		// Response is either stream acknowledgement or user result
		assert(response.isStreamAck() || response.isUserResult());

		// Extract stream ID to select stream worker
		auto const streamId = (response.isStreamAck())
			? response.getStreamAck().getStreamId()
			: response.getUserResult().getStreamId();

		// Get socket for given stream ID
		auto const idSocketPair = taskSockets.find(streamId);

		// Create new stream task connection with known endpoint format, if needed
		if (idSocketPair == taskSockets.end()) {
			assert(response.isStreamAck());

			auto task_socket = zmq::socket_t{ctx, zmq::socket_type::push};
			auto taskEndpoint = "inproc://stream-" + std::to_string(streamId);
			fprintf(stderr, "creating new task socket %s\n", taskEndpoint.c_str());
			task_socket.bind(taskEndpoint);

			// Relay result to new task socket
			task_socket.send(std::move(result), zmq::send_flags::none);

			// Store in stream task socket map
			taskSockets.emplace_hint(idSocketPair, streamId, std::move(task_socket));

		// Relay result to stream worker
		} else {
			idSocketPair->second.send(std::move(result), zmq::send_flags::none);
		}
	}

	// Return control of downstream data socket to main thread
	return std::move(data_socket);
}

// Proxy result messages produced by local stream tasks to upstream data socket
// Returns control of upstream data socket upon completion
static zmq::socket_t proxy_results_upstream(zmq::context_t& ctx, std::string resultEndpoint, zmq::socket_t&& upstream_data_socket)
{
	// Create result
	auto result_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	result_socket.bind(resultEndpoint);

	while (true) {
		// Read next incoming local message
		auto message = recv_message(result_socket);

		// If exit message indicating all streams completed, return socket back to frontend for cleanup
		if (make_MessageReader(message).getRoot<NetworkResponse>().isExitAck()) {
			return std::move(upstream_data_socket);
		}

		// Otherwise, relay application result response upstream
		upstream_data_socket.send(std::move(message), zmq::send_flags::none);
	}
}

// Process result messages; aggregate and send to local result socket
static int stream_aggregator(zmq::context_t& ctx, std::string resultEndpoint,
	StreamId streamId, int numChildren, std::string filterPath)
{
	int rc = -1;

	// Open task socket to receive this stream's input from downstream data task
	auto task_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	{ auto const taskEndpoint = "inproc://stream-" + std::to_string(streamId);
		fprintf(stderr, "%d: connecting to task endpoint %s\n", streamId, taskEndpoint.c_str());
		task_socket.connect(taskEndpoint);
	}

	// Wait until all downstream children have created this stream
	for (int i = 0; i < numChildren; i++) {
		fprintf(stderr, "%d: ack %d / %d\n", streamId, i, numChildren);
		auto [reader, resultReader, response] = recv_MessageRoot<NetworkResponse>(task_socket);
		assert(response.isStreamAck() && (response.getStreamAck().getStreamId() == streamId));
	}

	// Open the filter implementation dynamic library
	fprintf(stderr, "%d: opening filter at %s\n", streamId, filterPath.c_str());
	using FilterRunType = std::string(std::vector<std::string> const&);
	auto filterHandle = cti::Dlopen::Handle{filterPath};
	auto filterRun = filterHandle.load<FilterRunType>("run");

	// Open result socket to send this stream's output to upstream proxy task
	auto result_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	{ fprintf(stderr, "%d: connecting to task result %s\n", streamId, resultEndpoint.c_str());
		result_socket.connect(resultEndpoint);
	}

	// Send stream open acknowledgement to parent
	{ auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		response.initStreamAck().setStreamId(streamId);

		send_MessageBuilder(result_socket, builder);
	}

	// Filter synchronization, aggregate and send results when all children have reported
	auto in_progress_results = std::vector<std::string>{};
	in_progress_results.reserve(numChildren);

	while (true) {

		// Receive result from task socket
		auto [reader, resultReader, response] = recv_MessageRoot<NetworkResponse>(task_socket);

		// If exit message, remove one child
		if (response.isExitAck()) {
			numChildren--;
			fprintf(stderr, "%d: child exited, %d remaining\n", streamId, numChildren);

		} else {
			// Check stream ID and user result
			assert(response.isUserResult() && (response.getUserResult().getStreamId() == streamId));

			// Add content to in-progress result list
			fprintf(stderr, "content: '%s'\n", response.getUserResult().getContent().cStr());
			in_progress_results.emplace_back(response.getUserResult().getContent());
		}

		// Aggregate when all children have reported a result, or all children exit
		if (!in_progress_results.empty()
		 && ((in_progress_results.size() == (size_t)numChildren) || (numChildren == 0))) {

			// Run user filter and clear message list
			fprintf(stderr, "running user filter\n");
			auto result = filterRun(std::move(in_progress_results));
			in_progress_results.clear();
			in_progress_results.reserve(numChildren);

			// Create outgoing result message
			auto builder = capnp::MallocMessageBuilder{};
			auto response = builder.initRoot<NetworkResponse>();
			auto userResult = response.initUserResult();
			userResult.setStreamId(streamId);
			userResult.setContent(result);

			// Send result
			fprintf(stderr, "sending result: '%s'\n", result.c_str());
			send_MessageBuilder(result_socket, builder);
		}

		// If all children exited, send exit message to result socket and exit
		if (numChildren == 0) {
			auto builder = capnp::MallocMessageBuilder{};
			auto response = builder.initRoot<NetworkResponse>();
			response.setExitAck();

			send_MessageBuilder(result_socket, builder);
			break;
		}
	}

	rc = 0;

	return rc;
}

} // namespace workers

int main(int argc, char* const argv[])
{
	int rc = -1;
	auto ctx = zmq::context_t{};

	// Read arguments from frontend
	auto const [dealerEndpoint, leaf_argv] = parse_argv(argc, argv);

	// Topology and child counting
	auto numChildren = int{};
	auto localChildPids = std::vector<pid_t>{};

	// Start network connection process

	// Create upstream and downstream sockets
	auto upstream = UpstreamLink{ctx};
	auto downstream = TcpDownstreamLink{ctx};

	// Format externally-accessible endpoints
	auto const [controlEndpoint, dataEndpoint] = formatEndpoints(downstream, cti_be_getNodeHostname());
	fprintf(stderr, "listening on %s / %s\n", controlEndpoint.c_str(), dataEndpoint.c_str());

	// Send endpoints to topology server and receive parent's endpoints and number of network children
	{ auto request_socket = zmq::socket_t{ctx, zmq::socket_type::req};
		request_socket.connect(dealerEndpoint);
		fprintf(stderr, "connected to topology server %s\n", dealerEndpoint.c_str());

		// Create topology request message
		auto builder = capnp::MallocMessageBuilder{};
		auto topologyRequest = builder.initRoot<TopologyRequest>();
		topologyRequest.setNodeControlEndpoint(controlEndpoint);
		topologyRequest.setNodeDataEndpoint(dataEndpoint);

		// Send this node's endpoints to topology server
		send_MessageBuilder(request_socket, builder);
		fprintf(stderr, "registered endpoints with topology server\n");

		// Receive this node's topology info from topology server
		auto [response, responseReader, topologyResponse] = recv_MessageRoot<TopologyResponse>(request_socket, zmq::recv_flags::none);
		fprintf(stderr, "topology server reported parent %s / %s, %d children\n",
			topologyResponse.getParentControlEndpoint().cStr(),
			topologyResponse.getParentDataEndpoint().cStr(),
			topologyResponse.getNumChildren());

		// Apply topology information
		numChildren += topologyResponse.getNumChildren();

		// Connect to parent's endpoints
		auto const& parentControlEndpoint = topologyResponse.getParentControlEndpoint();
		auto const& parentDataEndpoint = topologyResponse.getParentDataEndpoint();
		fprintf(stderr, "connecting to parent %s / %s\n",
			parentControlEndpoint.cStr(), parentDataEndpoint.cStr());
		upstream.connect(parentControlEndpoint, parentDataEndpoint);
	}

	// Start task distribution

	// Determine number of local children, one per PE on node
	auto const firstPE = cti_be_getNodeFirstPE();
	auto const localPEs = cti_be_getNodePEs();
	numChildren += localPEs;

	// Launch local children with provided leaf binary
	{ auto const leafPath = std::string{cti_be_getBinDir()} + "/" + leaf_argv[0];
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
		for (int i = 0; i < numChildren; i++) {
			fprintf(stderr, "waiting %d / %d\n", i, numChildren);
			auto [reader, responseReader, readyResponse] = recv_MessageRoot<NetworkResponse>(downstream.data_socket);
			assert(readyResponse.isReadyAck());
		}
	}

	// Send ready to parent
	fprintf(stderr, "sending ready to parent\n");
	{ auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		response.setReadyAck();

		send_MessageBuilder(upstream.data_socket, builder);
	}

	// Receive incoming stream data messages and distribute to respective stream task,
	// return control of downstream data socket
	auto distributeStreamInputsTask = std::async(std::launch::async,
		[](zmq::context_t& ctx, zmq::socket_t&& downstream_data_socket, int numChildren) {
			return workers::distribute_stream_inputs(ctx, std::move(downstream_data_socket), numChildren);
		}, std::ref(ctx), std::move(downstream.data_socket), numChildren);

	// Proxy local worker result socket to upstream socket, return control of upstream data socket
	auto const resultEndpoint = "inproc://result";
	auto proxyResultsUpstreamTask = std::async(std::launch::async,
		[](zmq::context_t& ctx, std::string resultEndpoint, zmq::socket_t&& upstream_data_socket) {
			return workers::proxy_results_upstream(ctx, std::move(resultEndpoint), std::move(upstream_data_socket));
		}, std::ref(ctx), resultEndpoint, std::move(upstream.data_socket));

	// Command loop
	auto streamWorkerTasks = std::vector<std::future<int>>{};
	auto const filterBasePath = std::string{cti_be_getLibDir()};
	while (true) {

		// Receive next command from parent
		fprintf(stderr, "waiting for command from upstream\n");
		auto message = recv_message(upstream.control_socket);
		auto messageReader = make_MessageReader(message);
		auto command = messageReader.getRoot<NetworkCommand>();

		// Relay command to children
		downstream.control_socket.send(message, zmq::send_flags::none);

		// Determine if exit command was received
		if (command.isExit()) {
			fprintf(stderr, "got exit command\n");
			break;

		// Determine if new stream is to be registered
		} else if (command.isNewStream()) {
			fprintf(stderr, "got new stream command\n");
			auto const newStreamCommand = command.getNewStream();
			auto const streamId = newStreamCommand.getNewStreamId();
			auto const filterName = newStreamCommand.getFilterName();

			// Create new stream task worker
			streamWorkerTasks.emplace_back(std::async(std::launch::async,
				[](zmq::context_t& ctx, std::string resultEndpoint, StreamId streamId, int numChildren, std::string filterPath) {
					return workers::stream_aggregator(ctx, std::move(resultEndpoint), streamId, numChildren, std::move(filterPath));
				}, std::ref(ctx), resultEndpoint, streamId, numChildren, filterBasePath + "/" + std::string{filterName}));

		} else {
			fprintf(stderr, "got user command\n");
		}
	}

	rc = 0;

cleanup_main:

	// Wait on all local child PIDs
	fprintf(stderr, "waiting for child pids\n");
	for (auto&& pid : localChildPids) {
		int status;
		if (waitpid(pid, &status, 0) || (WIFEXITED(status) && WEXITSTATUS(status))) {
			fprintf(stderr, "pid %d failed\n", pid);
			rc = -1;
		}
	}

	// Wait on stream worker threads
	fprintf(stderr, "waiting for stream worker threads\n");
	for (auto&& streamWorkerTask : streamWorkerTasks) {
		auto const stream_rc = streamWorkerTask.get();
		if (stream_rc) {
			fprintf(stderr, "stream worker failed with code %d\n", stream_rc);
			rc = -1;
		}
	}

	// Wait on stream relay thread
	fprintf(stderr, "waiting for result relay thread\n");
	auto downstream_data_socket = distributeStreamInputsTask.get();

	// Wait for proxy task to complete and regain ownership of upstream data socket
	fprintf(stderr, "waiting for proxy task\n");
	auto upstream_data_socket = proxyResultsUpstreamTask.get();

	// Notify parent that cleanup has completed
	{ auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		response.setExitAck();

		send_MessageBuilder(upstream_data_socket, builder);
	}

	// Close all network sockets
	upstream.control_socket.close();
	upstream_data_socket.close();
	downstream.control_socket.close();
	downstream_data_socket.close();
	fprintf(stderr, "context reset\n");
	ctx.close();

	fprintf(stderr, "commnode cleaned up and exiting\n");

	return rc;
}
