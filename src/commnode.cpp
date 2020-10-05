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

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include "libzmqnet/NetworkSchema.capnp.h"

static inline void send_MessageBuilder(zmq::socket_t& socket, capnp::MessageBuilder& builder, zmq::send_flags const sockopt = zmq::send_flags::none)
{
	auto messageArray = capnp::messageToFlatArray(builder);
	auto const messageBytes = messageArray.asBytes();
	socket.send(zmq::message_t{messageBytes.begin(), messageBytes.size()}, sockopt);
}

static inline auto make_MessageReader(zmq::message_t& message)
{
	assert(message.size() % sizeof(capnp::word) == 0);
	auto const messageWords = message.data<capnp::word>();
	auto const messageWordsLen = message.size() / sizeof(capnp::word);

	return capnp::FlatArrayMessageReader(kj::arrayPtr(messageWords, messageWordsLen));
}

static inline auto recv_MessageReader(zmq::socket_t& socket, zmq::recv_flags const sockopt = zmq::recv_flags::none)
{
	auto message = recv_message(socket, sockopt);
	auto messageReader = make_MessageReader(message);

	return std::make_tuple(std::move(message), std::move(messageReader));
}

template <typename RootType>
static inline auto recv_MessageRoot(zmq::socket_t& socket, zmq::recv_flags const sockopt = zmq::recv_flags::none)
{
	auto [result, resultReader] = recv_MessageReader(socket, sockopt);
	auto rootReader = resultReader.getRoot<RootType>();
	return std::make_tuple(std::move(result), std::move(resultReader), rootReader);
}

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

auto taskSockets = std::unordered_map<StreamId, zmq::socket_t>{};

static int stream_worker(zmq::context_t& ctx,
	std::string taskEndpoint, std::string resultEndpoint,
	StreamId streamId, int numChildren, std::string filterPath)
{
	int rc = -1;

	fprintf(stderr, "%d: connecting to task endpoint %s\n", streamId, taskEndpoint.c_str());
	auto task_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	task_socket.connect(taskEndpoint);

	fprintf(stderr, "%d: connecting to task result %s\n", streamId, resultEndpoint.c_str());
	auto result_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	result_socket.connect(resultEndpoint);

	fprintf(stderr, "%d: opening filter at %s\n", streamId, filterPath.c_str());
	using FilterRunType = std::string(std::vector<std::string> const&);
	auto filterHandle = cti::Dlopen::Handle{filterPath};
	auto filterRun = filterHandle.load<FilterRunType>("run");

	// Wait until all children have acknowledged filter
	for (int i = 0; i < numChildren; i++) {
		fprintf(stderr, "%d: ack %d / %d\n", streamId, i, numChildren);
		auto [reader, resultReader, response] = recv_MessageRoot<NetworkResponse>(task_socket);
		assert(response.isAck());
	}

	// Send acknowledgement to parent
	{ auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		response.setStreamId(streamId);
		response.setAck();

		send_MessageBuilder(result_socket, builder);
	}

	// Filter synchronization
	auto messages = std::vector<std::string>{};
	messages.reserve(numChildren);

	while (true) {

		// Receive result from task queue
		auto [reader, resultReader, response] = recv_MessageRoot<NetworkResponse>(task_socket);

		// Check stream ID and user result
		assert(response.getStreamId() == streamId);
		assert(response.isUserData());

		// Add content to messages list
		fprintf(stderr, "content: '%s'\n", response.getUserData().cStr());
		messages.emplace_back(response.getUserData());

		// Aggregate when synchronization predicate satisfied
		if (messages.size() == numChildren) {

			// Run user filter and clear message list
			fprintf(stderr, "running user filter\n");
			auto result = filterRun(std::move(messages));
			messages.clear();
			messages.reserve(numChildren);

			// Create outgoing result
			auto builder = capnp::MallocMessageBuilder{};
			auto response = builder.initRoot<NetworkResponse>();
			response.setStreamId(streamId);
			response.setUserData(result);

			// Send result
			fprintf(stderr, "sending result: '%s'\n", result.c_str());
			send_MessageBuilder(result_socket, builder);
		}
	}

	rc = 0;

cleanup_stream_worker:
	return rc;
}

static int result_worker(zmq::socket_t&& data_socket)
{
	int rc = -1;

	while (true) {
		// Receive next result from children
		fprintf(stderr, "waiting for result from downstream\n");
		try {
			auto result = recv_message(data_socket);
			auto resultReader = make_MessageReader(result);
			auto response = resultReader.getRoot<NetworkResponse>();

			// Extract stream ID and user command
			auto const streamId = response.getStreamId();

			// Relay result to stream worker
			taskSockets.at(streamId).send(std::move(result), zmq::send_flags::none);
		} catch (zmq::error_t const& err) {
			break;
		}
	}

cleanup_result_worker:
	return rc;
}

static auto exchangeTopologyData(zmq::context_t& ctx, std::string const& dealerEndpoint,
	std::string const& controlEndpoint, std::string const& dataEndpoint)
{
	// Connect to topology server
	auto request_socket = zmq::socket_t{ctx, zmq::socket_type::req};
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

	return std::make_tuple(std::move(response), std::move(responseReader), std::move(topologyResponse));
}

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
	{ auto const [response, responseReader, topologyResponse] =
		exchangeTopologyData(ctx, dealerEndpoint, controlEndpoint, dataEndpoint);
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
	for (int i = 0; i < numChildren; i++) {
		fprintf(stderr, "waiting %d / %d\n", i, numChildren);
		auto [reader, responseReader, readyResponse] = recv_MessageRoot<NetworkResponse>(downstream.data_socket);
		assert(readyResponse.isAck());
	}

	// Send ready to parent
	fprintf(stderr, "sending ready to parent\n");
	{ auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		response.setAck();

		send_MessageBuilder(upstream.data_socket, builder);
	}

	// Create stream task work sockets
	auto streamSockets = std::unordered_map<StreamId, zmq::socket_t>{};
	auto result_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	result_socket.bind("inproc://result");
	auto const resultEndpoint = result_socket.get(zmq::sockopt::last_endpoint);
	auto streamWorkers = std::vector<std::future<int>>{};
	auto const filterBasePath = std::string{cti_be_getLibDir()};

	// Proxy local worker result socket to upstream socket
	auto proxyTask = std::async(std::launch::async, [](zmq::socket_t&& result_socket, zmq::socket_t&& data_socket) {
		//zmq::proxy(result_socket, data_socket);
		while (true) {
			auto message = recv_message(result_socket);
			fprintf(stderr, "proxy got message\n");
			data_socket.send(std::move(message), zmq::send_flags::none);
		}
		return 0;
	}, std::move(result_socket), std::move(upstream.data_socket));

	// Result relay thread
	auto resultRelayTask = std::async(std::launch::async,
		result_worker, std::move(downstream.data_socket));

	// Command relay loop

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

			// Cancel all stream workers

			break;

		// Determine if new stream is to be registered
		} else if (command.isNewStream()) {
			fprintf(stderr, "got new stream command\n");
			auto const newStreamCommand = command.getNewStream();
			auto const streamId = newStreamCommand.getNewStreamId();
			auto const filterName = newStreamCommand.getFilterName();

			// Create new task distribution socket
			auto task_socket = zmq::socket_t{ctx, zmq::socket_type::push};
			auto taskEndpoint = "inproc://stream-" + std::to_string(streamId);
			task_socket.bind(taskEndpoint);
			taskSockets.emplace(streamId, std::move(task_socket));

			// Create new stream task worker
			streamWorkers.emplace_back(std::async(std::launch::async, [&](std::string taskEndpoint, std::string filterName) {
				return stream_worker(ctx,
					std::move(taskEndpoint),
					resultEndpoint,
					streamId, numChildren, filterBasePath + "/" + std::move(filterName));
			}, std::move(taskEndpoint), std::string{filterName}));

		} else {
			fprintf(stderr, "got user command\n");
		}
	}

	rc = 0;

cleanup_main:
	// Wait on stream relay thread
	(void)resultRelayTask.get();

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
