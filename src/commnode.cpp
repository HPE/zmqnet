/*
 * Copyright 2020 Hewlett Packard Enterprise Development LP.
 */

#include <unistd.h>
#include <wait.h>
#include <stdarg.h>

#include <cassert>

#include <string>
#include <vector>
#include <future>
#include <thread>
#include <sstream>

#include "zmqnet.hpp"

#include "cti_dlopen.hpp"
#include "cti_split.hpp"

#include "common_tools_be.h"

#include "argv_defs.hpp"

#include "libzmqnet/lrle_bitset_impl.hpp"
#include "libzmqnet/libzmqnet.hpp"

// Parse user-provided arguments
static auto parse_argv(int argc, char* const argv[])
{
	auto dealerEndpoint = std::string{};
	auto nodeConnectionMap = std::string{};
	auto packed_mode = false;
	auto packed_rank = int{-1};

	using CA = CommnodeArgv;
	auto parsedArgv = cti::IncomingArgv<CA>{argc, argv};

	// Read FE-passed options
	while (true) {
		auto [c, value] = parsedArgv.get_next();
		if (c <= 0) { break; }

		switch (c) {

		case CA::Packed.val:
			packed_mode = true;
			break;

		case CA::DealerEndpoint.val:
			dealerEndpoint = std::move(value);
			break;

		case CA::NodeConnectionMap.val:
			nodeConnectionMap = std::move(value);
			break;

		case CA::PackedRank.val:
			packed_rank = std::stoi(value);
			break;

		case '?':
		default:
			throw std::runtime_error("bad arg");
		}
	}

	// Rest of arguments are leaf arguments
	auto const [leaf_argc, leaf_argv] = parsedArgv.get_rest();
	if (leaf_argv[0] == nullptr) {
		throw std::runtime_error("leaf binary not provided");
	}
	return std::make_tuple(std::move(dealerEndpoint), std::move(nodeConnectionMap), packed_mode, packed_rank, leaf_argc, leaf_argv);
}

// Parse node topology and parent connection data from argument string
static auto parseNodeConnectionMap(std::string&& nodeConnectionMap)
{
	auto parentControlEndpoint = std::string{};
	auto parentDataEndpoint = std::string{};
	auto commnode_children = int{};
	auto commnode_subchildren = int{};

	auto parent_node_index = int{-1};

	// Separate topology from connection data
	auto [topologyData, connectionData] = cti::split::string<2>(std::move(nodeConnectionMap), ';');

	// Parse out each piece of node topology information connection information for parent nodes
	{ auto nodeTopology = std::string{};
		auto nodeTopologyStream = std::istringstream{std::move(topologyData)};

		auto const commnodeHostname = std::string{cti_be_getNodeHostname()};

		// Read next node topology data
		while (std::getline(nodeTopologyStream, nodeTopology, ',')) {

			// Split out topology data to compare hostnames
			auto [hostname, parentNodeIndexString, numChildrenString, numSubChildrenString]
				= cti::split::string<4>(std::move(nodeTopology), ' ');
			if (hostname == commnodeHostname) {
				parent_node_index = std::stoi(parentNodeIndexString);
				commnode_children = std::stoi(numChildrenString);
				commnode_subchildren = std::stoi(numSubChildrenString);
				break;
			}
		}
	}

	// If this commnode was not supposed to launch at this level, exit
	if (parent_node_index < 0) {
		goto cleanup_parseNodeConnectionMap;
	}

	// Find connection information for parent node from index
	{ auto nodeConnection = std::string{};
		auto nodeConnectionStream = std::istringstream{std::move(connectionData)};

		// Get parent node data at index
		for (int i = 0; i <= parent_node_index; i++) {
			if (!std::getline(nodeConnectionStream, nodeConnection, ',')) {
				throw std::runtime_error("malformed connection map: invalid parent node index");
			}
		}

		// Split out parent connection data
		auto [controlEndpoint, dataEndpoint]
			= cti::split::string<2>(std::move(nodeConnection), ' ');
		parentControlEndpoint = std::move(controlEndpoint);
		parentDataEndpoint = std::move(dataEndpoint);
	}

cleanup_parseNodeConnectionMap:
	return std::make_tuple(std::move(parentControlEndpoint), std::move(parentDataEndpoint), commnode_children, commnode_subchildren);
}

namespace workers
{

// Process result messages; aggregate and send to local result socket
static int response_callback(zmq::context_t& ctx, std::string resultEndpoint,
	lrle_bitset const& descendants_bitset, int num_children,
	zmqnet::Stream::Id streamId, std::string filterPath, std::string filterFunctionName)
{
	int rc = -1;

	// Open task socket to receive this stream's input from downstream data task
	auto task_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	{ auto const taskEndpoint = async::task_socket_endpoint(streamId);
		log("commnode %d: connecting to task endpoint %s\n", streamId, taskEndpoint.c_str());
		task_socket.connect(taskEndpoint);
	}

	// Wait until all downstream children have created this stream
	for (int i = 0; i < num_children; i++) {
		log("commnode %d: waiting for stream create ack %d / %d\n", streamId, i + 1, num_children);
		auto reader = net::recv_MessageReader(task_socket);
		auto response = reader.getRoot<NetworkResponse>();
		assert(response.isStream() && (response.getStream().getStreamId() == streamId));
	}

	// Open the filter implementation dynamic library
	log("commnode %d: opening filter at %s\n", streamId, filterPath.c_str());
	using FilterRunType = zmqnet::ByteArray(std::vector<zmqnet::ByteArray> const&);
	auto filterHandle = cti::Dlopen::Handle{filterPath};
	auto filterRun = filterHandle.load<FilterRunType>(filterFunctionName);

	// Open result socket to send this stream's output to upstream proxy task
	auto result_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	{ log("commnode %d: connecting to task result %s\n", streamId, resultEndpoint.c_str());
		result_socket.connect(resultEndpoint);
	}

	// Send stream open acknowledgement to parent
	{ auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		response.initStream().setStreamId(streamId);

		log("commnode %d: sending stream open acknowledgement\n");
		net::send_MessageBuilder(result_socket, net::communicator_ids::AllRanks, builder);
	}

	// Filter synchronization, aggregate and send results when all children have reported
	auto in_progress_results = std::vector<zmqnet::ByteArray>{};

	while (true) {

		// Receive result from task socket
		log("commnode %d: waiting for result from task socket\n", streamId);
		auto reader = net::recv_MessageReader(task_socket);
		auto response = reader.getRoot<NetworkResponse>();

		// If exit message, remove one child
		if (response.isExit()) {
			num_children--;
			log("commnode %d: child exited, %d remaining\n", streamId, num_children);

		} else {
			// Check stream ID and user result
			assert(response.isUser() && (response.getUser().getStreamId() == streamId));

			// Add content to in-progress result list
			log("content length: '%lu'\n", response.getUser().getContent().size());
			in_progress_results.emplace_back(kj::heapArray(response.getUser().getContent().asBytes()));
		}

		// Aggregate when all children have reported a result, or all children exit
		if (!in_progress_results.empty()
		 && ((in_progress_results.size() == (size_t)num_children) || (num_children == 0))) {

			// Run user filter and clear message list
			log("running user filter\n");
			auto result = filterRun(std::move(in_progress_results));
			in_progress_results.clear();
			in_progress_results.reserve(num_children);

			// Create outgoing result message
			auto builder = capnp::MallocMessageBuilder{};
			auto response = builder.initRoot<NetworkResponse>();
			auto userResult = response.initUser();
			userResult.setStreamId(streamId);
			auto const result_size = result.size();
			userResult.setContent(std::move(result));

			// Send result
			log("sending result size %lu\n", result_size);
			net::send_MessageBuilder(result_socket, net::communicator_ids::AllRanks, builder);
		}

		// If all children exited, end loop
		if (num_children == 0) {
			break;
		}
	}

	rc = 0;

	return rc;
}

} // namespace workers

namespace launch
{

static auto fcfs(zmq::context_t& ctx, net::UpstreamLink& upstream, net::TcpDownstreamLink& downstream,
	std::string const& dealerEndpoint)
{
	// Format externally-accessible endpoints
	auto const hostname = cti_be_getNodeHostname();
	auto const [controlEndpoint, dataEndpoint] = formatEndpoints(downstream, hostname);
	log("listening on %s / %s\n", controlEndpoint.c_str(), dataEndpoint.c_str());

	// Send endpoints to topology server and receive parent's endpoints and number of network children
	auto request_socket = zmq::socket_t{ctx, zmq::socket_type::req};
	request_socket.connect(dealerEndpoint);
	log("connected to topology server %s\n", dealerEndpoint.c_str());

	// Create topology request message
	{ auto builder = capnp::MallocMessageBuilder{};
		auto topologyRequest = builder.initRoot<TopologyRequest>();
		topologyRequest.setNodeControlEndpoint(controlEndpoint);
		topologyRequest.setNodeDataEndpoint(dataEndpoint);

		// Send this node's endpoints to topology server
		net::send_MessageBuilder(request_socket, net::communicator_ids::AllRanks, builder);
		log("registered endpoints with topology server\n");
	}

	// Receive this node's topology info from topology server
	auto reader = net::recv_MessageReader(request_socket);
	auto topologyResponse = reader.getRoot<TopologyResponse>();
	log("topology server reported parent %s / %s, %d children\n",
		topologyResponse.getParentControlEndpoint().cStr(),
		topologyResponse.getParentDataEndpoint().cStr(),
		topologyResponse.getNumChildren());

	// Apply topology information
	int num_children = topologyResponse.getNumChildren();

	// Connect to parent's endpoints
	auto const& parentControlEndpoint = topologyResponse.getParentControlEndpoint();
	auto const& parentDataEndpoint = topologyResponse.getParentDataEndpoint();
	log("connecting to parent %s / %s\n",
		parentControlEndpoint.cStr(), parentDataEndpoint.cStr());
	upstream.connect(parentControlEndpoint, parentDataEndpoint);

	return num_children;
}

static void layered(net::UpstreamLink& upstream, net::TcpDownstreamLink& downstream,
	std::string const& parentControlEndpoint, std::string const& parentDataEndpoint,
	int num_subchildren)
{
	// Format externally-accessible endpoints
	auto const hostname = cti_be_getNodeHostname();
	auto const [controlEndpoint, dataEndpoint] = formatEndpoints(downstream, hostname);
	log("listening on %s / %s\n", controlEndpoint.c_str(), dataEndpoint.c_str());

	// Connect to parent's endpoints
	log("connecting to parent %s / %s\n",
		parentControlEndpoint.c_str(), parentDataEndpoint.c_str());
	upstream.connect(parentControlEndpoint, parentDataEndpoint);

	// Send endpoints to frontend
	{ auto builder = capnp::MallocMessageBuilder{};
		auto topologyRequest = builder.initRoot<TopologyRequest>();
		topologyRequest.setNodeControlEndpoint(controlEndpoint);
		topologyRequest.setNodeDataEndpoint(dataEndpoint);
		topologyRequest.setNodeHostname(hostname);

		// Send this node's endpoints to topology server
		net::send_MessageBuilder(upstream.data_socket, net::communicator_ids::AllRanks, builder);
	}
	log("registered with frontend. relaying child messages\n");

	// Receive and relay all future levels of connection messages
	for (int i = 0; i < num_subchildren; i++) {
		auto message = net::recv_message(downstream.data_socket);
		log("subchild %d/%d connection\n", i + 1, num_subchildren);
		upstream.data_socket.send(std::move(message), zmq::send_flags::none);
	}

	// Wait for leaf start command from frontend and relay
	log("children launched, waiting for rest of network to launch\n");
	{ auto reader = net::recv_MessageReader(upstream.control_socket);
		auto command = reader.getRoot<NetworkCommand>();
		assert(command.isStartLeaves());
		downstream.control_socket.send(reader.eject(), zmq::send_flags::none);
	}
}

} // namespace launch

int main(int argc, char* const argv[])
{
	auto const start_ms = get_milliseconds();
	int rc = -1;
	auto ctx = zmq::context_t{};

	// Read arguments from frontend
	auto [dealerEndpoint, nodeConnectionMap, packed_mode, packed_rank, leaf_argc, leaf_argv] = parse_argv(argc, argv);

	// Start network connection process
	auto upstream = net::UpstreamLink{ctx};
	net::subscribe_to_tag(upstream, net::communicator_ids::AllRanks);
	auto downstream = net::TcpDownstreamLink{ctx};
	auto num_children = int{};

	// Use either FCFS or leveled tree launch based on arguments
	if (!dealerEndpoint.empty()) {

		// Run FCFS launch procedure
		num_children = launch::fcfs(ctx, upstream, downstream, dealerEndpoint);

	} else {
		// Connect using leveled tree launch
		auto [parentControlEndpoint, parentDataEndpoint, num_children_, num_subchildren] = parseNodeConnectionMap(std::move(nodeConnectionMap));
		num_children = num_children_;

		// Determine if launch should continue
		if (parentControlEndpoint.empty()) {
			return 0;
		}

		// Run layered launch procedure
		launch::layered(upstream, downstream, parentControlEndpoint, parentDataEndpoint, num_subchildren);
	}
	log("%d network children\n", num_children);

	// Start leaf process launch
	auto localChildPids = std::vector<pid_t>{};

	// Determine number of local children, one per PE on node
	auto const first_pe = (packed_rank < 0)
		? cti_be_getNodeFirstPE()
		: packed_rank;
	auto const local_pes = 1;
	num_children += local_pes;

	// If running packed mode and no rank was provided, start other packed commnodes
	if (packed_mode && (packed_rank < 0)) {
		auto const packed_commnodes = cti_be_getNodePEs() - 1;
		log("forking %d local packed commnodes\n", packed_commnodes);

		// Set up commnode daemon arguments with topology connection info and leaf process arguments
		auto commnodeArgv = cti::OutgoingArgv<CommnodeArgv>{argv[0]};
		commnodeArgv.add(CommnodeArgv::DealerEndpoint, dealerEndpoint);
		commnodeArgv.add(CommnodeArgv::Packed);
		commnodeArgv.add(cti::Argv::Argument{"-r"});
		commnodeArgv.add(cti::Argv::Argument{"-1"}); // Add seperately from option so it can be replaced via pointer

		// Add provided leaf arguments
		for (int i = 0; i < leaf_argc; i++) {
			commnodeArgv.add(cti::Argv::Argument{leaf_argv[i]});
		}

		// Get pointer to rank argument
		auto commnode_argv = commnodeArgv.get();
		auto rank_arg_ptr = (char**)&commnode_argv[4];

		// Launch packed commnodes
		for (int i = 0; i < packed_commnodes; i++) {
			log("%d / %d\n", i + 1, packed_commnodes);

			if (auto const forkedPid = fork()) {
				// Add launched process to cleanup list
				localChildPids.emplace_back(forkedPid);

			} else {
				// Set rank argument
				auto const rank = first_pe + local_pes + i;
				auto const rankString = std::to_string(rank);
				*rank_arg_ptr = (char*)rankString.c_str();

				// Exec leaf binary
				execvp(argv[0], commnode_argv);
				perror("execvp");
				_exit(-1);
			}
		}
	}

	// Launch local children with provided leaf binary
	{ auto const leafPath = std::string{cti_be_getBinDir()} + "/" + leaf_argv[0];
		auto const [controlEndpoint, dataEndpoint] = net::formatEndpoints(downstream, cti_be_getNodeHostname());
		log("forking %d local leaf processes %s\n", local_pes, leafPath.c_str());

		// Prepend endpoints and rank number to provided leaf arguments
		auto completeLeafArgv = cti::ManagedArgv
			{ leaf_argv[0]
			, controlEndpoint
			, dataEndpoint
			, ""
		};

		// Copy provided arguments
		for (int i = 1; i < leaf_argc; i++) {
			completeLeafArgv.add(leaf_argv[i]);
		}

		// Get pointer to rank argument
		auto complete_leaf_argv = completeLeafArgv.get();
		auto rank_arg_ptr = &complete_leaf_argv[3];

		for (int rank = first_pe; rank < first_pe + local_pes; rank++) {
			log("%d / %d\n", rank - first_pe + 1, local_pes);

			if (auto const forkedPid = fork()) {
				// Add launched process to cleanup list
				localChildPids.emplace_back(forkedPid);

			} else {
				// Set rank argument
				auto const rankString = std::to_string(rank);
				*rank_arg_ptr = (char*)rankString.c_str();

				// Exec leaf binary
				execvp(leafPath.c_str(), complete_leaf_argv);
				perror("execvp");
				_exit(-1);
			}
		}
	}

	// Create mask of all descendants' ranks
	auto descendants_bitset = lrle_bitset{};

	// Wait for all children to connect and send ready
	for (int i = 0; i < num_children; i++) {
		log("waiting for child %d / %d\n", i + 1, num_children);
		auto reader = net::recv_MessageReader(downstream.data_socket);
		auto response = reader.getRoot<NetworkResponse>();
		assert(response.isReady());

		// Add ranks to children rank bitset
		auto const response_rank_bitset = response.getReady().getRanks().asBytes();
		descendants_bitset.or_eq_buf(response_rank_bitset.begin(), response_rank_bitset.size());
	}

	// Send ready to parent
	log("sending ready to parent\n");
	fprintf(stderr, "%05lu: sending ready to parent\n", get_milliseconds() - start_ms);
	{ auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		auto ready = response.initReady();

		// Set rank bitset
		ready.setRanks(kj::ArrayPtr<kj::byte const>
			{ (unsigned char const*)descendants_bitset.compressed.data()
			, descendants_bitset.compressed.size()
		});

		net::send_MessageBuilder(upstream.data_socket, net::communicator_ids::AllRanks, builder);
	}

	// Proxy local worker result socket to upstream socket, return control of upstream data socket
	auto const collectorEndpoint = "inproc://result";
	auto collector_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	auto responseCollector = std::async(std::launch::async,
		[](zmq::context_t& ctx, std::string collectorEndpoint, zmq::socket_t&& output_socket) {
			return async::collect_network_responses(ctx, std::move(collectorEndpoint), std::move(output_socket));
		}, std::ref(ctx), collectorEndpoint, std::move(upstream.data_socket));

	// Connect for message injection by main thread
	collector_socket.connect(collectorEndpoint);

	// Receive incoming stream data messages and distribute to respective stream task,
	// return control of downstream data socket
	auto responseDistributor = std::async(std::launch::async,
		[](zmq::context_t& ctx, zmq::socket_t&& input_socket, int num_clients) {
			return async::distribute_messages<NetworkResponse>(ctx, std::move(input_socket), num_clients);
		}, std::ref(ctx), std::move(downstream.data_socket), num_children);

	// Command loop
	auto streamWorkerTasks = std::vector<std::future<int>>{};
	auto const filterBasePath = std::string{cti_be_getLibDir()};
	while (true) {

		// Receive next command from parent
		log("waiting for command from upstream\n");
		auto reader = net::recv_MessageReader(upstream.control_socket);
		auto command = reader.getRoot<NetworkCommand>();

		// Relay command to children
		downstream.control_socket.send(reader.get(), zmq::send_flags::none);

		// Determine if exit command was received
		if (command.isExit()) {
			log("got exit command\n");
			break;

		} else if (command.isStartLeaves()) {
			log("unexpected: startLeaves\n");
			continue;

		// Determine if new communicator is to be registered and subscribed
		} else if (command.isCommunicator()) {

			// Get communicator rank mask
			auto const communicator_ranks_bitset = command.getCommunicator().getRanks().asBytes();

			// If any descendants match subset mask, relay to children and subscribe to subset tag
			if (lrle_any_intersection(
				descendants_bitset.compressed.data(), descendants_bitset.compressed.size(),
				communicator_ranks_bitset.begin(), communicator_ranks_bitset.size())) {

				auto const communicatorId = reader.getTag();

				// Relay to children
				downstream.control_socket.send(reader.eject(), zmq::send_flags::none);

				// Subscribe to subset tag
				net::subscribe_to_tag(upstream, communicatorId);
			}

		// Determine if new stream is to be registered
		} else if (command.isStream()) {
			log("got new stream command\n");
			auto const newStreamCommand = command.getStream();
			auto const streamId = newStreamCommand.getStreamId();
			auto const filterLibraryName = newStreamCommand.getFilterLibraryName();
			auto const filterFunctionName = newStreamCommand.getFilterFunctionName();

			// Create new stream task worker
			streamWorkerTasks.emplace_back(std::async(std::launch::async,
				[](zmq::context_t& ctx, std::string collectorEndpoint,
					lrle_bitset const& descendants_bitset, int num_children,
					zmqnet::Stream::Id streamId, std::string filterPath, std::string filterFunctionName) {

					return workers::response_callback(ctx, std::move(collectorEndpoint),
						descendants_bitset, num_children,
						streamId, std::move(filterPath), std::move(filterFunctionName));

				}, std::ref(ctx), collectorEndpoint,
					std::ref(descendants_bitset), num_children,
					streamId, filterBasePath + "/" + std::string{filterLibraryName}, filterFunctionName));

		} else {
			log("got user command\n");
		}
	}

	rc = 0;

cleanup_main:
	// Wait on stream relay thread
	log("waiting for result relay thread\n");
	downstream.data_socket = responseDistributor.get();

	// Wait on stream worker threads
	log("waiting for stream worker threads\n");
	for (auto&& streamWorkerTask : streamWorkerTasks) {
		auto const stream_rc = streamWorkerTask.get();
		if (stream_rc) {
			log("stream worker failed with code %d\n", stream_rc);
			rc = -1;
		}
	}

	// Construct exit message for proxy
	{ auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		response.setExit();

		// Send exit message to proxy
		net::send_MessageBuilder(collector_socket, net::communicator_ids::AllRanks, builder);
	}

	// Wait for proxy task to complete and regain ownership of upstream data socket
	log("waiting for proxy task\n");
	upstream.data_socket = responseCollector.get();

	// Clean up local child PIDs
	log("waiting for child pids\n");
	for (auto&& pid : localChildPids) {

		// Wait for exit
		int status;
		if (waitpid(pid, &status, 0) < 0) {
			log("waitpid %d failed: %s\n", pid, strerror(errno));
			rc = -1;
		} else if (WIFEXITED(status) && WEXITSTATUS(status)) {
			log("pid %d failed with error code %d\n", pid, WEXITSTATUS(status));
			rc = -1;
		}
	}

	// Notify parent that cleanup has completed
	{ auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		response.setExit();

		net::send_MessageBuilder(upstream.data_socket, net::communicator_ids::AllRanks, builder);
		log("frontend notified of exit\n");
	}

	// Close all network sockets
	upstream.control_socket.close();
	upstream.data_socket.close();
	downstream.control_socket.close();
	downstream.data_socket.close();
	log("context reset\n");
	ctx.close();

	log("commnode cleaned up and exiting\n");
	fprintf(stderr, "%05lu: exiting\n", get_milliseconds() - start_ms);

	return rc;
}
