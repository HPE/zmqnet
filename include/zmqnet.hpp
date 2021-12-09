/*
 * Copyright 2011-2020 Hewlett Packard Enterprise Development LP.
 */

#pragma once

#include <uuid/uuid.h>

#include <vector>
#include <string>
#include <unordered_map>
#include <set>
#include <mutex>
#include <variant>

#include <future>
#include <memory>

#include <zmq.hpp>

#include <kj/array.h>

// Forward-declares
namespace net
{
	struct shared_semaphore;

	class TcpDownstreamLink;
	class UpstreamLink;
}

namespace zmqnet
{

// Command data is provided via ByteArray
// Extract bytes with .begin() and size with .size()
using ByteArray = kj::Array<kj::byte>;

// Response data is provided via ByteArrayPtr
// Extract bytes with .begin() and size with .size()
using ByteArrayPtr = kj::ArrayPtr<kj::byte const>;

// Filters are passed an array of ByteArrays as inputs, and return a ByteArray as a result
// If no output is desired, return an empty zmqnet::ByteArray{}
using FilterType = zmqnet::ByteArray(std::vector<zmqnet::ByteArray> const&);

// In asynchronous callbacks or filter functions, return a ByteArray to produce a message result
static inline auto make_ByteArray(char const* buffer, size_t buffer_len)
{
	return kj::heapArray((unsigned char const*)buffer, buffer_len);
}
static inline auto make_ByteArray(unsigned char const* buffer, size_t buffer_len)
{
	return kj::heapArray(buffer, buffer_len);
}

// When constructing a Frontend object, provide a topology structure to build the network
struct Topology
{
	// Launch strategy
	enum Launch : int
		{ Fcfs    // First-come first-served. Single launch invocation
		, Layered // Statically assign topology. Launch the network layer-by-layer
		, Packed  // Simulate running at higher scale by launching a commnode for each PE
	};
	Launch launch;

	// Communication tree fanout. Each non-leaf commnode will have `fanout` children
	int fanout;
};

// Communicators are subsets of job nodes to which commands can be targeted. Can include all nodes.
using NodeSet = std::set<std::string>;
using RankSet = std::set<size_t>;
struct Communicator
{
	using Id = int;
	Id id;
};

// Streams are owned by Communicators, and run user-provided filtering code to transform results.
struct Stream
{
	using Id = int;
	Id communicatorId;
	Id id;
};

// Message aggregation can be synchronized, or subject to a timeout
namespace sync
{

// Do not wait, apply user filter function to message as soon as it is received
struct Immediate {};

// Wait for all child nodes in a stream to send a message before applying the filter function
struct WaitForAll {};

// Wait for the given number of milliseconds to elapse, or for all messages to be received
// before applying the filter function
struct Timeout
{
	int ms;
};

} // sync

using StreamSync = std::variant<sync::Immediate, sync::WaitForAll, sync::Timeout>;

// Internal base Frontend and Backend objects
namespace detail
{

class Frontend
{
protected: // members
	int m_max_node_id;
	std::unordered_map<std::string, int> m_nodeIdMap;
	zmq::context_t m_ctx;
	std::unique_ptr<net::TcpDownstreamLink> m_downstream;
	Communicator::Id m_next_communicatorId;

protected: // derived class interface
	Frontend(int const app_id, long int const session_id, long int const manifest_id,
		Topology const& topology, char const* const* leaf_argv);
	~Frontend();

	Communicator createCommunicator(NodeSet const& nodes);
	Communicator createCommunicator(const RankSet &ranks);
	Stream createStream(Communicator const& communicator, Stream::Id const& streamId,
		std::string const& filterLibraryName, std::string const& filterFunctionName,
		zmqnet::StreamSync const& streamSync);

	void sendCommand(Stream const& stream, char const* command_buffer, size_t command_len);
	void sendExit();
};

class Backend
{
protected: // members
	zmq::context_t m_ctx;
	std::unique_ptr<net::UpstreamLink> m_upstream;
	int m_node_id;
	std::vector<unsigned char> m_node_bitset;

protected: // derived class interface
	template <typename CharPtr>
	static auto getParams(int argc, CharPtr argv)
	{
		if (argc < 5) {
			throw std::runtime_error("expected 4 network param arguments");
		}
		return std::make_tuple(
			std::string{argv[1]}, // Upstream control endpoint
			std::string{argv[2]}, // Upstream data endpoint
			std::stoi(argv[3]),   // Node ID
			argc - 4,             // Leaf binary argc
			argv + 4);            // Leaf binary argv
	}

	Backend(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int node_id);
	~Backend();

	void notifyReady();

	void sendResult(Stream const& stream, char const* result_buffer, size_t result_len);
};

class CallbackProcessorManager {
	// Instances of AsyncBackend use a CallbackProcessorManager to launch
	// callback processor threads (see workers::callback_processor in Backend.cpp).
	// Launching a callback processor requires two things: AsyncBackend::registerCallback to be called by 
	// the user on the server side, and AsyncFrontend::registerStream to be called by the user
	// on the client side.
	// The CallbackProcessorManager makes sure both of these things happpen,
	// specifically in the order given, before actually spawning a new thread.
private:
	// same as AsyncBackend::CallbackFunc
	using CallbackFunc = std::function<ByteArray(Stream::Id const&, ByteArray)>;

public:
	CallbackProcessorManager(zmq::context_t &ctx, const std::string &collectorEndpoint, 
		const int &nodeId, const std::vector<unsigned char> &nodeBitset);

	// Called by a user's call to AsyncBackend::registerCallback. Records 
	// callbackFunc to be used on stream streamId when the stream is created.
	void registerCallback(Stream::Id const& streamId, CallbackFunc callbackFunc);

	// Called by the AsyncBackend's incoming command thread. Actually launches
	// the thread stored with the given id. If the id doesn't exist, an exception
	// is thrown.
	void launchCallbackProcessor(const Stream::Id &id);

	// Wait for all running callback processors to exit. It is assumed that the
	// running threads have already received an exit command over the network.
	void waitForExit();

	// Returns true if there is a running thread associated with the given id.
	bool streamIsActive(const Stream::Id &id) const;

private:
	// Given an id, returns true if either a waiting callback function has been
	// registered but not launched, or if there is a running thread associated
	// with the given id.
	bool callbackProcessorExists(const Stream::Id &id);

	// Functions that have been registered but not launched
	std::unordered_map<Stream::Id, CallbackFunc> m_waitingCallbackProcs;
	// Launched threads
	std::unordered_map<Stream::Id, std::future<void>> m_runningCallbackProcs;
	mutable std::mutex m_callbackLock;

	// References to data in AsyncBackend needed to launch a new thread
	zmq::context_t &m_ctx;
	const std::string &m_collectorEndpoint;
	const int m_nodeId;
	const std::vector<unsigned char> &m_nodeBitset;
};

} // detail

// Synchronous Frontends send a command and receive results synchronously.
// This is useful for non-interactive programs that do not need to issue commands in response to
// user input.
class SyncFrontend : private detail::Frontend
{
public: // interface

	// Frontend construction launches the network and connects the communication tree.
	// The Frontend constructor is used to build a communication tree on a running application.
	// Any filter libraries required by the application should be added to the CTI Manifest
	// specified by `manifest_id` before construction.
	// `leaf_argv` are any additional arguments to be passed to the user's leaf binary,
	// with `leaf_argv[0]` being the path to the leaf binary. This is the binary that will
	// instantiate the Backend object to connect to the network and respond to network commands.
	SyncFrontend(int const app_id, long int const session_id, long int const manifest_id,
		Topology const& topology, char const* const* leaf_argv);

	// Get the default communicator to send a message to all nodes
	Communicator getAllNodesCommunicator() const;

	// Create a new communicator targeting the nodes with hostnames specified in `nodes`
	Communicator registerCommunicator(NodeSet const& nodes);
	// Create a new communicator targeting the nodes with ranks specified in `ranks`
	Communicator registerCommunicator(const RankSet &ranks);

	// Create a new stream in the provided communicator. Stream IDs are determined by the user.
	// Provide a filter library and filter function name.
	Stream registerStream(Communicator const& communicator, Stream::Id const& streamId,
		std::string const& filterLibraryName, std::string const& filterFunctionName,
		StreamSync const& streamSync);

	// Once a Stream is created, send commands by providing a buffer of command data.
	// Results from the Backend instances are aggregated according to the filter function
	// specified during construction of the Stream, and returned to the caller in a ByteArray
	// object.
	ByteArray runCommand(Stream const& stream, char const* command_buffer, size_t command_len);
	ByteArray runCommand(Stream const& stream, std::string const& command);

	// Notify all Backend instances that the network should exit. Returns once all Backend instances
	// have exited.
	void runExit();
};

// Asynchronous Frontend register callbacks for each handled Stream that will receive
// the contents of every incoming message for that stream.
class AsyncFrontend : private detail::Frontend
{
public: // types

	// Command callbacks take a pair of Stream ID and ByteArray, where the Stream ID corresponds
	// to the stream on which the incoming command is received, and the ByteArray is the raw
	// contents of the command.
	using CallbackFunc = std::function<void(Stream::Id const&, ByteArrayPtr)>;

private: // members
	zmq::context_t m_ctx;
	std::unordered_map<Stream::Id, std::future<void>> m_callbackProcessors;
	std::unique_ptr<net::shared_semaphore> m_blocking_registration;
	std::future<zmq::socket_t> m_responseDistributor;

public: // interface

	// See zmqnet::SyncFrontend for description
	AsyncFrontend(int const app_id, long int const session_id, long int const manifest_id,
		Topology const& topology, char const* const* leaf_argv);
	~AsyncFrontend();

	// Get the default communicator to send a message to all nodes
	Communicator getAllNodesCommunicator() const;

	// Create a new communicator targeting the nodes with hostnames specified in `nodes`
	Communicator registerCommunicator(NodeSet const& nodes);
	// Create a new communicator targeting the nodes with ranks specified in `ranks`
	Communicator registerCommunicator(const RankSet &ranks);

	// Create a new callback processor for the given stream ID. After the callback is registered,
	// the actual Stream can be registered with the network
	void registerStreamCallback(Stream::Id const& streamId, CallbackFunc callbackFunc);

	// Create a new stream in the provided communicator. Stream IDs are determined by the user.
	// Provide a filter library and filter function name.
	Stream registerStream(Communicator const& communicator, Stream::Id const& streamId,
		std::string const& filterLibraryName, std::string const& filterFunctionName,
		zmqnet::StreamSync const& streamSync);

	// Once a Stream is created, send send commands by providing a buffer of command data.
	// Results from the Backend instances are aggregated according to the filter function
	// specified during construction of the Stream, and passed to the Stream callback registered
	// previously.
	void sendCommand(Stream const& stream, char const* command_buffer, size_t command_len);
	void sendCommand(Stream const& stream, std::string const& command);

	// Notify all Backend instances that the network should exit. Returns once all Backend instances
	// have exited.
	void runExit();
};

// Backend nodes are automatically launched by intermediate commnode instances during network
// startup.
// Synchronous backends follow a simple command loop, where the next command is received blocking
// and returned to the caller with the referenced Stream object, and the ByteArray contents of the
// Command.
class SyncBackend : private detail::Backend
{
public: // interface

	// Network connection arguments are prepended to the argument array for the user-provided
	// leaf binary. Extract these arguments using `getParams` to obtain the following:
	// Upstream control endpoint
	// Upstream data endpoint
	// Node ID
	// Remaining argument count and arguments provided by the user during Frontend construction

	// Example usage:
	// auto const [controlEndpoint, dataEndpoint, node_id, rest_argc, rest_argv]
	//     = zmqnet::SyncBackend::getParams(argc, argv);
	// auto backend = zmqnet::SyncBackend(controlEndpoint, dataEndpoint, node_id);
	template <typename CharPtr>
	static auto getParams(int argc, CharPtr argv) { return detail::Backend::getParams(argc, argv); }

	// Connect to the communication tree using network parameters extracted by
	// zmqnet::Backend::getParams(argc, argv) where argc and argv are the actual arguments provided
	// to the user binary calling SyncBackend.
	SyncBackend(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int node_id);

	// Block and wait for the next incoming command. The Stream on which this command was received
	// is provided, containing the user-provided Stream ID that was used when creating the Stream
	// on the Frontend object.
	// When an exit command is received, the result will have a Stream with an ID of -1. The leaf
	// binary is expected to exit in response to this command, after any necessary cleanup.
	// The Backend object destructor will send the exit confirmation up the communication tree.
	std::tuple<Stream, ByteArray> nextCommand();

	// Send result data up the communication tree on the given Stream to be aggregated by the
	// Stream's filter function.
	void sendResult(Stream const& stream, char const* result_buffer, size_t result_len);
};

// Asynchronous Backends register callbacks for each handled Stream that will be provided with
// the contents of every incoming message for that stream.
// The main thread of the process running an asynchronous Backend is expected to call the blocking
// function `waitForExit` that returns when an exit command is received.
class AsyncBackend : private detail::Backend
{
public: // types

	// Command callbacks take a pair of Stream ID and ByteArray, where the Stream ID corresponds
	// to the stream on which the incoming command is received, and the ByteArray is the raw
	// contents of the command.
	// The callback result is a new ByteArray that is then sent as a message to the parent node.
	// If the result array is empty, then no message is sent. This can be accomplished by returning
	// a default-constructed zmqnet::ByteArray{}
	using CallbackFunc = std::function<ByteArray(Stream::Id const&, ByteArray)>;

private: // members
	zmq::context_t m_ctx;

	std::string m_collectorEndpoint;
	zmq::socket_t m_collectorSocket;
	std::future<zmq::socket_t> m_responseCollector;
	std::future<zmq::socket_t> m_commandDistributor;
	detail::CallbackProcessorManager m_callbackProcManager;
	
public: // interface

	// Network connection arguments are prepended to the argument array for the user-provided
	// leaf binary. Extract these arguments using `getParams` to obtain the following:
	// Upstream control endpoint
	// Upstream data endpoint
	// Node ID
	// Remaining argument count and arguments provided by the user during Frontend construction

	// Example usage:
	// auto const [controlEndpoint, dataEndpoint, node_id, rest_argc, rest_argv]
	//     = zmqnet::SyncBackend::getParams(argc, argv);
	// auto backend = zmqnet::SyncBackend(controlEndpoint, dataEndpoint, node_id);
	template <typename CharPtr>
	static auto getParams(int argc, CharPtr argv) { return detail::Backend::getParams(argc, argv); }

	// The constructor takes the same network parameters as the synchronous variant, as extracted
	// from the leaf binary arguments by zmqnet::Backend::getParams.
	AsyncBackend(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int node_id);
	~AsyncBackend();

	// Before initialization of the async. backend can complete, callbacks must be registered for
	// all possible incoming stream IDs.
	void registerCallback(Stream::Id const& streamId, CallbackFunc callbackFunc);

	// After all callbacks are registered, send a "setup complete" message with `notifyReady`.
	void notifyReady();

	// Directly send 'result' to the front end using the stream 'streamId'.
	// The data is filtered through the registered filter functions and
	// passed to the client side callback defined in
	// 'AsyncFrontend::registerStreamCallback'.
	void sendResult(const Stream::Id &streamId, zmqnet::ByteArray &&result);

	// Blocks and waits for an incoming exit message to be received.
	void waitForExit();
};

} // zmqnet
