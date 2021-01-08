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
		task_socket.bind(taskEndpoint);
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
	log("commnode %02x %d: sending stream create ack\n",
		stream.communicatorId, stream.id);
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
			{ auto const just_exited = response.getExit().getRanks().asBytes();
				auto const just_exited_descendants = descendants_bitset.and_buf(just_exited.begin(), just_exited.size());
				exited_descendants |= just_exited_descendants;
			}

			log("commnode %02x %d: descendant(s) exited\n",
				stream.communicatorId, stream.id);
			exited_update = true;

		// Check stream ID and user result
		} else {
			assert(reader.getTag() == stream.communicatorId);
			assert(response.isUser());
			assert(response.getUser().getStreamId() == stream.id);

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
			net::send_MessageBuilder(result_socket, stream.communicatorId, builder);

			// Reset in progress ranks
			reported_descendants = lrle_bitset{};
		}

		// If all children exited, end loop
		if (all_descendants_exited) {
			log("commnode %02x %d: exiting (no descendants remaining)\n",
				stream.communicatorId, stream.id);

			break;

		} else {
			auto const remaining
				= lrle_count(descendants_bitset.compressed.data(), descendants_bitset.compressed.size())
				- lrle_count(exited_descendants.compressed.data(), exited_descendants.compressed.size());
			log("commnode %02x %d: %zu descendants remaining\n",
				stream.communicatorId, stream.id, remaining);
			assert(remaining > 0);
		}
	}

	rc = 0;

	return rc;
}

static inline zmq::socket_t response_distributor(zmq::context_t& ctx, zmq::socket_t&& input_socket,
	std::string const& resultEndpoint, std::string const& communicatorCommandEndpoint,
	lrle_bitset const& descendants_bitset)
{
	// Open result socket to send this communicator's output to upstream proxy task
	log("commnode: connecting to task result %s\n",
		resultEndpoint.c_str());
	auto result_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	result_socket.connect(resultEndpoint);

	// Open communicator command socket to receive new communicator information
	log("commnode: connecting to communicator endpoint %s\n",
		communicatorCommandEndpoint.c_str());
	auto communicator_command_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	communicator_command_socket.connect(communicatorCommandEndpoint);

	// Store stream task sockets
	struct hash_Stream {
		size_t operator() (zmqnet::Stream const& stream) const {
			auto communicator_id_hash = std::hash<zmqnet::Communicator::Id>{}(stream.communicatorId);
			auto stream_id_hash = std::hash<zmqnet::Stream::Id>{}(stream.id);
			return communicator_id_hash + (net::communicator_ids::AllRanks + 1) * stream_id_hash;
		}
	};
	struct compare_Stream {
		bool operator() (zmqnet::Stream const& lhs, zmqnet::Stream const& rhs) const {
			return (lhs.communicatorId == rhs.communicatorId) && (lhs.id == rhs.id);
		}
	};
	auto taskSockets = std::unordered_map<zmqnet::Stream, zmq::socket_t, hash_Stream, compare_Stream>{};

	// Track exited descendants
	auto exited_descendants = lrle_bitset{};

	// Track starting communicators
	auto in_progress_communicator_id = zmqnet::Communicator::Id{0x0};
	auto in_progress_communicator_descendants = lrle_bitset{};
	auto in_progress_communicator_started_descendants = lrle_bitset{};

	while (true) {
		// Receive next message
		log("commnode: receiving message...\n");
		auto reader = net::recv_MessageReader(input_socket);
		auto response = reader.getRoot<NetworkResponse>();

		// If exit, clean up
		if (response.isExit()) {
			log("commnode: got exit response\n");
			assert(reader.getTag() == net::communicator_ids::AllRanks);

			// Relay to stream workers
			for (auto&& [stream, task_socket] : taskSockets) {
				log("commnode: relay exit message to stream %d in communicator %02x\n",
					stream.id, stream.communicatorId);
				net::send_MessageReader_copy(task_socket, reader);
			}

			// Add to exited descendants
			{ auto const exited_ranks = response.getExit().getRanks().asBytes();
				exited_descendants.or_eq_buf(exited_ranks.begin(), exited_ranks.size());
			}

			// Check if all descendants exited
			if (exited_descendants == descendants_bitset) {
				log("commnode: exiting (none remaining)\n");

				// All clients have exited, run cleanup
				break;
			}

			continue;

		// If communicator, add to in-progress communicator
		} else if (response.isCommunicator()) {

			// New communicator?
			if (in_progress_communicator_id == 0x0) {

				// Read new communicator command
				auto communicator_reader = net::recv_MessageReader(communicator_command_socket);
				auto command = communicator_reader.getRoot<NetworkCommand>();
				assert(command.isCommunicator());

				// Get new communicator ID
				assert(command.getCommunicator().getCommunicatorId() != 0x0);
				in_progress_communicator_id = command.getCommunicator().getCommunicatorId();

				// Extract communicator descendants and assign to target bitset
				auto const communicator_descendants = command.getCommunicator().getRanks().asBytes();
				in_progress_communicator_descendants = lrle_bitset
					{ communicator_descendants.begin()
					, communicator_descendants.size()
				};

				// Add to started descendants
				auto const started_communicator_descendants = response.getCommunicator().getRanks().asBytes();
				in_progress_communicator_started_descendants = lrle_bitset
					{ started_communicator_descendants.begin()
					, started_communicator_descendants.size()
				};

				log("commnode: starting communicator %02x create (target %s have %s)\n",
					in_progress_communicator_id,
					in_progress_communicator_descendants.to_string().c_str(),
					in_progress_communicator_started_descendants.to_string().c_str());

			// Mismatching communicator startup?
			} else if (response.getCommunicator().getCommunicatorId() != in_progress_communicator_id) {
				throw std::runtime_error("communicator "
					+ std::to_string(in_progress_communicator_id)
					+ " is starting, but received communicator response "
					+ std::to_string(response.getCommunicator().getCommunicatorId()));

			// Continue communicator startup
			} else {

				// Add to started descendants
				auto const communicator_descendants = response.getCommunicator().getRanks().asBytes();
					in_progress_communicator_started_descendants.or_eq_buf(
						communicator_descendants.begin(),
						communicator_descendants.size());

				log("commnode: continue communicator %02x create (target %s have %s)\n",
					in_progress_communicator_id,
					in_progress_communicator_descendants.to_string().c_str(),
					in_progress_communicator_started_descendants.to_string().c_str());
			}

			// If complete, send communicator open acknowledgement to parent
			if (in_progress_communicator_descendants == in_progress_communicator_started_descendants) {
				auto builder = capnp::MallocMessageBuilder{};
				auto response = builder.initRoot<NetworkResponse>();
				auto communicatorAck = response.initCommunicator();
				communicatorAck.setCommunicatorId(in_progress_communicator_id);

				// Set rank bitset
				communicatorAck.setRanks(kj::ArrayPtr<kj::byte const>
					{ (unsigned char const*)in_progress_communicator_descendants.compressed.data()
					, in_progress_communicator_descendants.compressed.size()
				});

				// Send acknowledgement
				log("commnode: sending communicator %02x create acknowledgement\n",
					in_progress_communicator_id);
				net::send_MessageBuilder(result_socket, net::communicator_ids::AllRanks, builder);

				// Reset in-progress communicator bitsets
				in_progress_communicator_id = 0x0;
				in_progress_communicator_descendants = lrle_bitset{};
				in_progress_communicator_started_descendants = lrle_bitset{};
			}

			continue;
		}

		// Content is either stream or user content
		assert(response.isStream() || response.isUser());

		// Extract stream ID to select stream worker
		auto const streamId = (response.isStream())
			? response.getStream().getStreamId()
			: response.getUser().getStreamId();

		// Reconstitute stream object
		auto const stream = zmqnet::Stream { .communicatorId = reader.getTag(), .id = streamId };

		// Get socket for given communicator & stream ID
		auto const streamSocketPair = taskSockets.find(stream);

		// Create new stream task connection with known endpoint format, if needed
		if (streamSocketPair == taskSockets.end()) {
			log("commnode: new stream %d on communicator %02x\n",
				stream.id, stream.communicatorId);
			assert(response.isStream());

			// Create task socket and store in stream task socket map
			taskSockets.emplace_hint(streamSocketPair, stream,
				async::make_task_socket(ctx, stream, std::move(reader)));

		// Relay message to task socket
		} else {
			log("commnode: relay message to stream %d on communicator %02x\n",
				streamId, stream.communicatorId);
			net::send_MessageReader(streamSocketPair->second, std::move(reader));
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
		auto reader = net::recv_MessageReader(downstream.data_socket);
		log("subchild %d/%d connection\n", i + 1, num_subchildren);
		net::send_MessageReader(upstream.data_socket, std::move(reader));
	}

	// Wait for leaf start command from frontend and relay
	log("children launched, waiting for rest of network to launch\n");
	{ auto reader = net::recv_MessageReader(upstream.control_socket);
		auto command = reader.getRoot<NetworkCommand>();
		assert(command.isStartLeaves());
		net::send_MessageReader(downstream.control_socket, std::move(reader));
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
	auto communicatorDescendants = std::unordered_map<zmqnet::Communicator::Id, lrle_bitset>
		{ { net::communicator_ids::AllRanks, lrle_bitset{} }
	};

	// Wait for all children to connect and send ready
	for (int i = 0; i < num_children; i++) {
		log("waiting for child %d / %d\n", i + 1, num_children);
		auto reader = net::recv_MessageReader(downstream.data_socket);
		auto response = reader.getRoot<NetworkResponse>();
		assert(response.isReady());

		// Add ranks to children rank bitset
		auto const response_rank_bitset = response.getReady().getRanks().asBytes();
		communicatorDescendants.at(net::communicator_ids::AllRanks).or_eq_buf(
			response_rank_bitset.begin(),
			response_rank_bitset.size());
	}

	// Send ready to parent
	log("sending ready to parent\n");
	{ auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		auto ready = response.initReady();

		// Set rank bitset
		ready.setRanks(kj::ArrayPtr<kj::byte const>
			{ (unsigned char const*)communicatorDescendants.at(net::communicator_ids::AllRanks).compressed.data()
			, communicatorDescendants.at(net::communicator_ids::AllRanks).compressed.size()
		});

		net::send_MessageBuilder(upstream.data_socket, net::communicator_ids::AllRanks, builder);
	}

	// Proxy local worker result socket to upstream socket
	auto const resultEndpoint = "inproc://result";
	auto result_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	result_socket.connect(resultEndpoint);

	// Create worker thread to deliver results upstream, return control of upstream data socket
	auto responseCollector = std::async(std::launch::async,
		[](zmq::context_t& ctx, std::string resultEndpoint, zmq::socket_t&& output_socket) {
			return async::collect_network_responses(ctx, std::move(resultEndpoint), std::move(output_socket));
		}, std::ref(ctx), resultEndpoint, std::move(upstream.data_socket));

	// Connect for communicator create commands
	auto const communicatorCommandEndpoint = "inproc://communicator-command";
	auto communicator_command_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	communicator_command_socket.bind(communicatorCommandEndpoint);

	// Receive incoming stream data messages and distribute to respective stream task,
	// return control of downstream data socket
	auto responseDistributor = std::async(std::launch::async,
		[](zmq::context_t& ctx, zmq::socket_t&& input_socket, std::string resultEndpoint, std::string communicatorCommandEndpoint, lrle_bitset descendants_bitset) {

			return workers::response_distributor(ctx, std::move(input_socket), std::move(resultEndpoint), std::move(communicatorCommandEndpoint), std::move(descendants_bitset));

		}, std::ref(ctx), std::move(downstream.data_socket), resultEndpoint, communicatorCommandEndpoint, communicatorDescendants.at(net::communicator_ids::AllRanks));

	// Command loop
	auto streamAggregatorTasks = std::vector<std::thread>{};
	auto const filterBasePath = std::string{cti_be_getLibDir()};

	while (true) {

		// Receive next command from parent
		log("waiting for command from upstream\n");
		auto reader = net::recv_MessageReader(upstream.control_socket);
		auto command = reader.getRoot<NetworkCommand>();

		// Determine if exit command was received
		if (command.isExit()) {
			log("got exit command\n");

			// Relay command to children
			net::send_MessageReader(downstream.control_socket, std::move(reader));

			break;

		} else if (command.isStartLeaves()) {
			log("unexpected: startLeaves\n");
			continue;

		// Determine if new communicator is to be registered and subscribed
		} else if (command.isCommunicator()) {
			log("got new communicator command\n");

			// Extract communicator ID
			auto const communicator_id = command.getCommunicator().getCommunicatorId();

			// Find set of descendants in the communicator subset
			auto communicator_descendants = lrle_bitset{};

			// If communicator is all ranks, use preexisting descendants bitset
			if (communicator_id == net::communicator_ids::AllRanks) {
				log("communicator contains all ranks\n");
				communicator_descendants = communicatorDescendants.at(net::communicator_ids::AllRanks);

			// Run bitset intersection
			} else {

				// Get communicator rank mask
				auto const communicator_ranks_bitset = command.getCommunicator().getRanks().asBytes();

				// Determine if any descendants are in the communicator subset
				auto const& all_descendants = communicatorDescendants.at(net::communicator_ids::AllRanks);
				if (!lrle_any_intersection(
					all_descendants.compressed.data(), all_descendants.compressed.size(),
					communicator_ranks_bitset.begin(), communicator_ranks_bitset.size())) {

					log("no descendants match\n");
					continue;
				}

				// Find descendants intersection
				communicator_descendants = all_descendants.and_buf(
					communicator_ranks_bitset.begin(),
					communicator_ranks_bitset.size());

				log("descendants match: %s\n",
					communicator_descendants.to_string().c_str());
			}

			// Subscribe to subset tag
			log("subscribing to communicator %02x\n", communicator_id);
			net::subscribe_to_tag(upstream.control_socket, communicator_id);

			// Send communicator create command to response worker
			auto builder = capnp::MallocMessageBuilder{};
			auto command = builder.initRoot<NetworkCommand>();
			auto communicatorCommand = command.initCommunicator();
			communicatorCommand.setCommunicatorId(communicator_id);

			// Set rank bitset
			communicatorCommand.setRanks(kj::ArrayPtr<kj::byte const>
				{ (unsigned char const*)communicator_descendants.compressed.data()
				, communicator_descendants.compressed.size()
			});

			// Relay to response thread
			net::send_MessageBuilder(communicator_command_socket, net::communicator_ids::AllRanks, builder);

			// Relay to children. TODO: avoid builder copy
			net::send_MessageBuilder(downstream.control_socket, net::communicator_ids::AllRanks, builder);

			// Store communicator descendants bitset
			communicatorDescendants.emplace(communicator_id, std::move(communicator_descendants));

		// Determine if new stream is to be registered
		} else if (command.isStream()) {
			auto const newStreamCommand = command.getStream();
			auto const streamId = newStreamCommand.getStreamId();
			auto const filterLibraryName = newStreamCommand.getFilterLibraryName();
			auto const filterFunctionName = newStreamCommand.getFilterFunctionName();

			// Reconstitute stream object
			auto const stream = zmqnet::Stream { .communicatorId = reader.getTag(), .id = streamId };
			log("got new stream %d command on communicator %02x\n",
				stream.id, stream.communicatorId);

			// Relay to children
			net::send_MessageReader(downstream.control_socket, std::move(reader));

			// Create new stream task worker
			streamAggregatorTasks.emplace_back(
				[](zmq::context_t& ctx, std::string resultEndpoint,
					zmqnet::Stream stream, std::string filterPath, std::string filterFunctionName,
					lrle_bitset descendants_bitset) {

					(void)workers::stream_aggregator(ctx, std::move(resultEndpoint),
						stream, std::move(filterPath), std::move(filterFunctionName),
						std::move(descendants_bitset));

				}, std::ref(ctx), resultEndpoint,
					stream, filterBasePath + "/" + std::string{filterLibraryName}, filterFunctionName,
					communicatorDescendants.at(stream.communicatorId));

		} else {
			log("got user command on stream %d communicator %02x\n",
				reader.getTag(), command.getUser().getStreamId());

			// Relay command to children
			net::send_MessageReader(downstream.control_socket, std::move(reader));
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
		stream_aggregator_task.join();
	}

	// Construct exit message for proxy
	{ auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		auto exitResponse = response.initExit();

		// Send exit message to proxy
		net::send_MessageBuilder(result_socket, net::communicator_ids::AllRanks, builder);
	}

	// Wait for proxy task to complete and regain ownership of upstream data socket
	log("waiting for proxy task\n");
	upstream.data_socket = responseCollector.get();

	// Clean up local child PIDs
	for (auto&& pid : localChildPids) {
		log("waiting for child pid: %d\n", pid);

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
		auto const& all_descendants = communicatorDescendants.at(net::communicator_ids::AllRanks);
		exitAck.setRanks(kj::ArrayPtr<kj::byte const>
			{ (unsigned char const*)all_descendants.compressed.data()
			, all_descendants.compressed.size()
		});

		net::send_MessageBuilder(upstream.data_socket, net::communicator_ids::AllRanks, builder);
		log("parent notified of exit\n");
	}

	// Close all network sockets
	upstream.close();
	downstream.close();
	log("context reset\n");
	ctx.close();

	log("commnode cleaned up and exiting\n");
	fprintf(stderr, "%05lu: exiting\n", get_milliseconds() - start_ms);

	return rc;
}
