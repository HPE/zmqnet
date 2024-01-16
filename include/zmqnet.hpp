/*
 * Copyright 2011-2020 Hewlett Packard Enterprise Development LP.
 */

#pragma once

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

class lrle_bitset;

namespace zmqnet
{

namespace env
{
	// Where zmqnet will look for zmqnet related libraries and binaries.
	// e.g. where zmqnet finds its commnode binary.
	constexpr auto INSTALL_DIR = "LIBZMQNET_INSTALL_DIR";

	// If set to a number other than "0", explicitly enables encryption.
	constexpr auto CURVE = "LIBZMQNET_CURVE";

	// If set, enables debug behavior (e.g. logging, disabling encryption).
	constexpr auto ENABLEDEBUG = "CTI_DEBUG";

	// If set to a valid path and DEBUG is enabled,
	// zmqnet will write debug log files to the directory.
	constexpr auto LOG_DIR = "CTI_LOG_DIR";

	// During zmqnet network startup, there are a number of "handshakes"
	// that need to be done between the frontend, the commnodes, and the backends.
	// This variable is the amount of seconds that a process (frontend, commnode, or backend)
	// will wait for the other side of any given startup handshake before quitting.
	// If set to -1, the startup handshake timeout will be disabled.
	// If not set, the default value is 300 seconds.
	constexpr auto STARTUP_TIMEOUT = "LIBZMQNET_STARTUP_TIMEOUT";

	// If set to a positive integer, zmqnet commnodes will sleep for n seconds when they start.
	constexpr auto DEBUG_COMMNODE_STARTUP_SLEEP = "LIBZMQNET_DEBUG_COMMNODE_STARTUP_SLEEP";

	// If set, use this as the port number base for allocating ports
	constexpr auto PORT_BASE = "LIBZMQNET_PORT_BASE";

	// Maximum port offsets to try (default 10)
	constexpr auto PORT_MAX = "LIBZMQNET_PORT_MAX";
} // namespace env

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

// Type of encryption / logging
enum class Auth : int
	{ None // No encryption or authentication
	, Curve // Use ZMQ CURVE key-based authentication and encryption
	, Log // Encryption is disabled and logging is enabled
};

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

// Events that a callback can be hooked to
enum class Event : size_t {
	                   // called upon the occurance of...
	InitialConnection, // each initial connection during network construction
	LeafExit           // an unexpected exit of a zmqnet leaf program
};

class UserCallbacks
{
private:
	template<typename> constexpr static bool alwaysFalse = false;
public:
	UserCallbacks() = default;
	UserCallbacks(UserCallbacks&&) = default;
	UserCallbacks(const UserCallbacks&) = delete;

	template <Event e, typename F>
	void registerCallback(F fn) {
		if constexpr (e == Event::InitialConnection) {
			m_uponInitialConnect = fn;
		} else if constexpr (e == Event::LeafExit) {
			m_uponLeafExit = fn;
		} else {
			static_assert(UserCallbacks::alwaysFalse<e>, "Invalid event");
		}
	};

	template <Event e, typename ...Args>
	void invokeCallback(Args&&... args) {
		if constexpr (e == Event::InitialConnection) {
			if (m_uponInitialConnect)
				m_uponInitialConnect(std::forward<Args>(args)...);
		} else if constexpr (e == Event::LeafExit) {
			if (m_uponLeafExit)
				m_uponLeafExit(std::forward<Args>(args)...);
		} else {
			static_assert(UserCallbacks::alwaysFalse<e>, "Invalid event");
		}
	}

	template <Event e>
	void removeCallback() {
		if constexpr (e == Event::InitialConnection) {
			m_uponInitialConnect = nullptr;
		} else if constexpr (e == Event::LeafExit) {
			m_uponLeafExit = nullptr;
		} else {
			static_assert(UserCallbacks::alwaysFalse<e>, "Invalid event");
		}
	}
private:
	std::function<void(int)> m_uponInitialConnect;
	std::function<void(int)> m_uponLeafExit;
};

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
	UserCallbacks m_uCallbacks;
	std::unique_ptr<lrle_bitset> m_allDescendants;

protected: // derived class interface
	Frontend(int const app_id, long int const session_id, long int const manifest_id,
		Topology const& topology, char const* const* leaf_argv, char const* const* leaf_env,
		UserCallbacks&& callbacks, const std::atomic<bool>* killSwitch
	);
	~Frontend();

	Communicator getAllNodesCommunicator() const;

	// Derived classes specialize these low-level network operations
	Communicator createCommunicator(zmq::socket_t& sender, NodeSet const& nodes);
	Communicator createCommunicator(zmq::socket_t& sender, const RankSet &ranks);
	static Stream createStream(zmq::socket_t& sender,
		Communicator const& communicator, Stream::Id const& streamId,
		std::string const& filterLibraryName, std::string const& filterFunctionName,
		zmqnet::StreamSync const& streamSync);

	static void sendCommand(
		zmq::socket_t& sender, Stream const& stream, char const* command_buffer, size_t command_len);
	static void sendExit(zmq::socket_t& sender);
	static RankSet sendPing(Communicator const& comm, zmq::socket_t& sender, zmq::socket_t& listener, std::string payload, int timeoutMs, RankSet const& stopAfter);
	static bool confirmPing(Communicator const& comm, RankSet const& ranks, zmq::socket_t& sender, zmq::socket_t& listener, std::string payload);

	template<Event e, typename FnT>
	void registerCallback(FnT&& CBFn) { m_uCallbacks.registerCallback<e>(std::forward<FnT>(CBFn)); }
	template<Event e>
	void removeCallback() { m_uCallbacks.removeCallback<e>(); }

	RankSet toRankSet(NodeSet const& nodes);
};

class Backend
{
public: // types
	struct BackendParams
	{
		Auth authType;
		std::string upstreamControlEndpoint, upstreamDataEndpoint, upstreamPublicKey;
		int node_id, rank_id;
		pid_t target_pid;
	};

protected: // members
	zmq::context_t m_ctx;
	std::unique_ptr<net::UpstreamLink> m_upstream;
	int m_node_id, m_rank_id;
	pid_t m_target_pid;
	std::vector<unsigned char> m_node_bitset;

protected: // derived class interface
	template <typename CharPtr>
	static auto getParams(int argc, CharPtr argv)
	{
		if (argc < 7) {
			throw std::runtime_error("expected at least 7 network param arguments");
		}

		// if the calling program was launched by a zmqnet commnode, the public
		// key has '~' prepended to prevent argument parsing issues
		auto upstreamPubkey = std::string{argv[4] + (argv[4][0] == '~' ? 1 : 0)};

		return std::make_tuple(
			BackendParams
				{ .authType = (zmqnet::Auth)std::stoi(argv[1])
				, .upstreamControlEndpoint = argv[2]
				, .upstreamDataEndpoint = argv[3]
				, .upstreamPublicKey = upstreamPubkey
				, .node_id = std::stoi(argv[5])
				, .rank_id = std::stoi(argv[6])
				, .target_pid = std::stoi(argv[7])
			},
			argc - 8, // Rest of arguments count
			argv + 8); // Rest of arguments array
	}

	Backend(BackendParams const& backendParams);
	~Backend();

	void notifyReady();

	void sendResult(Stream const& stream, char const* result_buffer, size_t result_len);

	int getRank() const;
	pid_t getTargetPid() const;
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
	// Additional environment variables can be added to the leaf binary launch by
	// providing an optional leaf_env argument.
	SyncFrontend(int const app_id, long int const session_id, long int const manifest_id,
		Topology const& topology, char const* const* leaf_argv, char const* const* leaf_env = nullptr,
		UserCallbacks&& callbacks = UserCallbacks(), const std::atomic<bool>* killSwitch = nullptr
	);

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

	// Sends a synchronous "ping" to all nodes in `communicator`. After `timeoutMs`
	// milliseconds have passed with no response or every rank in `stopAfter` has 
	// responded, returns the set of ranks that heard the ping and responded.
	//
	// If `timeoutMs` is -1, no timeout is used, and the function waits an
	// unlimited amount of time for every rank in `stopAfter` to respond.
	// If `stopAfter` is an empty set, the function only stops waiting after the
	// timeout specified by `timeoutMs` is hit.
	// Passing `timeoutMs` < 0 and `stopAfter` == {} is invalid. If this invalid
	// set of arguments is passed, the function immediately returns an empty set.
	RankSet sendPing(Communicator const& communicator, int timeoutMs, RankSet const& stopAfter);
	// return true if all `ranks` respond to a ping on `communicator` before a
	// reasonable amount of time has passed.
	bool confirmPing(Communicator const& communicator, RankSet const& ranks);

	// Register a callback function to be called after a specified zmqnet::Event.
	// The event is passed as a template parameter,
	// e.g. registerCallback<zmqnet::Event::InitialConnection>(myFn);
	template<Event e, typename FnT>
	void registerCallback(FnT&& CBFn) { Frontend::registerCallback<e>(std::forward<FnT>(CBFn)); }
	template<Event e>
	void removeCallback() { Frontend::removeCallback<e>(); }

	// Notify all Backend instances that the network should exit. Returns once all Backend instances
	// have exited.
	void runExit();

private:

	// specializations of generic methods in detail::Frontend
	Communicator createCommunicator(const RankSet& ranks);
	Stream createStream(Communicator const& communicator, Stream::Id const& streamId,
		std::string const& filterLibraryName, std::string const& filterFunctionName,
		zmqnet::StreamSync const& streamSync);
	void sendExit();
};

// Asynchronous Frontend register callbacks for each handled Stream that will receive
// the contents of every incoming message for that stream.
class AsyncFrontend : private detail::Frontend
{
private: // types
	class HeartbeatManager;

public: // types

	// Command callbacks take a pair of Stream ID and ByteArray, where the Stream ID corresponds
	// to the stream on which the incoming command is received, and the ByteArray is the raw
	// contents of the command.
	using CallbackFunc = std::function<void(Stream::Id const&, ByteArrayPtr)>;

private: // members
	std::unordered_map<Stream::Id, std::future<void>> m_callbackProcessors;
	std::unique_ptr<net::shared_semaphore> m_blocking_registration;
	std::future<zmq::socket_t> m_responseDistributor;
	constexpr static auto m_pingEndpoint = "inproc://ping-main";
	constexpr static auto m_heartbeatPingEndpoint = "inproc://ping-heartbeat";
	zmq::socket_t m_pingSocket;
	constexpr static auto m_commandDistributorEndpoint = "inproc://command";
	zmq::socket_t m_controlSocket;
	std::unique_ptr<HeartbeatManager> m_heartbeatManager;
	std::future<zmq::socket_t> m_commandDistributor;

	// specializations of generic methods in detail::Frontend
	Communicator createCommunicator(const RankSet& ranks);
	Stream createStream(Communicator const& communicator, Stream::Id const& streamId,
		std::string const& filterLibraryName, std::string const& filterFunctionName,
		zmqnet::StreamSync const& streamSync);
	void sendExit();

public: // interface

	// See zmqnet::SyncFrontend for description
	AsyncFrontend(int const app_id, long int const session_id, long int const manifest_id,
		Topology const& topology, char const* const* leaf_argv, char const* const* leaf_env = nullptr,
		UserCallbacks&& callbacks = UserCallbacks(), const std::atomic<bool>* killSwitch = nullptr
	);
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

	// Sends a synchronous "ping" to all nodes in `communicator`. After `timeoutMs`
	// milliseconds have passed with no response or every rank in `stopAfter` has 
	// responded, returns the set of ranks that heard the ping and responded.
	//
	// If `timeoutMs` is -1, no timeout is used, and the function waits an
	// unlimited amount of time for every rank in `stopAfter` to respond.
	// If `stopAfter` is an empty set, the function only stops waiting after the
	// timeout specified by `timeoutMs` is hit.
	// Passing `timeoutMs` < 0 and `stopAfter` == {} is invalid. If this invalid
	// set of arguments is passed, the function immediately returns an empty set.
	RankSet sendPing(Communicator const& communicator, int timeoutMs, RankSet const& stopAfter);
	// return true if all `ranks` respond to a ping on `communicator` before a
	// reasonable amount of time has passed.
	bool confirmPing(Communicator const& communicator, RankSet const& ranks);

	// Register a callback function to be called after a specified zmqnet::Event.
	// The event is passed as a template parameter,
	// e.g. registerCallback<zmqnet::Event::InitialConnection>(myFn);
	template<Event e, typename FnT>
	void registerCallback(FnT CBFn) { Frontend::registerCallback<e>(CBFn); }
	template<Event e>
	void removeCallback() { Frontend::removeCallback<e>(); }

	// Notify all Backend instances that the network should exit and wait for their exit
	// to complete. Returns once all Backend instances have exited.
	void runExit();

	// Immediately disconnect from network with no guarantees about cleanup. Commnodes and
	// leaf nodes might still be left running after this, but frontend communication
	// channels with them will be cleaned up and the AsyncFrontend can be destroyed.
	void forceDisconnect();

	// Start a monitor thread, sends a ping to `communicator` every `interval_ms`
	void startHeartbeat(int interval_ms);

	// Whether the heartbeat thread is running
	bool heartbeatRunning();

	// Query the network heartbeat state. If monitor thread was not started,
	// this will return false
	bool heartbeatLive();
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
	// leaf binary. Extract these arguments using `getParams` to obtain a struct
	// to pass to the backend constructor, followed by the remaining argument
	// count and array.

	// Example usage:
	// auto const [backendParams, rest_argc, rest_argv]
	//     = zmqnet::SyncBackend::getParams(argc, argv);
	// auto backend = zmqnet::SyncBackend(backendParams);
	template <typename CharPtr>
	static auto getParams(int argc, CharPtr argv) { return detail::Backend::getParams(argc, argv); }

	// Connect to the communication tree using network parameters extracted by
	// zmqnet::Backend::getParams(argc, argv) where argc and argv are the actual arguments provided
	// to the user binary calling SyncBackend.
	SyncBackend(detail::Backend::BackendParams const& backendParams);

	// If started in packed mode, each backend will be assigned a rank and target PID by
	// the commnode. This supports launching one leaf to analyze each rank on the node.
	// If not launched in packed mode, these functions will throw.
	int getRank() const { return detail::Backend::getRank(); }
	pid_t getTargetPid() const { return detail::Backend::getTargetPid(); }

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
	// leaf binary. Extract these arguments using `getParams` to obtain a struct
	// to pass to the backend constructor, followed by the remaining argument
	// count and array.

	// Example usage:
	// auto const [backendParams, rest_argc, rest_argv]
	//     = zmqnet::AsyncBackend::getParams(argc, argv);
	// auto backend = zmqnet::SyncBackend(backendParams);
	template <typename CharPtr>
	static auto getParams(int argc, CharPtr argv) { return detail::Backend::getParams(argc, argv); }

	// The constructor takes the same network parameters as the synchronous variant, as extracted
	// from the leaf binary arguments by zmqnet::Backend::getParams.
	AsyncBackend(detail::Backend::BackendParams const& backendParams);
	~AsyncBackend();

	// If started in packed mode, each backend will be assigned a rank and target PID by
	// the commnode. This supports launching one leaf to analyze each rank on the node.
	// If not launched in packed mode, these functions will throw.
	int getRank() const { return detail::Backend::getRank(); }
	pid_t getTargetPid() const { return detail::Backend::getTargetPid(); }

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
