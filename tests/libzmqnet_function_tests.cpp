/*
 * Copyright 2021 Hewlett Packard Enterprise Development LP.
 *
 * Unpublished Proprietary Information.
 * This unpublished work is protected to trade secret, copyright and other laws.
 * Except as permitted by contract or express written permission of Hewlett Packard Enterprise Development LP.,
 * no part of this work or its content may be used, reproduced or disclosed
 * in any form.
 */

#include <string>
#include <vector>
#include <set>
#include <algorithm>

#include "gtest/gtest.h"

#include "common_tools_fe.h"

#include "libzmqnet_tests.hpp"

class ZmqnetFunctionTest : public ::testing::Test
{
protected:
	int app_id = 0;

	ZmqnetFunctionTest() {}

	virtual void SetUp()
	{}

	// Launch a stopped application with CTI, so that the network can be launched alongside it.
	// Application ID is stored and cleaned up upon test teardown.
	void launchCtiApp(int num_procs, int num_nodes)
	{
		// Determine WLM type for launcher arguments
		auto const wlm_type = cti_current_wlm();

		// Create application arguments
		auto launcherArgs = cti::ManagedArgv{};
		launcherArgs.add("-n " + std::to_string(num_procs));

		switch (wlm_type) {

		case CTI_WLM_SLURM:
			launcherArgs.add("-N " + std::to_string(num_nodes));
			break;

		default:
			throw std::runtime_error("not implemented: ZmqnetFunctionTest for WLM type" + std::to_string(wlm_type));
		}

		launcherArgs.add("./stop");

		// Launch application
		app_id = cti_launchApp(launcherArgs.get(), -1, -1, nullptr, nullptr, nullptr);
		if (app_id == 0) {
			throw std::runtime_error(cti_error_str());
		}
	}

	// Launch a synchronous ZMQNet network using CTI
	auto launchSyncNetwork(int num_procs, int num_nodes, zmqnet::Topology const& topology,
		std::vector<std::string> const& backendArgs = {}, zmqnet::UserCallbacks&& ucbs = zmqnet::UserCallbacks())
	{
		// Launch app and save app ID
		launchCtiApp(num_procs, num_nodes);

		// Create and fill backend manifest
		auto const session_id = cti_createSession(app_id);
		auto const manifest_id = cti_createManifest(session_id);
		cti_addManifestBinary(manifest_id, "./test_backend");
		cti_addManifestLibrary(manifest_id, "./libfilter.so");

		// Create backend arguments
		auto backendArgv = cti::OutgoingArgv<BackendArgv>{"test_backend"};
		backendArgv.add(BackendArgv::Sync, "yes");
		for (auto&& backendArg : backendArgs) {
			backendArgv.add(backendArg);
		}

		// Launch network
		auto const raw_backend_argv = (const char* const*)backendArgv.get();
		return zmqnet::SyncFrontend{app_id, session_id, manifest_id, topology, raw_backend_argv, nullptr, std::move(ucbs)};
	}

	// Launch an asynchronous ZMQNet network using CTI
	auto launchAsyncNetwork(int num_procs, int num_nodes, zmqnet::Topology const& topology,
		std::vector<std::string> const& backendArgs = {}, zmqnet::UserCallbacks&& ucbs = zmqnet::UserCallbacks())
	{
		// Launch app and save app ID
		launchCtiApp(num_procs, num_nodes);

		// Create and fill backend manifest
		auto const session_id = cti_createSession(app_id);
		auto const manifest_id = cti_createManifest(session_id);
		cti_addManifestBinary(manifest_id, "./test_backend");
		cti_addManifestLibrary(manifest_id, "./libfilter.so");

		// Create backend arguments
		auto backendArgv = cti::OutgoingArgv<BackendArgv>{"test_backend"};
		backendArgv.add(BackendArgv::Sync, "no");
		for (auto&& backendArg : backendArgs) {
			backendArgv.add(backendArg);
		}

		// Launch network
		auto const raw_backend_argv = (const char* const*)backendArgv.get();
		return zmqnet::AsyncFrontend{app_id, session_id, manifest_id, topology, raw_backend_argv, nullptr, std::move(ucbs)};
	}

	// Clean up launched test application
	virtual void TearDown()
	{
		if (app_id > 0) {
			cti_deregisterApp(app_id);
			app_id = 0;
		}
	}
};

// Get the list of node IDs as produced by the test aggregation filter defined in libfilter.cpp
static std::set<int> parseNodeIds(std::string networkResult)
{
	auto result = std::set<int>{};

	// Remove brackets
	networkResult.erase(std::remove(networkResult.begin(), networkResult.end(), '{'), networkResult.end());
	networkResult.erase(std::remove(networkResult.begin(), networkResult.end(), '}'), networkResult.end());

	// Parse all node IDs
	auto resultStream = std::stringstream{networkResult};
	auto node_id = int{};
	while (resultStream >> node_id) {
		result.insert(node_id);
	}

	return result;
}

// Test synchronous network instantiation
TEST_F(ZmqnetFunctionTest, LaunchExitTest)
{
	// Launch network with one node
	auto network = launchSyncNetwork(1, 1,
		zmqnet::Topology { .launch = zmqnet::Topology::Fcfs, .fanout = 1 });

	// Exit network
	network.runExit();
}

// Test launching a single node
TEST_F(ZmqnetFunctionTest, SingleNodeLaunchTest)
{
	// Launch network with one node
	auto network = launchSyncNetwork(1, 1,
		zmqnet::Topology { .launch = zmqnet::Topology::Fcfs, .fanout = 1 });

	// Exit network
	network.runExit();
}

// Test creating a stream with a single node
TEST_F(ZmqnetFunctionTest, SingleNodeStreamTest)
{
	// Launch network with one node
	auto network = launchSyncNetwork(1, 1,
		zmqnet::Topology { .launch = zmqnet::Topology::Fcfs, .fanout = 1 });

	// Create stream
	auto allNodesCommunicator = network.getAllNodesCommunicator();
	(void)network.registerStream(allNodesCommunicator, TestStreamId,
		"libfilter.so", "run", zmqnet::sync::WaitForAll{});

	// Exit network
	network.runExit();
}

// Test sending command to a single node, check result
TEST_F(ZmqnetFunctionTest, SingleNodeCommandTest)
{
	// Launch network with one node
	auto network = launchSyncNetwork(1, 1,
		zmqnet::Topology { .launch = zmqnet::Topology::Fcfs, .fanout = 1 });

	// Create stream
	auto allNodesCommunicator = network.getAllNodesCommunicator();
	auto stream = network.registerStream(allNodesCommunicator, TestStreamId,
		"libfilter.so", "run", zmqnet::sync::WaitForAll{});

	// Send command and get result
	auto const result = network.runCommand(stream, "send", 5);

	// Check result
	auto const nodeIds = parseNodeIds((char const*)result.asBytes().begin());
	EXPECT_EQ(nodeIds.size(), size_t{1});

	// Exit network
	network.runExit();
}

// Test sending an asynchronous command to a single node, check result
TEST_F(ZmqnetFunctionTest, SingleNodeCommandTestAsync)
{
	auto result_mutex = std::mutex{};
	auto result_cv = std::condition_variable{};
	auto results_received = std::atomic<size_t>{0};

	// Launch network with one node
	auto network = launchAsyncNetwork(1, 1,
		zmqnet::Topology { .launch = zmqnet::Topology::Fcfs, .fanout = 1 });

	// Create callback
	auto send_callback = [&](zmqnet::Stream::Id const& streamId, zmqnet::ByteArrayPtr result) {
		ASSERT_EQ(streamId, TestStreamId);

		// Parse result
		auto const nodeIds = parseNodeIds((char const*)result.asBytes().begin());
		results_received = nodeIds.size();
		result_cv.notify_one();
	};

	// Regster callback and create stream
	network.registerStreamCallback(TestStreamId, send_callback);
	auto allNodesCommunicator = network.getAllNodesCommunicator();
	auto stream = network.registerStream(allNodesCommunicator, TestStreamId,
		"libfilter.so", "run", zmqnet::sync::WaitForAll{});

	// Send command
	network.sendCommand(stream, "send", 5);

	// Wait for result
	{ auto result_lock = std::unique_lock<std::mutex>{result_mutex};
		result_cv.wait(result_lock, [&results_received]() {
			return results_received.load() > 0;
		});
	}

	// Check result
	EXPECT_EQ(results_received.load(), size_t{1});

	// Exit network
	network.runExit();
}

// Test sending command to multiple nodes, check result
TEST_F(ZmqnetFunctionTest, MultiNodeCommandTest)
{
	// Launch network with four nodes
	auto network = launchSyncNetwork(4, 4,
		zmqnet::Topology { .launch = zmqnet::Topology::Fcfs, .fanout = 2 });

	// Create stream
	auto allNodesCommunicator = network.getAllNodesCommunicator();
	auto stream = network.registerStream(allNodesCommunicator, TestStreamId,
		"libfilter.so", "run", zmqnet::sync::WaitForAll{});

	// Send command and get result
	auto const result = network.runCommand(stream, "send", 5);

	// Check result
	auto const nodeIds = parseNodeIds((char const*)result.asBytes().begin());
	EXPECT_EQ(nodeIds.size(), size_t{4});

	// Exit network
	network.runExit();
}

// Test sending asynchronous command to multiple nodes, check result
TEST_F(ZmqnetFunctionTest, MultiNodeCommandTestAsync)
{
	auto result_mutex = std::mutex{};
	auto result_cv = std::condition_variable{};
	auto results_received = std::atomic<size_t>{0};

	// Launch network with four nodes
	auto network = launchAsyncNetwork(4, 4,
		zmqnet::Topology { .launch = zmqnet::Topology::Fcfs, .fanout = 1 });

	// Create callback
	auto send_callback = [&](zmqnet::Stream::Id const& streamId, zmqnet::ByteArrayPtr result) {
		ASSERT_EQ(streamId, TestStreamId);

		// Parse result
		auto const nodeIds = parseNodeIds((char const*)result.asBytes().begin());
		results_received = nodeIds.size();
		result_cv.notify_one();
	};

	// Regster callback and create stream
	network.registerStreamCallback(TestStreamId, send_callback);
	auto allNodesCommunicator = network.getAllNodesCommunicator();
	auto stream = network.registerStream(allNodesCommunicator, TestStreamId,
		"libfilter.so", "run", zmqnet::sync::WaitForAll{});

	// Send command
	network.sendCommand(stream, "send", 5);

	// Wait for result
	{ auto result_lock = std::unique_lock<std::mutex>{result_mutex};
		result_cv.wait(result_lock, [&results_received]() {
			return results_received.load() > 0;
		});
	}

	// Check result
	EXPECT_EQ(results_received.load(), size_t{4});

	// Exit network
	network.runExit();
}

// Test creating a stream subset across multiple nodes
TEST_F(ZmqnetFunctionTest, SubsetCreationTest)
{
	// Launch network with four nodes
	auto network = launchSyncNetwork(4, 4,
		zmqnet::Topology { .launch = zmqnet::Topology::Fcfs, .fanout = 2 });

	// Create node subset
	auto hostsPlacement = std::unique_ptr<cti_hostsList_t, decltype(&cti_destroyHostsList)>
		{ cti_getAppHostsPlacement(app_id)
		, cti_destroyHostsList
	};

	// Add nodes with even IDs to subset
	auto some_nodes = std::set<std::string>{};
	for (int i = 0; i < hostsPlacement->numHosts; i += 2) {
		some_nodes.emplace(hostsPlacement->hosts[i].hostname);
	}

	// Create subset stream
	auto someNodesCommunicator = network.registerCommunicator(some_nodes);
	(void)network.registerStream(someNodesCommunicator, TestStreamId,
		"libfilter.so", "run", zmqnet::sync::WaitForAll{});

	// Exit network
	network.runExit();
}

// Test creating a stream subset across a single node
TEST_F(ZmqnetFunctionTest, SubsetCreationTestByRank)
{
	// Launch network with four nodes
	auto network = launchSyncNetwork(8, 1,
		zmqnet::Topology { .launch = zmqnet::Topology::Packed, .fanout = 2 });

	std::set<size_t> ranks = {0, 1, 2, 3};

	// Create subset stream
	auto someNodesCommunicator = network.registerCommunicator(ranks);
	(void)network.registerStream(someNodesCommunicator, TestStreamId,
		"libfilter.so", "run", zmqnet::sync::WaitForAll{});

	// Exit network
	network.runExit();
}

// Test ping function
TEST_F(ZmqnetFunctionTest, PingTest)
{
	auto allRanks = zmqnet::RankSet{0, 1, 2, 3};
	auto halfRanks = zmqnet::RankSet{0, 2};

	auto network = launchSyncNetwork(allRanks.size(), allRanks.size(),
		zmqnet::Topology {.launch = zmqnet::Topology::Fcfs, .fanout = 2});

	auto halfComm = network.registerCommunicator(halfRanks);
	auto allComm = network.getAllNodesCommunicator();	

	// Invalid combination of timeout/ranks should return empty set immediately
	ASSERT_EQ(network.sendPing(allComm, -1, {}), zmqnet::RankSet{}); 
	
	// Ping until no response for 1000 ms
	auto responded = network.sendPing(allComm, 1000, {});
	ASSERT_EQ(responded, allRanks);

	// Blocking ping until ranks in halfRanks respond
	responded = network.sendPing(halfComm, -1, halfRanks);
	ASSERT_EQ(responded, halfRanks);

	// Ping only half the ranks, but pass all ranks. Should timeout
	responded = network.sendPing(halfComm, 1000, allRanks);
	ASSERT_EQ(responded, halfRanks);	
	
	network.runExit();
}

// Test ping function on an Async network
TEST_F(ZmqnetFunctionTest, PingTestAsync)
{
	auto allRanks = zmqnet::RankSet{0, 1, 2, 3};
	auto halfRanks = zmqnet::RankSet{0, 2};

	auto network = launchAsyncNetwork(allRanks.size(), allRanks.size(),
		zmqnet::Topology {.launch = zmqnet::Topology::Fcfs, .fanout = 2});

	auto halfComm = network.registerCommunicator(halfRanks);
	auto allComm = network.getAllNodesCommunicator();	

	// Invalid combination of timeout/ranks should return empty set immediately
	ASSERT_EQ(network.sendPing(allComm, -1, {}), zmqnet::RankSet{}); 
	
	// Ping until no response for 1000 ms
	auto responded = network.sendPing(allComm, 1000, {});
	ASSERT_EQ(responded, allRanks);

	// Blocking ping until ranks in halfRanks respond
	responded = network.sendPing(halfComm, -1, halfRanks);
	ASSERT_EQ(responded, halfRanks);

	// Ping only half the ranks, but pass all ranks. Should timeout
	responded = network.sendPing(halfComm, 1000, allRanks);
	ASSERT_EQ(responded, halfRanks);	

	network.runExit();
}


// Test sending command to a stream subset across multiple nodes, check result
TEST_F(ZmqnetFunctionTest, SubsetCommandTest)
{
	// Launch network with four nodes
	auto network = launchSyncNetwork(4, 4,
		zmqnet::Topology { .launch = zmqnet::Topology::Fcfs, .fanout = 2 });

	// Create node subset
	auto hostsPlacement = std::unique_ptr<cti_hostsList_t, decltype(&cti_destroyHostsList)>
		{ cti_getAppHostsPlacement(app_id)
		, cti_destroyHostsList
	};

	// Add nodes with even IDs to subset
	auto some_nodes = std::set<std::string>{};
	for (int i = 0; i < hostsPlacement->numHosts; i += 2) {
		some_nodes.emplace(hostsPlacement->hosts[i].hostname);
	}

	// Create subset stream
	auto someNodesCommunicator = network.registerCommunicator(some_nodes);
	auto someNodesStream = network.registerStream(someNodesCommunicator, TestStreamId,
		"libfilter.so", "run", zmqnet::sync::WaitForAll{});

	// Send command and get result
	auto const result = network.runCommand(someNodesStream, "send", 5);

	// Check result
	auto const nodeIds = parseNodeIds((char const*)result.asBytes().begin());
	EXPECT_EQ(nodeIds.size(), size_t{2});

	// Exit network
	network.runExit();
}

// Test sending command to a stream subset across a single node, check result
TEST_F(ZmqnetFunctionTest, SubsetCommandTestByRank)
{
	// Launch network with four nodes
	auto network = launchSyncNetwork(4, 4,
		zmqnet::Topology { .launch = zmqnet::Topology::Packed, .fanout = 2 });

	// Add nodes with even IDs to subset
	auto some_nodes = std::set<size_t>{0, 2};

	// Create subset stream
	auto someNodesCommunicator = network.registerCommunicator(some_nodes);
	auto someNodesStream = network.registerStream(someNodesCommunicator, TestStreamId,
		"libfilter.so", "run", zmqnet::sync::WaitForAll{});

	// Send command and get result
	auto const result = network.runCommand(someNodesStream, "send", 5);

	// Check result
	auto const nodeIds = parseNodeIds((char const*)result.asBytes().begin());
	
	EXPECT_EQ(nodeIds.size(), size_t{2});

	// Exit network
	network.runExit();
}

// Test creating an asynchronous stream subset across multiple nodes
TEST_F(ZmqnetFunctionTest, SubsetCreationTestAsync)
{
	// Launch network with four nodes
	auto network = launchAsyncNetwork(4, 4,
		zmqnet::Topology { .launch = zmqnet::Topology::Fcfs, .fanout = 1 });

	// Create callback
	auto send_callback = [&](zmqnet::Stream::Id const& streamId, zmqnet::ByteArrayPtr result) {
		ASSERT_EQ(streamId, TestStreamId);
	};

	// Create node subset
	auto hostsPlacement = std::unique_ptr<cti_hostsList_t, decltype(&cti_destroyHostsList)>
		{ cti_getAppHostsPlacement(app_id)
		, cti_destroyHostsList
	};

	// Add nodes with even IDs to subset
	auto some_nodes = std::set<std::string>{};
	for (int i = 0; i < hostsPlacement->numHosts; i += 2) {
		some_nodes.emplace(hostsPlacement->hosts[i].hostname);
	}

	// Register callback and create subset stream
	network.registerStreamCallback(TestStreamId, send_callback);
	auto someNodesCommunicator = network.registerCommunicator(some_nodes);
	(void)network.registerStream(someNodesCommunicator, TestStreamId,
		"libfilter.so", "run", zmqnet::sync::WaitForAll{});

	// Exit network
	network.runExit();
}

// Test creating a stream subset across a single node
TEST_F(ZmqnetFunctionTest, SubsetCreationTestByRankAsync)
{
	// Launch network with four nodes
	auto network = launchAsyncNetwork(8, 1,
		zmqnet::Topology { .launch = zmqnet::Topology::Packed, .fanout = 1 });

	// Create callback
	auto send_callback = [&](zmqnet::Stream::Id const& streamId, zmqnet::ByteArrayPtr result) {
		ASSERT_EQ(streamId, TestStreamId);
	};

	// Add nodes with even IDs to subset
	std::set<size_t> some_nodes = {0, 1, 2, 3};

	// Register callback and create subset stream
	network.registerStreamCallback(TestStreamId, send_callback);
	auto someNodesCommunicator = network.registerCommunicator(some_nodes);
	(void)network.registerStream(someNodesCommunicator, TestStreamId,
		"libfilter.so", "run", zmqnet::sync::WaitForAll{});

	// Exit network
	network.runExit();
}

// Test sending an asynchronous command to a stream subset across multiple nodes, check result
TEST_F(ZmqnetFunctionTest, SubsetCommandTestAsync)
{
	auto result_mutex = std::mutex{};
	auto result_cv = std::condition_variable{};
	auto results_received = std::atomic<size_t>{0};

	// Launch network with four nodes
	auto network = launchAsyncNetwork(4, 4,
		zmqnet::Topology { .launch = zmqnet::Topology::Fcfs, .fanout = 1 });

	// Create callback
	auto send_callback = [&](zmqnet::Stream::Id const& streamId, zmqnet::ByteArrayPtr result) {
		ASSERT_EQ(streamId, TestStreamId);

		// Parse result
		auto const nodeIds = parseNodeIds((char const*)result.asBytes().begin());
		results_received = nodeIds.size();
		result_cv.notify_one();
	};

	// Create node subset
	auto hostsPlacement = std::unique_ptr<cti_hostsList_t, decltype(&cti_destroyHostsList)>
		{ cti_getAppHostsPlacement(app_id)
		, cti_destroyHostsList
	};

	// Add nodes with even IDs to subset
	auto some_nodes = std::set<std::string>{};
	for (int i = 0; i < hostsPlacement->numHosts; i += 2) {
		some_nodes.emplace(hostsPlacement->hosts[i].hostname);
	}

	// Create subset stream
	network.registerStreamCallback(TestStreamId, send_callback);
	auto someNodesCommunicator = network.registerCommunicator(some_nodes);
	auto someNodesStream = network.registerStream(someNodesCommunicator, TestStreamId,
		"libfilter.so", "run", zmqnet::sync::WaitForAll{});

	// Send command
	network.sendCommand(someNodesStream, "send", 5);

	// Wait for result
	{ auto result_lock = std::unique_lock<std::mutex>{result_mutex};
		result_cv.wait(result_lock, [&results_received]() {
			return results_received.load() > 0;
		});
	}

	// Check result
	EXPECT_EQ(results_received.load(), size_t{2});

	// Exit network
	network.runExit();
}

// Test sending an asynchronous command to a stream subset across a single node, check result
TEST_F(ZmqnetFunctionTest, SubsetCommandTestAsyncByRank)
{
	auto result_mutex = std::mutex{};
	auto result_cv = std::condition_variable{};
	auto results_received = std::atomic<size_t>{0};

	// Launch network with four nodes
	auto network = launchAsyncNetwork(4, 4,
		zmqnet::Topology { .launch = zmqnet::Topology::Packed, .fanout = 1 });

	// Create callback
	auto send_callback = [&](zmqnet::Stream::Id const& streamId, zmqnet::ByteArrayPtr result) {
		ASSERT_EQ(streamId, TestStreamId);

		// Parse result
		auto const nodeIds = parseNodeIds((char const*)result.asBytes().begin());
		results_received = nodeIds.size();
		result_cv.notify_one();
	};

	// Add nodes with even IDs to subset
	auto some_nodes = std::set<size_t>{0, 2};

	// Create subset stream
	network.registerStreamCallback(TestStreamId, send_callback);
	auto someNodesCommunicator = network.registerCommunicator(some_nodes);
	auto someNodesStream = network.registerStream(someNodesCommunicator, TestStreamId,
		"libfilter.so", "run", zmqnet::sync::WaitForAll{});

	// Send command
	network.sendCommand(someNodesStream, "send", 5);

	// Wait for result
	{ auto result_lock = std::unique_lock<std::mutex>{result_mutex};
		result_cv.wait(result_lock, [&results_received]() {
			return results_received.load() > 0;
		});
	}

	// Check result
	EXPECT_EQ(results_received.load(), size_t{2});

	// Exit network
	network.runExit();
}

// Test sending asynchronous command to multiple nodes with the immediate synchronization setting
TEST_F(ZmqnetFunctionTest, ImmediateTestAsync)
{
	auto result_mutex = std::mutex{};
	auto result_cv = std::condition_variable{};
	auto results_received = std::atomic<size_t>{0};
	auto callbacks_received = std::atomic<size_t>{0};

	// Launch network with four nodes
	auto network = launchAsyncNetwork(4, 4,
		zmqnet::Topology { .launch = zmqnet::Topology::Fcfs, .fanout = 1 });

	// Create callback
	auto send_callback = [&](zmqnet::Stream::Id const& streamId, zmqnet::ByteArrayPtr result) {
		ASSERT_EQ(streamId, TestStreamId);

		// Parse result
		auto const nodeIds = parseNodeIds((char const*)result.asBytes().begin());
		results_received += nodeIds.size();
		callbacks_received++;
		result_cv.notify_one();
	};

	// Regster callback and create stream
	network.registerStreamCallback(TestStreamId, send_callback);
	auto allNodesCommunicator = network.getAllNodesCommunicator();
	auto stream = network.registerStream(allNodesCommunicator, TestStreamId,
		"libfilter.so", "run", zmqnet::sync::Immediate{});

	// Send command
	network.sendCommand(stream, "send", 5);

	// Wait for result
	{ auto result_lock = std::unique_lock<std::mutex>{result_mutex};
		result_cv.wait(result_lock, [&callbacks_received]() {
			return callbacks_received.load() == size_t{4};
		});
	}

	// Check result
	EXPECT_EQ(results_received.load(), size_t{4});

	// Exit network
	network.runExit();
}

// Test sending asynchronous command to multiple nodes with a timeout setting, and one node
// dropping commands
TEST_F(ZmqnetFunctionTest, TimeoutTestAsync)
{
	auto result_mutex = std::mutex{};
	auto result_cv = std::condition_variable{};
	auto results_received = std::atomic<size_t>{0};

	// Launch network with four nodes
	auto network = launchAsyncNetwork(4, 4,
		zmqnet::Topology { .launch = zmqnet::Topology::Fcfs, .fanout = 1 },
		// Set node ID 0 to drop commands to test timeout
		{ BackendArgv::TimeoutNodeId.format("0") } );

	// Create callback
	auto send_callback = [&](zmqnet::Stream::Id const& streamId, zmqnet::ByteArrayPtr result) {
		ASSERT_EQ(streamId, TestStreamId);

		// Parse result
		auto const nodeIds = parseNodeIds((char const*)result.asBytes().begin());
		results_received += nodeIds.size();
		result_cv.notify_one();
	};

	// Regster callback and create stream
	network.registerStreamCallback(TestStreamId, send_callback);
	auto allNodesCommunicator = network.getAllNodesCommunicator();
	auto stream = network.registerStream(allNodesCommunicator, TestStreamId,
		"libfilter.so", "run",

		// Set a half-second timeout at each level
		zmqnet::sync::Timeout{ .ms = 500 });

	// Send command
	network.sendCommand(stream, "send", 5);

	// Wait for result
	{ auto result_lock = std::unique_lock<std::mutex>{result_mutex};
		result_cv.wait(result_lock, [&results_received]() {
			return results_received.load() >= size_t{3};
		});
	}

	// Check result
	EXPECT_EQ(results_received.load(), size_t{3});

	// Exit network
	network.runExit();
}

// Test launch callbacks
TEST_F(ZmqnetFunctionTest, CallbackLaunchTestFcfs) {
    auto callbacks = zmqnet::UserCallbacks();

    auto count = int{0};
    auto count_them = [&count](int which_one) {
        count++;
    };

    callbacks.registerCallback<zmqnet::Event::InitialConnection>(count_them);

    auto net = launchSyncNetwork(4, 2,
        zmqnet::Topology { .launch = zmqnet::Topology::Fcfs, .fanout = 1 },
        {} /* leaf args */, std::move(callbacks)
    );

    // in a Fcfs launch, the callback is invoked once per *node*
    ASSERT_EQ(count, 2);
}

TEST_F(ZmqnetFunctionTest, CallbackLaunchTestFcfsAsync) {
    auto callbacks = zmqnet::UserCallbacks();

    auto count = int{0};
    auto count_them = [&count](int which_one) {
        count++;
    };

    callbacks.registerCallback<zmqnet::Event::InitialConnection>(count_them);

    auto net = launchAsyncNetwork(4, 2,
        zmqnet::Topology { .launch = zmqnet::Topology::Fcfs, .fanout = 1 },
        {} /* leaf args */, std::move(callbacks)
    );

    // in a Fcfs launch, the callback is invoked once per *node*
    EXPECT_EQ(count, 2);

    net.runExit();
}

int main(int argc, char **argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
