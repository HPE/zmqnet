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
		launcherArgs.add("-n " + std::to_string(num_nodes));

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
	auto launchSyncNetwork(int num_procs, int num_nodes, zmqnet::Topology const& topology)
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

		// Launch network
		auto const raw_backend_argv = (const char* const*)backendArgv.get();
		return zmqnet::SyncFrontend{app_id, session_id, manifest_id, topology, raw_backend_argv};
	}

	// Launch an asynchronous ZMQNet network using CTI
	auto launchAsyncNetwork(int num_procs, int num_nodes, zmqnet::Topology const& topology)
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

		// Launch network
		auto const raw_backend_argv = (const char* const*)backendArgv.get();
		return zmqnet::AsyncFrontend{app_id, session_id, manifest_id, topology, raw_backend_argv};
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


int main(int argc, char **argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
