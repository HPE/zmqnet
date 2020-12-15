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
static int stream_aggregator(zmq::context_t& ctx, std::string resultEndpoint,
	zmqnet::Stream const& stream, std::string filterPath, std::string filterFunctionName,
	lrle_bitset const& descendants_bitset)
{
	int rc = -1;

	// Open task socket to receive this stream's input from downstream data task
	auto task_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	{ auto const taskEndpoint = async::stream_aggregator_endpoint(stream);
		log("commnode %02x %d: connecting to task endpoint %s\n",
			stream.communicatorId, stream.id, taskEndpoint.c_str());
		task_socket.connect(taskEndpoint);
	}

	// Open result socket to send this stream's output to upstream proxy task
	log("commnode %02x %d: connecting to task result %s\n",
		stream.communicatorId, stream.id, resultEndpoint.c_str());
	auto result_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	result_socket.connect(resultEndpoint);

	// Open the filter implementation dynamic library
	log("commnode %02x %d: opening filter at %s\n",
		stream.communicatorId, stream.id, filterPath.c_str());
	using FilterRunType = zmqnet::ByteArray(std::vector<zmqnet::ByteArray> const&);
	auto filterHandle = cti::Dlopen::Handle{filterPath};
	auto filterRun = filterHandle.load<FilterRunType>(filterFunctionName);

	// Wait until all downstream children have created this stream
	for (auto started_descendants = lrle_bitset{}; started_descendants != descendants_bitset; ) {
		log("commnode %02x %d: waiting for descendants stream create (target %s have %s)\n",
			stream.communicatorId, stream.id, descendants_bitset.to_string().c_str(), started_descendants.to_string().c_str());
		auto reader = net::recv_MessageReader(task_socket);
		auto response = reader.getRoot<NetworkResponse>();
		assert(response.isStream() && (response.getStream().getStreamId() == stream.id));

		// Add to started descendants
		{ auto const started_ranks = response.getStream().getRanks().asBytes();
			started_descendants.or_eq_buf(started_ranks.begin(), started_ranks.size());
		}
	}

	// Send stream open acknowledgement to parent
	{ auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		auto streamResponse = response.initStream();
		streamResponse.setStreamId(stream.id);

		// Set rank bitset
		streamResponse.setRanks(kj::ArrayPtr<kj::byte const>
			{ (unsigned char const*)descendants_bitset.compressed.data()
			, descendants_bitset.compressed.size()
		});

		net::send_MessageBuilder(result_socket, stream.communicatorId, builder);
	}

	// Filter synchronization, aggregate and send results when all children have reported
	auto reported_results = std::vector<zmqnet::ByteArray>{};
	auto reported_descendants = lrle_bitset{};
	auto exited_descendants = lrle_bitset{};

	while (true) {
		auto exited_update = false;
		auto results_update = false;

		// Receive result from task socket
		log("commnode %02x %d: waiting for result from task socket\n",
			stream.communicatorId, stream.id);
		auto reader = net::recv_MessageReader(task_socket);
		auto response = reader.getRoot<NetworkResponse>();

		// If exit message, remove one child
		if (response.isExit()) {
			// Add to exited descendants
			{ auto const exited_ranks = response.getExit().getRanks().asBytes();
				exited_descendants.or_eq_buf(exited_ranks.begin(), exited_ranks.size());
			}

			log("commnode %02x %d: descendant(s) exited\n",
				stream.communicatorId, stream.id);
			exited_update = true;
		} else {
			// Check stream ID and user result
			assert(response.isUser() && (response.getUser().getStreamId() == stream.id));

			// Add content to in-progress result list
			reported_results.emplace_back(kj::heapArray(response.getUser().getContent().asBytes()));

			// Add to reported descendants
			{ auto const reported_ranks = response.getUser().getRanks().asBytes();
				reported_descendants.or_eq_buf(reported_ranks.begin(), reported_ranks.size());
			}

			log("commnode %02x %d: user content length: %lu, target %s have %s\n",
				stream.communicatorId, stream.id, response.getUser().getContent().size(),
				descendants_bitset.to_string().c_str(), reported_descendants.to_string().c_str());
			results_update = true;
		}

		// Aggregate when all children have reported a result, or all descendants exit
		auto const all_descendants_exited = (exited_update && (exited_descendants == descendants_bitset));
		if (!reported_results.empty()
		 && (all_descendants_exited
		  || (results_update && (reported_descendants == descendants_bitset)))) {

			// Run user filter and clear message list
			log("commnode %02x %d: running user filter\n",
				stream.communicatorId, stream.id);
			auto result = filterRun(std::move(reported_results));
			reported_results.clear();

			// Create outgoing result message
			auto builder = capnp::MallocMessageBuilder{};
			auto response = builder.initRoot<NetworkResponse>();
			auto userResult = response.initUser();
			userResult.setStreamId(stream.id);
			auto const result_size = result.size();
			userResult.setContent(std::move(result));

			// Set rank bitset
			userResult.setRanks(kj::ArrayPtr<kj::byte const>
				{ (unsigned char const*)reported_descendants.compressed.data()
				, reported_descendants.compressed.size()
			});

			// Send result
			log("commnode %02x %d: sending result size %lu\n",
				stream.communicatorId, stream.id, result_size);
			net::send_MessageBuilder(result_socket, net::communicator_ids::AllRanks, builder);

			// Reset in progress ranks
			reported_descendants = lrle_bitset{};
		}

		// If all children exited, end loop
		if (all_descendants_exited) {
			break;
		}
	}

	rc = 0;

	return rc;
}

static inline int communicator_distributor(zmq::context_t& ctx,
	std::string const& resultEndpoint, std::string const& communicatorBroadcastEndpoint,
	zmqnet::Communicator const& communicator,
	lrle_bitset const& descendants_bitset)
{
	int rc = -1;

	// Open task socket to receive this communicator's input from distributor
	log("commnode %02x: connecting to endpoint %s\n",
		communicator.id, communicatorBroadcastEndpoint.c_str());
	auto input_socket = zmq::socket_t{ctx, zmq::socket_type::sub};
	input_socket.connect(communicatorBroadcastEndpoint);
	net::subscribe_to_tag(input_socket, net::communicator_ids::AllRanks);
	net::subscribe_to_tag(input_socket, communicator.id);

	// Open result socket to send this communicator's output to upstream proxy task
	log("commnode %02x: connecting to task result %s\n",
		communicator.id, resultEndpoint.c_str());
	auto result_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	result_socket.connect(resultEndpoint);

	// Wait until all downstream children have created this communicator
	auto started_descendants = lrle_bitset{};
	while (started_descendants != descendants_bitset) {
		log("commnode %02x: waiting for descendants communicator create (target %s have %s)\n",
			communicator.id, descendants_bitset.to_string().c_str(), started_descendants.to_string().c_str());
		auto reader = net::recv_MessageReader(input_socket);
		auto response = reader.getRoot<NetworkResponse>();
		assert(response.isCommunicator() && (response.getCommunicator().getCommunicatorId() == communicator.id));

		// Add to started descendants
		{ auto const started_ranks = response.getCommunicator().getRanks().asBytes();
			started_descendants.or_eq_buf(started_ranks.begin(), started_ranks.size());
		}
	}

	// Send communicator open acknowledgement to parent
	{ auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		auto communicatorResponse = response.initCommunicator();
		communicatorResponse.setCommunicatorId(communicator.id);

		// Set rank bitset
		communicatorResponse.setRanks(kj::ArrayPtr<kj::byte const>
			{ (unsigned char const*)descendants_bitset.compressed.data()
			, descendants_bitset.compressed.size()
		});

		log("commnode %02x: sending communicator create acknowledgement\n",
			communicator.id);
		net::send_MessageBuilder(result_socket, communicator.id, builder);
	}

	// Store stream task sockets
	auto taskSockets = std::unordered_map<zmqnet::Stream::Id, zmq::socket_t>{};

	// Track exited descendants
	auto exited_descendants = lrle_bitset{};

	while (true) {
		// Receive next message
		log("commnode %02x: receiving message...\n", communicator.id);
		auto reader = net::recv_MessageReader(input_socket);
		auto response = reader.getRoot<NetworkResponse>();

		// If exit, clean up
		if (response.isExit()) {
			log("commnode %02x: got exit response\n", communicator.id);
			assert(reader.getTag() == net::communicator_ids::AllRanks);

			// Add to exited descendants
			{ auto const exited_ranks = response.getExit().getRanks().asBytes();
				exited_descendants.or_eq_buf(exited_ranks.begin(), exited_ranks.size());
			}

			// Relay exit message to task sockets
			auto message = reader.eject();
			for (auto&& [streamId, task_socket] : taskSockets) {
				task_socket.send(message, zmq::send_flags::none);
			}

			// One child has exited, check if all descendants exited
			if (exited_descendants == descendants_bitset) {
				log("commnode %02x: exiting (none remaining)\n", communicator.id);

				// All clients have exited, run cleanup
				break;
			}

			continue;
		}

		// Content is either stream or user content
		assert(response.isStream() || response.isUser());

		// Extract stream ID to select stream worker
		auto const streamId = (response.isStream())
			? response.getStream().getStreamId()
			: response.getUser().getStreamId();

		// Get socket for given stream ID
		auto const idSocketPair = taskSockets.find(streamId);

		// Reconstitute stream object
		auto const stream = zmqnet::Stream { .communicatorId = reader.getTag(), .id = streamId };

		// Create new stream task connection with known endpoint format, if needed
		if (idSocketPair == taskSockets.end()) {
			log("commnode %02x: new stream %d\n", communicator.id, stream.id);
			assert(response.isStream());

			// Create task socket and store in stream task socket map
			taskSockets.emplace_hint(idSocketPair, stream.id,
				async::make_task_socket(ctx, stream, reader.eject()));

		// Relay message to task socket
		} else {
			log("commnode %02x: relay message to stream %d\n", communicator.id, streamId);
			idSocketPair->second.send(reader.eject(), zmq::send_flags::none);
		}
	}

	rc = 0;

	return rc;
}

static inline zmq::socket_t response_distributor(zmq::context_t& ctx, zmq::socket_t&& input_socket,
	std::string const& resultEndpoint, std::string const& communicatorBroadcastEndpoint,
	int const num_children)
{
	// Open result socket to send this communicator's output to upstream proxy task
	log("commnode: connecting to task result %s\n",
		resultEndpoint.c_str());
	auto result_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	result_socket.connect(resultEndpoint);

	// Open communicator broadcast socket
	log("commnode: binding to communicator endpoint %s\n",
		communicatorBroadcastEndpoint.c_str());
	auto communicator_broadcast_socket = zmq::socket_t{ctx, zmq::socket_type::pub};
	communicator_broadcast_socket.bind(communicatorBroadcastEndpoint);

	// Track exited descendants
	auto exited_descendants = lrle_bitset{};

	while (true) {
		// Receive next message
		log("commnode: receiving message...\n");
		auto reader = net::recv_MessageReader(input_socket);
		auto response = reader.getRoot<NetworkResponse>();

		// If exit, clean up
		if (response.isExit()) {
			log("commnode: got exit response\n");
			assert(reader.getTag() == net::communicator_ids::AllRanks);

			// Add to exited descendants
			{ auto const exited_ranks = response.getExit().getRanks().asBytes();
				exited_descendants.or_eq_buf(exited_ranks.begin(), exited_ranks.size());
			}

			// Relay exit message to communicators
			communicator_broadcast_socket.send(reader.eject(), zmq::send_flags::none);

			// One child has exited, check if all descendants exited
			if (lrle_full(exited_descendants.compressed.data(), exited_descendants.compressed.size()) == (size_t)num_children) {
				log("commnode: exiting (none remaining)\n");

				// All clients have exited, run cleanup
				break;
			}

		} else {
			// Relay message to communicators
			log("commnode: relay message to communicators\n");
			sleep(1);
			communicator_broadcast_socket.send(reader.eject(), zmq::send_flags::none);
		}
	}

	// Return control of incoming result socket
	return std::move(input_socket);
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
	net::subscribe_to_tag(upstream.control_socket, net::communicator_ids::AllRanks);
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
	auto const resultEndpoint = "inproc://result";
	auto collector_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	auto responseCollector = std::async(std::launch::async,
		[](zmq::context_t& ctx, std::string resultEndpoint, zmq::socket_t&& output_socket) {
			return async::collect_network_responses(ctx, std::move(resultEndpoint), std::move(output_socket));
		}, std::ref(ctx), resultEndpoint, std::move(upstream.data_socket));

	// Connect for message injection by main thread
	collector_socket.connect(resultEndpoint);

	// Receive incoming stream data messages and distribute to respective stream task,
	// return control of downstream data socket
	auto const communicatorBroadcastEndpoint = "inproc://communicator-broadcast";
	auto responseDistributor = std::async(std::launch::async,
		[](zmq::context_t& ctx, zmq::socket_t&& input_socket, std::string resultEndpoint, std::string communicatorBroadcastEndpoint, lrle_bitset descendants_bitset) {
			return workers::response_distributor(ctx, std::move(input_socket), std::move(resultEndpoint), std::move(communicatorBroadcastEndpoint), std::move(descendants_bitset));
		}, std::ref(ctx), std::move(downstream.data_socket), resultEndpoint, communicatorBroadcastEndpoint, descendants_bitset);

	// Command loop
	auto streamAggregatorTasks = std::vector<std::future<int>>{};
	auto communicatorDistributorTasks = std::vector<std::future<int>>{};
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
			// Reconstitute communicator object
			auto const communicator = zmqnet::Communicator { .id = reader.getTag() };

			// Get communicator rank mask
			auto const communicator_ranks_bitset = command.getCommunicator().getRanks().asBytes();

			// If all ranks, subscribe
			auto communicator_descendants = lrle_bitset{};
			if (communicator.id == net::communicator_ids::AllRanks) {
				communicator_descendants = descendants_bitset;

			// If any descendants match subset mask, relay to children and subscribe to subset tag
			} else if (lrle_any_intersection(
				descendants_bitset.compressed.data(), descendants_bitset.compressed.size(),
				communicator_ranks_bitset.begin(), communicator_ranks_bitset.size())) {

				// Find descendants intersection
				communicator_descendants = descendants_bitset.and_buf(communicator_ranks_bitset.begin(), communicator_ranks_bitset.size());
			}

			if (!communicator_descendants.empty()) {
				// Create new communicator result worker
				communicatorDistributorTasks.emplace_back(std::async(std::launch::async,
					[](zmq::context_t& ctx, std::string resultEndpoint,
						std::string communicatorBroadcastEndpoint, zmqnet::Communicator communicator,
						lrle_bitset communicator_descendants) {

						return workers::communicator_distributor(ctx, std::move(resultEndpoint),
							std::move(communicatorBroadcastEndpoint),
							communicator, std::move(communicator_descendants));

					}, std::ref(ctx), resultEndpoint,
						"inproc://communicator-broadcast", communicator, std::move(communicator_descendants)));

				// Relay to children
				downstream.control_socket.send(reader.eject(), zmq::send_flags::none);

				// Subscribe to subset tag
				net::subscribe_to_tag(upstream.control_socket, communicator.id);
			}

		// Determine if new stream is to be registered
		} else if (command.isStream()) {
			log("got new stream command\n");
			auto const newStreamCommand = command.getStream();
			auto const streamId = newStreamCommand.getStreamId();
			auto const filterLibraryName = newStreamCommand.getFilterLibraryName();
			auto const filterFunctionName = newStreamCommand.getFilterFunctionName();

			// Reconstitute stream object
			auto const stream = zmqnet::Stream { .communicatorId = reader.getTag(), .id = streamId };

			// Create new stream task worker
			streamAggregatorTasks.emplace_back(std::async(std::launch::async,
				[](zmq::context_t& ctx, std::string resultEndpoint,
					zmqnet::Stream stream, std::string filterPath, std::string filterFunctionName,
					lrle_bitset const& descendants_bitset) {

					return workers::stream_aggregator(ctx, std::move(resultEndpoint),
						stream, std::move(filterPath), std::move(filterFunctionName),
						descendants_bitset);

				}, std::ref(ctx), resultEndpoint,
					stream, filterBasePath + "/" + std::string{filterLibraryName}, filterFunctionName,
					std::ref(descendants_bitset)));

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
	for (auto&& stream_aggregator_task : streamAggregatorTasks) {
		auto const stream_rc = stream_aggregator_task.get();
		if (stream_rc) {
			log("stream worker failed with code %d\n", stream_rc);
			rc = -1;
		}
	}

	// Wait on stream worker threads
	log("waiting for communicator worker threads\n");
	for (auto&& communicator_distributor_task : communicatorDistributorTasks) {
		auto const communicator_rc = communicator_distributor_task.get();
		if (communicator_rc) {
			log("communicator worker failed with code %d\n", communicator_rc);
			rc = -1;
		}
	}

	// Construct exit message for proxy
	{ auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		auto exitResponse = response.initExit();

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
		auto exitAck = response.initExit();

		// Set rank bitset
		exitAck.setRanks(kj::ArrayPtr<kj::byte const>
			{ (unsigned char const*)descendants_bitset.compressed.data()
			, descendants_bitset.compressed.size()
		});

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
