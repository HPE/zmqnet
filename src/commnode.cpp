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
#include <fstream>
#include <sstream>
#include <atomic>
#include <mutex>
#include <signal.h>

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
	auto authFile = std::string{};
	auto authType = zmqnet::Auth::None;
	auto dealerEndpoint = std::string{};
	auto nodeConnectionMap = std::string{};
	auto packed_mode = false;
	auto packed_node_id = int{-1};

	using CA = CommnodeArgv;
	auto parsedArgv = cti::IncomingArgv<CA>{argc, argv};

	// Read FE-passed options
	while (true) {
		auto [c, value] = parsedArgv.get_next();
		if (c <= 0) { break; }

		switch (c) {

		case CA::AuthFile.val:
			authFile = std::move(value);
			break;

		case CA::AuthType.val:
			authType = (zmqnet::Auth)std::stoi(value);
			break;

		case CA::Packed.val:
			packed_mode = true;
			break;

		case CA::DealerEndpoint.val:
			dealerEndpoint = std::move(value);
			break;

		case CA::NodeConnectionMap.val:
			nodeConnectionMap = std::move(value);
			break;

		case CA::PackedNodeId.val:
			packed_node_id = std::stoi(value);
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
	return std::make_tuple(
		std::move(authFile), authType,
		std::move(dealerEndpoint), std::move(nodeConnectionMap),
		packed_mode, packed_node_id,
		leaf_argc, leaf_argv);
}

// Parse node topology and parent connection data from argument string
static auto parseNodeConnectionMap(std::string&& nodeConnectionMap)
{
	auto node_id = int{0};
	auto parentControlEndpoint = std::string{};
	auto parentDataEndpoint = std::string{};
	auto parentPublicKey = std::string{};
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
			node_id++;
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

	// Find connection information for parent node from parent index
	{ auto nodeConnection = std::string{};
		auto nodeConnectionStream = std::istringstream{std::move(connectionData)};

		// Get parent node data at index
		for (int i = 0; i <= parent_node_index; i++) {
			if (!std::getline(nodeConnectionStream, nodeConnection, ',')) {
				throw std::runtime_error("malformed connection map: invalid parent node index");
			}
		}

		// Split out parent connection data
		auto [controlEndpoint, dataEndpoint, publicKey]
			= cti::split::string<3>(std::move(nodeConnection), ' ');
		parentControlEndpoint = std::move(controlEndpoint);
		parentDataEndpoint = std::move(dataEndpoint);
		parentPublicKey = std::move(publicKey);
	}

cleanup_parseNodeConnectionMap:
	return std::make_tuple(node_id, std::move(parentControlEndpoint), std::move(parentDataEndpoint), std::move(parentPublicKey), commnode_children, commnode_subchildren);
}

// Commnode child info
struct CommChild {
	enum class Type {
		Commnode,
		Leaf
	};
	
	CommChild(Type _type, pid_t _pid, int id)
		: type{_type}, pid{_pid}, nodeId{id}
	{}
	
	bool operator==(const CommChild& other) const {
		return type == other.type &&
			pid == other.pid &&
			nodeId == other.nodeId
		;
	}

	Type type;
	pid_t pid;
	int nodeId; // -1 if commnode
};

class Descendants {
	// Thread safe mapping of communicator ids to descendant bitsets
public:
	// Wait for `numChildren` NetworkResponse.Ready messages on `input_socket`.
	// If `timeout` seconds pass before all children have connected, throws std::runtime_error.
	Descendants(zmq::socket_t& input_socket, int numChildren, int timeout) {
		// Temporary timeout during startup
		auto oldTimeout = input_socket.get(zmq::sockopt::rcvtimeo);
		if (timeout >= 0) {
			input_socket.set(zmq::sockopt::rcvtimeo, timeout * 1000); // milliseconds;
		}

		// Wait for all children to connect and send ready
		for (int i = 0; i < numChildren; i++) {
			log("waiting for child %d / %d\n", i + 1, numChildren);
			auto maybeReader = net::recv_MessageReader_timeout(input_socket);
			if (!maybeReader) {
				log("timed out waiting for children. Exiting.\n");
				input_socket.set(zmq::sockopt::rcvtimeo, oldTimeout);
				throw std::runtime_error("Timed out");
			}
			auto response = maybeReader->getRoot<NetworkResponse>();
			assert(response.isReady());

			// Add nodes to children node bitset
			auto const response_node_bitset = response.getReady().getNodes().asBytes();
			m_allChildren.or_eq_buf(
				response_node_bitset.begin(),
				response_node_bitset.size()
			);
		}

		input_socket.set(zmq::sockopt::rcvtimeo, oldTimeout);
	}

	const lrle_bitset& getAllSet() const {
		return m_allChildren;
	};

	const lrle_bitset& at(zmqnet::Communicator::Id key) const {
		auto lock = std::scoped_lock<std::mutex>(m_mapLock);
		return m_children.at(key);
	}

	void emplace(zmqnet::Communicator::Id key, lrle_bitset&& expiring) {
		auto lock = std::scoped_lock<std::mutex>(m_mapLock);
		m_children.emplace(key, std::move(expiring));
	}
private:
	// special "all children" set, by far the most often read of any set
	lrle_bitset m_allChildren;
	mutable std::mutex m_mapLock;
	std::unordered_map<zmqnet::Communicator::Id, lrle_bitset> m_children;
};

// change the signal mask of the calling thread such that all signals are blocked
static void blockAllSignals() {
	sigset_t all_signals;
	sigfillset(&all_signals);
	pthread_sigmask(SIG_BLOCK, &all_signals, nullptr);
}

static void unblockAllSignals() {
	sigset_t all_signals;
	sigfillset(&all_signals);
	pthread_sigmask(SIG_UNBLOCK, &all_signals, nullptr);
}

template <typename T>
class ThreadSafeSet {
public:
	void insert(const T& x) {
		auto lock = std::scoped_lock<std::mutex>(m_lock);
		if (std::find(m_set.begin(), m_set.end(), x) == m_set.end()) {
			m_set.push_back(x);
		}
	}

	void remove(const T& x) {
		auto lock = std::scoped_lock<std::mutex>(m_lock);
		if (auto it = std::find(m_set.begin(), m_set.end(), x); it != m_set.end()) {
			m_set.erase(it);
		}
	}

	template <typename FN>
	std::optional<T> removeIf(FN&& pred) {
		auto lock = std::scoped_lock(m_lock);
		auto it = std::find_if(m_set.begin(), m_set.end(), pred);
		if (it != m_set.end()) {
			auto r = std::move(*it);
			m_set.erase(it);
			return r;
		} else {
			return {};
		}
	}

	bool contains(const T& x) const {
		auto lock = std::scoped_lock<std::mutex>(m_lock);
		return std::find(m_set.begin(), m_set.end(), x) != m_set.end();
	}

	template <typename FN>
	bool containsIf(FN&& pred) const {
		auto lock = std::scoped_lock(m_lock);
		return std::find_if(m_set.begin(), m_set.end(), pred) != m_set.end();
	}

	template <typename FN>
	void forEach(FN&& fn) {
		auto lock = std::scoped_lock<std::mutex>(m_lock);
		for (auto& x : m_set) {
			fn(x);
		}
	}

private:
	mutable std::mutex m_lock;
	std::vector<T> m_set;
};

namespace workers {

namespace endpts {
// socket endpoints used throughout the commnode

// input to main thread (data flows downstream)
auto const main_loop = "inproc://main_loop";

// input to main response thread (data flows upstream)
auto const response_distributor = "inproc://response_distributor";
// communicator_cmd is a secondary input socket for response_distributor
auto const communicator_cmd = "inproc://communicator_cmd";

// input to upstream proxy (data flows upstream)
auto const upstream_sink = "inproc://upstream_sink";

// stream aggregator endpoints are generated at runtime by response_distributor
} // namespace endpts

// Handle all external signals, with special interest in exited children (SIGCHLD)
void handle_all_signals(zmq::context_t& ctx, std::atomic<bool>& exitFlag, ThreadSafeSet<CommChild>& childPids) {
	// block all signals so their default action doesn't happen and bypass us
	// (this should have already happened in calling thread, but just in case)
	blockAllSignals();

	{	// allow SIGINT and SIGTERM:
		// 1: to keep a sane behavior for these standard signals
		// 2: to enable ctrl-c to work nicely while debugging the commnode in gdb
		//    (gdb can't intercept a blocked signal)
		sigset_t fatal_set;
		sigemptyset(&fatal_set);
		sigaddset(&fatal_set, SIGINT);
		sigaddset(&fatal_set, SIGTERM);
		pthread_sigmask(SIG_UNBLOCK, &fatal_set, nullptr);
	}

	// Connect to response distributor
	auto responseDistributorSocket = zmq::socket_t{ctx, zmq::socket_type::push};
	responseDistributorSocket.connect(endpts::response_distributor);

	// Handle all signals
	sigset_t all_signals;
	sigfillset(&all_signals);
	const auto two_seconds = timespec{.tv_sec = 2, .tv_nsec = 0};

	while (!exitFlag) {
		siginfo_t info;
		switch (auto sig = sigtimedwait(&all_signals, &info, &two_seconds)) {
			case -1: {
				// got nothing, try again
				break;
			}

			case SIGINT: // fall through
			case SIGTERM: {
				log("handle_signals: got fatal signal %d, passing to handlers.\n", sig);
				raise(sig); // pass to the installed (likely default) handler or gdb
			}

			case SIGCHLD: {
				log("handle_signals: got SIGCHLD.\n");

				bool isOurChild = childPids.containsIf([&info](auto&& child){
					return child.pid == info.si_pid;
				});

				// .si_code isn't a bitmask, unfortunately. need to check the long way.
				if (isOurChild && (
					info.si_code == CLD_EXITED || // normal exit
					info.si_code == CLD_KILLED || // killed
					info.si_code == CLD_DUMPED    // terminated abnomally
				)) {
					int status;
					auto pid = waitpid(info.si_pid, &status, WNOHANG);

					if (pid == info.si_pid) {
						log("handle_signals: child %d exited!\n", pid);

						auto maybeChild = childPids.removeIf([&pid](auto&& chld) {
							return chld.pid == pid;
						});

						if (maybeChild) {
							auto& child = *maybeChild;
							if (child.type == CommChild::Type::Commnode) {
								log("handle_signals: child %d was a commnode.\n", pid);
								// TODO: panic!
							} else {
								// type == Leaf
								log("handle_signals: child %d was a leaf.\n", pid);
								// Fake an exit message from the node that disappeared
								auto builder = capnp::MallocMessageBuilder{};
								auto response = builder.initRoot<NetworkResponse>();
								auto exit = response.initExit();

								exit.setNodeType(NodeType::LEAF);
								auto bitset = lrle_bitset((size_t)child.nodeId);
								exit.setNodes(kj::ArrayPtr<kj::byte const>((unsigned char const*)bitset.compressed.data(), bitset.compressed.size()));

								net::send_MessageBuilder(responseDistributorSocket, net::communicator_ids::AllNodes, builder);
							}
						} else {
							log("handle_signals: tried to remove child with pid %d, but didn't find one in childPids.\n", pid);
						}
					} else {
						log("handle_signals: got SIGCHLD and matched known child %d, but waitpid returned something else.\n", info.si_pid);
						log("handle_signals: waitpid returned pid %d.\n", pid);
					}
				} else {
					log("handle_signals: got SIGCHLD but didn't meet requirements for registering an exit.\n");
					log("handle_signals: SIGCHLD info: info.si_code: %d, info.si_pid: %d\n", info.si_code, info.si_pid);
				}
				break;
			}

			default: {
				log("handle_signals: unhandled signal %d. ignoring.\n", sig);
				break;
			}
		}
	}
	log("handle_signals: returning!\n");
}

// Runtime-selectable message synchronization implementations
namespace
{

class Sync
{
protected: // types
	using BitsetContentPair = std::pair<lrle_bitset, std::vector<zmqnet::ByteArray>>;

protected: // members

	// Synchronization implementations store the descendant bitset to determine if / when
	// to eject messages for aggregation
	unsigned char const* descendants_buf;
	size_t descendants_len;

protected: // inherited interface
	Sync(unsigned char const* descendants_buf_, size_t descendants_len_)
		: descendants_buf{descendants_buf_}
		, descendants_len{descendants_len_}
	{}

public: // interface
	virtual ~Sync() = default;

	// Return all stored messages and bitset of the source descendants. Resets state.
	// Called for aggregation when all descendants have reported or exited
	virtual BitsetContentPair eject() = 0;

	// Return all stored messages and bitset of the source descendants. Does not reset state.
	// Called in timeout handler for incoming message socket.
	virtual BitsetContentPair timeout() = 0;

	// Insert new content, reported by the descendants contained in bitset
	virtual BitsetContentPair update(unsigned char const* new_descendants_buf, size_t new_descendants_len,
		zmqnet::ByteArray&& content) = 0;
};

// Immediate: messages are sent as soon as they are received
class Sync_Immediate : public Sync
{
public: // interface
	Sync_Immediate(unsigned char const* descendants_buf, size_t descendants_len)
		: Sync{descendants_buf, descendants_len}
	{}

	// Immediate stores no messages, so it always ejects an empty content array
	BitsetContentPair eject()	override { return BitsetContentPair{}; }
	BitsetContentPair timeout() override { return BitsetContentPair{}; }

	// Create and return a content array with one element, the new content
	BitsetContentPair update(unsigned char const* new_descendants_buf, size_t new_descendants_len, zmqnet::ByteArray&& content) override
	{
		auto bitset = lrle_bitset{new_descendants_buf, new_descendants_len};
		auto contents = std::vector<zmqnet::ByteArray>{};
		if (content.size() > 0) {
			contents.emplace_back(std::move(content));
		}
		return BitsetContentPair{std::move(bitset), std::move(contents)};
	}
};

// WaitForAll: store all messages until one has been received from every descendant
struct Sync_WaitForAll : public Sync
{
private: // members
	// Reference to exited_descendants bitset updated by containing scope
	lrle_bitset& exited_descendants;
	// Track descendants from which a message has been received
	lrle_bitset reported_descendants;

	// Store all received content until ejected or received from all descentants
	std::vector<zmqnet::ByteArray> content;

public: // interface
	Sync_WaitForAll(unsigned char const* descendants_buf, size_t descendants_len, lrle_bitset& exited)
		: Sync{descendants_buf, descendants_len}
		, exited_descendants{exited}
		, reported_descendants{}
		, content{}
	{}

	// Eject all stored messages for aggregation
	BitsetContentPair eject() override
	{
		auto bitset = std::move(reported_descendants);

		// exited descendants are considered always accounted for in aggregation
		bitset |= exited_descendants;

		auto contents = std::move(content);

		reported_descendants.clear();
		content.clear();

		return BitsetContentPair{std::move(bitset), std::move(contents)};
	}

	// WaitForAll only ejects messages when one is received from each descendant,
	// ignore any requests for timeout
	BitsetContentPair timeout() override { return BitsetContentPair{}; }

	// Store the provided message, eject for aggregation if it was received from the last descendant
	BitsetContentPair update(unsigned char const* new_descendants_buf, size_t new_descendants_len, zmqnet::ByteArray&& new_content) override
	{
		// Add content to in-progress result list
		if (new_content.size() > 0) {
			content.emplace_back(std::move(new_content));
		}

		// Add to reported descendants
		reported_descendants.or_eq_buf(new_descendants_buf, new_descendants_len);

		// exited descendants are considered always accounted for in aggregation
		reported_descendants |= exited_descendants;

		// Return for aggregation if all descendants have reported
		if (reported_descendants.eq_buf(descendants_buf, descendants_len)) {
			return eject();
		}

		return BitsetContentPair{};
	}
};

// Timeout: store all messages until one has been received from every descendant, or until
// `timeout` method is called (triggered by socket read timeout at the aggregation level)
class Sync_Timeout : public Sync
{

	// Reported descendants may contain timed out descendants
	lrle_bitset reported_descendants;
	// Current descendants are those reported since the last timeout or full aggregation
	lrle_bitset current_descendants;
	// Both must be tracked for the case where half of the descendants time out, then the other
	// half is received. Overall, messages have been received from all descendants, but the first
	// half timed out and were ejected for aggregation.

	// Reference to exited_descendants bitset updated by containing scope
	lrle_bitset& exited_descendants;

	std::vector<zmqnet::ByteArray> content;

public: // interface
	Sync_Timeout(unsigned char const* descendants_buf, size_t descendants_len, lrle_bitset& exited)
		: Sync{descendants_buf, descendants_len}
		, reported_descendants{}
		, current_descendants{}
		, exited_descendants{exited}
		, content{}
	{}

	// Eject all stored messages for aggregation, clear all descendants
	BitsetContentPair eject() override
	{
		auto bitset = std::move(current_descendants);
		auto contents = std::move(content);

		// exited descendants are considered always accounted for in aggregation
		bitset |= exited_descendants;

		reported_descendants.clear();
		current_descendants.clear();
		content.clear();

		return BitsetContentPair{std::move(bitset), std::move(contents)};
	}

	// Eject all stored messages for aggregation, but keep track of timed-out descendants
	BitsetContentPair timeout() override
	{
		auto bitset = std::move(current_descendants);
		auto contents = std::move(content);

		// exited descendants are considered always accounted for in aggregation
		bitset |= exited_descendants;

		current_descendants.clear();
		content.clear();

		return BitsetContentPair{std::move(bitset), std::move(contents)};
	}

	// Store the provided message, eject for aggregation if it was received from the last descendant
	BitsetContentPair update(unsigned char const* new_descendants_buf, size_t new_descendants_len, zmqnet::ByteArray&& new_content) override
	{
		// Add content to in-progress result list
		if (new_content.size() > 0) {
			content.emplace_back(std::move(new_content));
		}

		// Add to reported and current descendants
		current_descendants.or_eq_buf(new_descendants_buf, new_descendants_len);
		current_descendants |= exited_descendants;
		reported_descendants.or_eq_buf(new_descendants_buf, new_descendants_len);
		reported_descendants |= exited_descendants;

		// Return for aggregation if all descendants have reported
		if (reported_descendants.eq_buf(descendants_buf, descendants_len)) {
			return eject();
		}

		return BitsetContentPair{};
	}
};

} // anonymous namespace

// Receive messages from upstream.control, intercept communicator commands,
// and forward the rest to endpts::main_loop
static zmq::socket_t upstream_source(zmq::context_t& ctx, zmq::socket_t&& input_socket, Descendants& descendants)
{
	// Receive messages from upstream and proxy them all to
	// main_loop. Communicator operations have to be done
	// in this thread, as those directly modify the input socket.

	auto main_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	main_socket.connect(endpts::main_loop);

	try {
		// Main thread will call ctx.shutdown(), causing ETERM to be thrown
		while (true) {
			log("upstream_source: waiting for command from upstream\n");
			auto reader = net::recv_MessageReader(input_socket);
			auto command = reader.getRoot<NetworkCommand>();

			if (command.isCommunicator()) {
				log("upstream_source: got new communicator command\n");

				// Extract communicator ID
				auto const communicatorId = command.getCommunicator().getCommunicatorId();

				// Find set of descendants in the communicator subset
				auto communicator_descendants = lrle_bitset{};

				// If communicator is all nodes, use preexisting descendants bitset
				if (communicatorId == net::communicator_ids::AllNodes) {
					log("communicator contains all nodes\n");
					communicator_descendants = descendants.getAllSet();

				// Run bitset intersection
				} else {

					// Get communicator node mask
					auto const communicator_nodes_bitset = command.getCommunicator().getNodes().asBytes();

					// Determine if any descendants are in the communicator subset
					auto const& all_descendants = descendants.getAllSet();
					if (!lrle_any_intersection(
						all_descendants.compressed.data(), all_descendants.compressed.size(),
						communicator_nodes_bitset.begin(), communicator_nodes_bitset.size())) {

						log("no descendants match\n");
						continue;
					}

					// Find descendants intersection
					communicator_descendants = all_descendants.and_buf(
						communicator_nodes_bitset.begin(),
						communicator_nodes_bitset.size());

					log("descendants match: %s\n",
						communicator_descendants.to_string().c_str());
				}

				// Subscribe to subset tag
				log("subscribing to communicator %02x\n", communicatorId);
				net::subscribe_to_tag(input_socket, communicatorId);
				
				auto builder = capnp::MallocMessageBuilder{};
				auto command = builder.initRoot<NetworkCommand>();
				auto communicatorCommand = command.initCommunicator();
				communicatorCommand.setCommunicatorId(communicatorId);

				// Set node bitset
				communicatorCommand.setNodes(kj::ArrayPtr<kj::byte const>
					{ (unsigned char const*)communicator_descendants.compressed.data()
					, communicator_descendants.compressed.size()
				});

				// Store communicator descendants bitset
				descendants.emplace(communicatorId, std::move(communicator_descendants));

				// relay to main
				net::send_MessageBuilder(main_socket, net::communicator_ids::AllNodes, builder);

				// don't want to send again below
				continue;
			}

			// proxy to main
			log("upstream_source: generic proxy to main.\n");
			net::send_MessageReader(main_socket, std::move(reader));
		}
	} catch (zmq::error_t& e) {
		if (e.num() == ETERM) {
			log("upstream_source: got ETERM, exiting.\n");
		} else {
			log("upstream_source: warning: Unexpected error %d in upstream_source. (%s)\n", e.num(), e.what());
		}
	}

	log("upstream_source: returning!\n");
	return std::move(input_socket);
}

// Process result messages; aggregate and send to local result socket
static int stream_aggregator(zmq::context_t& ctx, std::string resultEndpoint,
	zmqnet::Stream const& stream, std::string filterPath, std::string filterFunctionName,
	lrle_bitset const& descendants_bitset, zmqnet::StreamSync const& streamSync)
{
	int rc = -1;

	// Open task socket to receive this stream's input from downstream data task
	auto task_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	{ auto const taskEndpoint = async::stream_endpoint(stream);
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
	log("commnode %02x %d: opening filter at %s::%s\n",
		stream.communicatorId, stream.id, filterPath.c_str(), filterFunctionName.c_str());
	auto filterHandle = cti::Dlopen::Handle{filterPath};
	auto filterRun = filterHandle.load<zmqnet::FilterType>(filterFunctionName);

	// Wait until all downstream children have created this stream
	for (auto started_descendants = lrle_bitset{}; started_descendants != descendants_bitset; ) {
		log("commnode %02x %d: waiting for descendants stream create (target %s have %s)\n",
			stream.communicatorId, stream.id, descendants_bitset.to_string().c_str(), started_descendants.to_string().c_str());
		auto reader = net::recv_MessageReader(task_socket);
		auto response = reader.getRoot<NetworkResponse>();
		assert(response.isStream() && (response.getStream().getStreamId() == stream.id));

		// Add to started descendants
		{ auto const started_nodes = response.getStream().getNodes().asBytes();
			started_descendants.or_eq_buf(started_nodes.begin(), started_nodes.size());
		}
	}

	// Send stream open acknowledgement to parent
	log("commnode %02x %d: sending stream create ack\n",
		stream.communicatorId, stream.id);
	{ auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		auto streamResponse = response.initStream();
		streamResponse.setStreamId(stream.id);

		// Set node bitset
		streamResponse.setNodes(kj::ArrayPtr<kj::byte const>
			{ (unsigned char const*)descendants_bitset.compressed.data()
			, descendants_bitset.compressed.size()
		});

		net::send_MessageBuilder(result_socket, stream.communicatorId, builder);
	}

	auto exited_descendants = lrle_bitset{};

	// Select synchronization implementation
	auto synchronizer = std::unique_ptr<Sync>{};
	{ auto const descendants_buf = descendants_bitset.compressed.data();
		auto const descendants_len = descendants_bitset.compressed.size();

		// Immediate processing
		if (std::get_if<zmqnet::sync::Immediate>(&streamSync)) {
			synchronizer = std::make_unique<Sync_Immediate>(descendants_buf, descendants_len);

		// Wait for all descendants reporting
		} else if (std::get_if<zmqnet::sync::WaitForAll>(&streamSync)) {
			synchronizer = std::make_unique<Sync_WaitForAll>(descendants_buf, descendants_len, exited_descendants);

		// Aggregate when all reported, or timeout hit
		} else if (auto const timeout = std::get_if<zmqnet::sync::Timeout>(&streamSync)) {
			synchronizer = std::make_unique<Sync_Timeout>(descendants_buf, descendants_len, exited_descendants);

			// Apply timeout to socket
			task_socket.set(zmq::sockopt::rcvtimeo, timeout->ms);

		} else {
			assert(false);
		}
	}

	// Filter synchronization, aggregate and send results when all children have reported
	while (true) {
		auto aggregate_and_send = [&](lrle_bitset&& reported_descendants, std::vector<zmqnet::ByteArray>&& results) {
			if (results.empty()) {
				return;
			}

			// Run user filter
			log("commnode %02x %d: aggregating %zu messages\n",
					stream.communicatorId, stream.id, results.size());
			auto content = filterRun(std::move(results));

			// Create outgoing result message
			auto builder = capnp::MallocMessageBuilder{};
			auto response = builder.initRoot<NetworkResponse>();
			auto userResult = response.initUser();
			userResult.setStreamId(stream.id);
			auto const result_size = content.size();
			userResult.setContent(std::move(content));

			// Set node bitset
			userResult.setNodes(kj::ArrayPtr<kj::byte const>
				{ (unsigned char const*)reported_descendants.compressed.data()
				, reported_descendants.compressed.size()
			});

			// Send result
			log("commnode %02x %d: sending result size %lu\n",
				stream.communicatorId, stream.id, result_size);
			net::send_MessageBuilder(result_socket, stream.communicatorId, builder);
		};

		// Receive result from task socket
		log("commnode %02x %d: waiting for result from task socket\n",
			stream.communicatorId, stream.id);
		if (auto maybeReader = net::recv_MessageReader_timeout(task_socket)) {
			auto& reader = *maybeReader;
			auto response = reader.getRoot<NetworkResponse>();

			// If exit message, update exited descendants, possibly aggregate and exit
			if (response.isExit()) {
				log("commnode %02x %d: descendant(s) exited\n",
					stream.communicatorId, stream.id);

				// Add to exited descendants
				{ auto const just_exited = response.getExit().getNodes().asBytes();
					auto const just_exited_descendants = descendants_bitset.and_buf(just_exited.begin(), just_exited.size());
					exited_descendants |= just_exited_descendants;

					// The removal of the child node might make synchronizer ready to send
					auto [bitset, content] = synchronizer->update(
						just_exited_descendants.compressed.data(),
						just_exited_descendants.compressed.size(),
						zmqnet::ByteArray{}
					);

					// Synchronizer reported results to send, aggregate
					if (!bitset.empty()) {
						aggregate_and_send(std::move(bitset), std::move(content));
					}
				}

				// Aggregate when all children have reported a result, or all descendants exit
				if (exited_descendants == descendants_bitset) {
					auto [bitset, content] = synchronizer->eject();
					aggregate_and_send(std::move(bitset), std::move(content));

					log("commnode %02x %d: exiting (no descendants remaining)\n",
						stream.communicatorId, stream.id);
					break;
				}

			// Check stream ID and user result
			} else {
				assert(response.isUser());
				assert(response.getUser().getStreamId() == stream.id);
				log("commnode %02x %d: user content length: %lu\n",
					stream.communicatorId, stream.id, response.getUser().getContent().size());

				// Add content to in-progress result list
				auto const new_descendants = response.getUser().getNodes().asBytes();
				auto [bitset, content] = synchronizer->update(new_descendants.begin(), new_descendants.size(),
					kj::heapArray(response.getUser().getContent().asBytes()));

				// Synchronizer reported results to send, aggregate
				if (!bitset.empty()) {
					aggregate_and_send(std::move(bitset), std::move(content));
				}
			}

		// Socket hit timeout, send any timed-out synchronizer contents
		} else {
			assert(std::get_if<zmqnet::sync::Timeout>(&streamSync));

			auto [bitset, content] = synchronizer->timeout();

			// If synchronizer reported results to send, aggregate
			if (!bitset.empty()) {
				aggregate_and_send(std::move(bitset), std::move(content));
			}
		}
	}

	rc = 0;

	log("commnode %02x %d: returning!\n", stream.communicatorId, stream.id);
	return rc;
}

// Receive messages from endpts::response_distributor, distribute to endpts::upstream_sink,
// endpts::main_loop, or stream aggregation threads depending on message
static inline void response_distributor(zmq::context_t& ctx, lrle_bitset const& descendants_bitset)
{
	// Bind to input socket
	log("commnode: binding to response_distributor input socket at %s\n", endpts::response_distributor);
	auto input_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	input_socket.bind(endpts::response_distributor);

	// Open result socket to send this communicator's output to upstream proxy task
	log("commnode: connecting to task result %s\n", endpts::upstream_sink);
	auto result_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	result_socket.connect(endpts::upstream_sink);

	// Open communicator command socket to receive new communicator information
	log("commnode: connecting to communicator endpoint %s\n", endpts::communicator_cmd);
	auto communicator_command_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	communicator_command_socket.bind(endpts::communicator_cmd);

	// Store stream task sockets
	struct hash_Stream {
		size_t operator() (zmqnet::Stream const& stream) const {
			return std::hash<zmqnet::Stream::Id>{}(stream.id);
		}
	};
	struct compare_Stream {
		bool operator() (zmqnet::Stream const& lhs, zmqnet::Stream const& rhs) const {
			return lhs.id == rhs.id;
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
			assert(reader.getTag() == net::communicator_ids::AllNodes);

			// Relay to stream workers
			for (auto&& [stream, task_socket] : taskSockets) {
				log("commnode: relay exit message to stream %d in communicator %02x\n",
					stream.id, stream.communicatorId);

				// when using subsets, it's possible that the thread associated
				// with the task socket that we're looking at in this thread has 
				// already exited and trying to send it another exit would block 
				// forever. we use a poller_t to quickly check if it's still alive.

				auto poller = zmq::poller_t<>{};
				poller.add(task_socket, zmq::event_flags::pollout);
				// the endpoint is not a socket on another machine, it is another thread
				// right here in this process. if it's not ready in 10ms, it never will be.
				const auto timeout = std::chrono::milliseconds{10};
				std::vector<zmq::poller_t<>::event_type> pollout(1); // to hold results of poll

				if (poller.wait_all(pollout, timeout)) {
					// receiving end is ready
					net::send_MessageReader_copy(task_socket, reader);
				} else {
					// receiving end is not ready, possibly already gone
					log("commnode: WARNING: response_distributor exit message timed out. assuming stream already exited. \n");
				}
			}

			// Add to exited descendants
			{ auto const exited_nodes = response.getExit().getNodes().asBytes();
				exited_descendants.or_eq_buf(exited_nodes.begin(), exited_nodes.size());
			}

			// Forward upstream
			net::send_MessageReader(result_socket, std::move(reader));

			// Check if all descendants exited
			if (exited_descendants == descendants_bitset) {
				log("commnode: exiting (none remaining)\n");

				// All clients have exited, run cleanup
				break;
			}

			continue;

		// If ping, send upstream
		} else if (response.isPing()) {
			log("commnode: forwarding ping on communicator %d upstream\n", reader.getTag());
			net::send_MessageReader(result_socket, std::move(reader));

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
				auto const communicator_descendants = command.getCommunicator().getNodes().asBytes();
				in_progress_communicator_descendants = lrle_bitset
					{ communicator_descendants.begin()
					, communicator_descendants.size()
				};

				// Add to started descendants
				auto const started_communicator_descendants = response.getCommunicator().getNodes().asBytes();
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
				auto const communicator_descendants = response.getCommunicator().getNodes().asBytes();
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

				// Set node bitset
				communicatorAck.setNodes(kj::ArrayPtr<kj::byte const>
					{ (unsigned char const*)in_progress_communicator_descendants.compressed.data()
					, in_progress_communicator_descendants.compressed.size()
				});

				// Send acknowledgement
				log("commnode: sending communicator %02x create acknowledgement\n",
					in_progress_communicator_id);
				net::send_MessageBuilder(result_socket, net::communicator_ids::AllNodes, builder);

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

	log("commnode: telling main loop to exit.\n");
	{	auto main_loop_socket = zmq::socket_t{ctx, zmq::socket_type::push};
		main_loop_socket.connect(endpts::main_loop);
		
		auto builder = capnp::MallocMessageBuilder{};
		auto command = builder.initRoot<NetworkCommand>();
		command.setExit();

		net::send_MessageBuilder(main_loop_socket, net::communicator_ids::AllNodes, builder);
	}

	log("commnode: returning!\n");
}

} // namespace workers

namespace launch
{

static auto fcfs(zmq::context_t& ctx,
	zmqnet::Auth const& auth,
	net::UpstreamLink& upstream, net::TcpDownstreamLink& downstream,
	std::string const& dealerEndpoint, std::string const& dealerPublicKey)
{
	// Format externally-accessible endpoints
	auto const hostname = cti_be_getNodeHostname();
	auto const [controlEndpoint, dataEndpoint] = formatEndpoints(downstream, hostname);
	log("listening on %s / %s\n", controlEndpoint.c_str(), dataEndpoint.c_str());

	// Send endpoints to topology server and receive parent's endpoints and number of network children
	auto dealer_socket = zmq::socket_t{ctx, zmq::socket_type::req};

	// Authenticate and connect to topology server
	if (auth == zmqnet::Auth::Curve) {
		net::set_curve_client(dealer_socket, dealerPublicKey);
	}
	dealer_socket.connect(dealerEndpoint);
	if (startup_handshake_timeout() != -1) {
		dealer_socket.set(zmq::sockopt::rcvtimeo, startup_handshake_timeout() * 1000); // milliseconds
	}
	log("connected to topology server %s\n", dealerEndpoint.c_str());

	// Create topology request message
	{ auto builder = capnp::MallocMessageBuilder{};
		auto topologyRequest = builder.initRoot<TopologyRequest>();
		topologyRequest.setNodeHostname(hostname);
		topologyRequest.setNodeControlEndpoint(controlEndpoint);
		topologyRequest.setNodeDataEndpoint(dataEndpoint);
		topologyRequest.setNodePublicKey(downstream.getPublicKey());

		// Send this node's endpoints to topology server
		net::send_MessageBuilder(dealer_socket, net::communicator_ids::AllNodes, builder);
		log("registered endpoints with topology server\n");
	}

	// Receive this node's topology info from topology server
	auto maybeReader = net::recv_MessageReader_timeout(dealer_socket);
	if (!maybeReader) {
		// Should never happen in a normal launch. User might have aborted
		// the launch on the frontend.
		log("topology request timed out. Exiting.\n");
		std::exit(-1);
	}
	auto topologyResponse = maybeReader->getRoot<TopologyResponse>();
	log("topology server reported id %d parent %s / %s / %s, %d children\n",
		topologyResponse.getNodeId(),
		topologyResponse.getParentControlEndpoint().cStr(),
		topologyResponse.getParentDataEndpoint().cStr(),
		topologyResponse.getParentPublicKey().cStr(),
		topologyResponse.getNumChildren());

	// Apply topology information
	auto const node_id = topologyResponse.getNodeId();
	auto const num_children = topologyResponse.getNumChildren();

	// Connect to parent's endpoints
	auto const& parentControlEndpoint = topologyResponse.getParentControlEndpoint();
	auto const& parentDataEndpoint = topologyResponse.getParentDataEndpoint();
	auto const& parentPublicKey = topologyResponse.getParentPublicKey();
	log("connecting to parent %s / %s\n",
		parentControlEndpoint.cStr(), parentDataEndpoint.cStr());
	upstream.connect(parentControlEndpoint, parentDataEndpoint,
		auth, parentPublicKey);

	return std::make_tuple(node_id, num_children);
}

static void layered(zmq::context_t& ctx,
	zmqnet::Auth const& auth,
	net::UpstreamLink& upstream, net::TcpDownstreamLink& downstream,
	std::string const& parentControlEndpoint, std::string const& parentDataEndpoint, std::string const& parentPublicKey,
	int num_subchildren)
{
	// Format externally-accessible endpoints
	auto const hostname = cti_be_getNodeHostname();
	auto const [controlEndpoint, dataEndpoint] = formatEndpoints(downstream, hostname);
	log("listening on %s / %s\n", controlEndpoint.c_str(), dataEndpoint.c_str());

	// Connect to parent's endpoints
	log("connecting to parent %s / %s\n",
		parentControlEndpoint.c_str(), parentDataEndpoint.c_str());
	upstream.connect(parentControlEndpoint, parentDataEndpoint,
		auth, parentPublicKey);

	// Send endpoints to frontend
	{ auto builder = capnp::MallocMessageBuilder{};
		auto topologyRequest = builder.initRoot<TopologyRequest>();
		topologyRequest.setNodeHostname(hostname);
		topologyRequest.setNodeControlEndpoint(controlEndpoint);
		topologyRequest.setNodeDataEndpoint(dataEndpoint);

		// Send this node's endpoints to topology server
		net::send_MessageBuilder(upstream.data_socket, net::communicator_ids::AllNodes, builder);
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
	auto [authFile, authType,
		dealerEndpoint, nodeConnectionMap,
		packed_mode, packed_node_id,
		leaf_argc, leaf_argv] = parse_argv(argc, argv);

	// Start network connection process
	auto upstream = net::UpstreamLink{ctx, authType};
	net::subscribe_to_tag(upstream.control_socket, net::communicator_ids::AllNodes);
	auto downstream = net::TcpDownstreamLink{ctx, authType};
	auto num_children = int{};
	auto node_id = int{};

	auto const first_pe = (packed_node_id < 0)
		? cti_be_getNodeFirstPE()
		: packed_node_id;

	if (packed_mode) {
		enable_individual_logfile("zmqnet_leaf_commnode_" + std::to_string(first_pe) + ".log");
	}

	// Use either FCFS or leveled tree launch based on arguments
	if (!dealerEndpoint.empty()) {

		// Open and read authentication details if CURVE enabled
		auto dealerPublicKey = std::string{};
		if (!authFile.empty() && (authType == zmqnet::Auth::Curve)) {
			auto authStream = std::ifstream{authFile.c_str()};
			if (!std::getline(authStream, dealerPublicKey)) {
				throw std::runtime_error("failed to read dealer public key from auth file at " + authFile);
			}

			log("read CURVE key %s\n", dealerPublicKey.c_str());
		}

		// Run FCFS launch procedure
		log("launching FCFS %s\n", dealerPublicKey.c_str());
		std::tie(node_id, num_children) = launch::fcfs(ctx, authType,
			upstream, downstream,
			dealerEndpoint, dealerPublicKey);

	} else {
		// Connect using leveled tree launch
		auto [node_id_,
			parentControlEndpoint, parentDataEndpoint, parentPublicKey,
			num_children_, num_subchildren] = parseNodeConnectionMap(std::move(nodeConnectionMap));
		node_id = node_id_;
		num_children = num_children_;

		// Determine if launch should continue
		if (parentControlEndpoint.empty()) {
			return 0;
		}

		// Run layered launch procedure
		log("launching leveled tree with %d children\n", num_children);
		launch::layered(ctx, authType, upstream, downstream,
			parentControlEndpoint, parentDataEndpoint,
			parentPublicKey, num_subchildren);
	}
	log("%d network children\n", num_children);

	auto childPids = ThreadSafeSet<CommChild>{};

	// Notifies the signal handler thread to stop
	std::atomic<bool> exitFlag = false;

	// Block all signals, this will carry over to all worker threads
	blockAllSignals();

	// Start signal handler thread
	auto signalHandler = std::async(std::launch::async, workers::handle_all_signals,
		std::ref(ctx), std::ref(exitFlag), std::ref(childPids)
	);

	// Determine number of local children, one per PE on node
	auto const local_pes = 1;
	num_children += local_pes;

	// If running packed mode and no node was provided, start other packed commnodes
	if (packed_mode && (packed_node_id < 0)) {
		auto const packed_commnodes = cti_be_getNodePEs() - 1;
		log("forking %d local packed commnodes\n", packed_commnodes);

		// Set up commnode daemon arguments with topology connection info and leaf process arguments
		auto commnodeArgv = cti::OutgoingArgv<CommnodeArgv>{argv[0]};
		commnodeArgv.add(CommnodeArgv::DealerEndpoint, dealerEndpoint);
		commnodeArgv.add(CommnodeArgv::AuthFile, authFile);
		commnodeArgv.add(CommnodeArgv::AuthType, std::to_string(static_cast<int>(authType)));
		commnodeArgv.add(CommnodeArgv::Packed);
		commnodeArgv.add(cti::Argv::Argument{"-n"});
		commnodeArgv.add(cti::Argv::Argument{"-1"}); // Add seperately from option so it can be replaced via pointer

		// Add provided leaf arguments
		for (int i = 0; i < leaf_argc; i++) {
			commnodeArgv.add(cti::Argv::Argument{leaf_argv[i]});
		}

		// Get pointer to node argument
		auto commnode_argv = commnodeArgv.get();
		auto node_arg_ptr = (char**)&commnode_argv[6]; // should point to "-1"

		// Launch packed commnodes
		for (int i = 0; i < packed_commnodes; i++) {
			log("%d / %d\n", i + 1, packed_commnodes);

			if (auto const forkedPid = fork()) {
				// Add launched process to cleanup list
				const auto CommnodeT = CommChild::Type::Commnode;
				childPids.insert(CommChild(CommnodeT, forkedPid, -1));

			} else {
				// Set node argument
				auto const node = first_pe + local_pes + i;
				auto const nodeString = std::to_string(node);
				*node_arg_ptr = (char*)nodeString.c_str();

				// Exec commnode binary
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

		// Prepend pubkey with '~' to prevent keys with leading '-' from
		// interfering with the leaf program's argument parsing. '~' is not used
		// in the Z85 encoding scheme that the key is currently encoded as.
		auto pubkey = std::string{"~"} + downstream.getPublicKey();

		// Prepend endpoints and node number to provided leaf arguments
		auto completeLeafArgv = cti::ManagedArgv
			{ leaf_argv[0]
			, std::to_string((int)authType)
			, controlEndpoint
			, dataEndpoint
			, pubkey
			, ""
		};

		// Copy provided arguments
		for (int i = 0; i < leaf_argc; i++) {
			completeLeafArgv.add(leaf_argv[i]);
		}

		// Get pointer to node argument
		auto complete_leaf_argv = completeLeafArgv.get();
		auto node_arg_ptr = &complete_leaf_argv[5];

		for (int node = first_pe; node < first_pe + local_pes; node++) {
			log("%d / %d\n", node - first_pe + 1, local_pes);

			if (auto const forkedPid = fork()) {
				// Add launched process to cleanup list
				const auto LeafT = CommChild::Type::Leaf;
				childPids.insert({LeafT, forkedPid, node});

			} else {
				// Set node argument
				auto const nodeString = std::to_string(node);
				*node_arg_ptr = (char*)nodeString.c_str();

				// Exec leaf binary
				execvp(leafPath.c_str(), complete_leaf_argv);
				perror("execvp");
				_exit(-1);
			}
		}
	}

	// Wait for children to report ready and initialize the descendants map
	auto descendants = [&](){
		try {
			return Descendants(downstream.data_socket, num_children, startup_handshake_timeout());
		} catch (std::runtime_error& e) {
			// children never reported ready after startup_handshake_timeout() seconds

			// kill any children that might be alive
			auto builder = capnp::MallocMessageBuilder{};
			auto killSignal = builder.initRoot<NetworkCommand>();
			killSignal.setKill();

			net::send_MessageBuilder(downstream.control_socket, net::communicator_ids::AllNodes, builder);

			std::exit(-1);
		}
	}();

	// Send ready to parent
	log("sending ready to parent\n");
	{ auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		auto ready = response.initReady();

		// Set node bitset
		ready.setNodes(kj::ArrayPtr<kj::byte const>
			{ (unsigned char const*)descendants.getAllSet().compressed.data()
			, descendants.getAllSet().compressed.size()
		});

		net::send_MessageBuilder(upstream.data_socket, net::communicator_ids::AllNodes, builder);
	}

	// Create proxy threads to receive/deliver data from outside of the commnode.
	// Proxy threads are created so that main/worker threads can receive input
	// from multiple locations.

	// Commnode input proxy; owns upstream control socket. All data from upstream
	// passes through here and is sent to endpts::main.
	auto upstreamSource = std::async(std::launch::async, workers::upstream_source,
		std::ref(ctx), std::move(upstream.control_socket), std::ref(descendants)
	);

	// Commnode result proxy; owns upstream data socket. To send data upstream, all threads send data here.
	auto upstreamSink = std::async(std::launch::async, async::collect_network_responses,
		std::ref(ctx), workers::endpts::upstream_sink, std::move(upstream.data_socket)
	);

	// Child result proxy; owns downstream data socket. All data from downstream
	// children (commnodes, leaves) passes through here and is sent to endpts::response_distributor
	auto downstreamSource = std::async(std::launch::async,
		[](zmq::context_t& ctx, zmq::socket_t&& input_socket, const char* output_endpoint) {
			auto output_socket = zmq::socket_t{ctx, zmq::socket_type::push};
			output_socket.connect(output_endpoint);

			// blocks until ctx is closed, at which point ETERM will be thrown
			try {
				zmq::proxy(input_socket, output_socket);
			} catch (zmq::error_t& e) {
				if (e.num() != ETERM) throw;
				log("downstream_source: got ETERM, exiting.\n");
			}
			log("downstream_source: returning!\n");
			return std::move(input_socket);
		},
		std::ref(ctx), std::move(downstream.data_socket), workers::endpts::response_distributor
	);

	// No downstreamSink proxy thread is made because only the main thread sends commands downstream.

	// Receive incoming stream data messages and distribute to respective stream task
	auto responseDistributor = std::async(std::launch::async, workers::response_distributor,
		std::ref(ctx), descendants.getAllSet()
	);

	// Connect for communicator create commands
	auto communicator_command_socket = zmq::socket_t{ctx, zmq::socket_type::push};
	communicator_command_socket.connect(workers::endpts::communicator_cmd);

	auto upstream_command_socket = zmq::socket_t{ctx, zmq::socket_type::pull};
	upstream_command_socket.bind(workers::endpts::main_loop);

	// Command loop
	auto streamAggregatorTasks = std::vector<std::thread>{};
	auto const filterBasePath = std::string{cti_be_getLibDir()};

	while (true) {

		// Receive next command from parent
		log("waiting for command from upstream\n");
		auto reader = net::recv_MessageReader(upstream_command_socket);
		auto command = reader.getRoot<NetworkCommand>();

		if (command.isPing()) {
			log("got ping on communicator %d\n", reader.getTag());

			// Relay command to children
			net::send_MessageReader(downstream.control_socket, std::move(reader));

		// Determine if exit command was received
		} else if (command.isExit()) {
			log("got exit command\n");

			// Stop signal handling thread
			exitFlag = true;
			log("waiting for signal handler thread\n");
			signalHandler.get();

			unblockAllSignals();

			// Relay command to children
			net::send_MessageReader(downstream.control_socket, std::move(reader));

			break;

		// Last ditch kill signal
		} else if (command.isKill()) {
			log("got kill command\n");

			// Relay command to children
			net::send_MessageReader(downstream.control_socket, std::move(reader));

			// ensure that all pending outgoing messages get sent
			ctx.close();

			std::exit(-1);

		} else if (command.isStartLeaves()) {
			log("unexpected: startLeaves\n");
			continue;

		// Determine if new communicator is to be registered and subscribed
		} else if (command.isCommunicator()) {
			// Relay to response thread
			net::send_MessageReader_copy(communicator_command_socket, reader);

			// Relay to children. TODO: avoid builder copy
			net::send_MessageReader(downstream.control_socket, std::move(reader));

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
			log("got filter: %s::%s\n", std::string{filterLibraryName}.c_str(), std::string{filterFunctionName}.c_str());

			// Reconstitute stream sync setting
			auto streamSync = zmqnet::StreamSync{};
			if (newStreamCommand.getSync().isImmediate()) {
				streamSync = zmqnet::sync::Immediate{};
			} else if (newStreamCommand.getSync().isWaitForAll()) {
				streamSync = zmqnet::sync::WaitForAll{};
			} else if (newStreamCommand.getSync().isTimeout()) {
				streamSync = zmqnet::sync::Timeout{ .ms = newStreamCommand.getSync().getTimeout() };
			} else {
				assert(false);
			}

			// Create new stream task worker
			streamAggregatorTasks.emplace_back(
				[](zmq::context_t& ctx, std::string resultEndpoint,
					zmqnet::Stream stream, std::string filterPath, std::string filterFunctionName,
					lrle_bitset descendants_bitset, zmqnet::StreamSync streamSync) {

					(void)workers::stream_aggregator(ctx, std::move(resultEndpoint),
						stream, std::move(filterPath), std::move(filterFunctionName),
						std::move(descendants_bitset), std::move(streamSync));

				}, std::ref(ctx), workers::endpts::upstream_sink,
					stream, filterBasePath + "/" + std::string{filterLibraryName}, std::string{filterFunctionName},
					descendants.at(stream.communicatorId), streamSync);

			// Relay to children
			net::send_MessageReader(downstream.control_socket, std::move(reader));

		} else {
			log("got user command on stream %d communicator %02x\n",
				reader.getTag(), command.getUser().getStreamId());

			// Relay command to children
			net::send_MessageReader(downstream.control_socket, std::move(reader));
		}
	}

	rc = 0;

	// Wait on stream relay thread
	log("waiting for result relay thread\n");
	responseDistributor.get();

	// Wait on stream worker threads
	log("waiting for stream worker threads\n");
	for (auto&& stream_aggregator_task : streamAggregatorTasks) {
		stream_aggregator_task.join();
	}

	// Construct exit message for proxy
	{	auto const resultEndpoint = workers::endpts::upstream_sink;
		auto result_socket = zmq::socket_t{ctx, zmq::socket_type::push};
		result_socket.connect(resultEndpoint);

		auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		auto exit = response.initExit();
		exit.setNodeType(NodeType::UNSET); // UNSET to shut down result proxy

		// Send exit message to proxy
		net::send_MessageBuilder(result_socket, net::communicator_ids::AllNodes, builder);
	}

	// Get upstream data socket from exiting upstream sink
	log("waiting for upstream sink\n");
	upstream.data_socket = upstreamSink.get();

	// Clean up child pids
	childPids.forEach([&rc](auto& child) {
		const auto Leaf = CommChild::Type::Leaf;
		log("waiting for child pid: %d (%s)\n", child.pid, child.type == Leaf ? "leaf" : "commnode");

		// Wait for exit
		int status;
		if (waitpid(child.pid, &status, 0) < 0) {
			log("waitpid %d failed: %s\n", child.pid, strerror(errno));
			rc = -1;
		} else if (WIFEXITED(status) && WEXITSTATUS(status)) {
			log("pid %d failed with error code %d\n", child.pid, WEXITSTATUS(status));
			rc = -1;
		}
	});

	// Notify parent that cleanup has completed
	{ auto builder = capnp::MallocMessageBuilder{};
		auto response = builder.initRoot<NetworkResponse>();
		auto exitAck = response.initExit();

		// Set exit info
		exitAck.setNodeType(NodeType::COMMNODE);
		auto const& all_descendants = descendants.getAllSet();
		exitAck.setNodes(kj::ArrayPtr<kj::byte const>
			{ (unsigned char const*)all_descendants.compressed.data()
			, all_descendants.compressed.size()
		});

		log("notifying parent of exit (%s)\n", all_descendants.to_string().c_str());
		net::send_MessageBuilder(upstream.data_socket, net::communicator_ids::AllNodes, builder);
	}

	// Close all network sockets
	log("context shutdown\n");
	ctx.shutdown();

	// ctx.shutdown() will trigger exits
	upstream.control_socket = upstreamSource.get();
	downstream.data_socket = downstreamSource.get();

	upstream.control_socket.set(zmq::sockopt::linger, 0);

	log("commnode cleaned up and exiting\n");
	fprintf(stderr, "%05lu: exiting\n", get_milliseconds() - start_ms);

	return rc;
}
