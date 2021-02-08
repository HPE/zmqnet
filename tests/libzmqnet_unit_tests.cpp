/*
 * Copyright 2019,2020 Hewlett Packard Enterprise Development LP.
 *
 * Unpublished Proprietary Information.
 * This unpublished work is protected to trade secret, copyright and other laws.
 * Except as permitted by contract or express written permission of Hewlett Packard Enterprise Development LP.,
 * no part of this work or its content may be used, reproduced or disclosed
 * in any form.
 */

#include "gtest/gtest.h"

#include "src/cti_argv.hpp"
#include "zmqnet.hpp"

#include "common_tools_fe.h"

struct BackendArgv : public cti::Argv {
	using Option    = cti::Argv::Option;
	using Parameter = cti::Argv::Parameter;

	static constexpr Parameter Sync { "sync", 's' };

	static constexpr GNUOption long_options[] = {
		Sync,
		long_options_done
	};
};

class ZmqnetUnitTest : public ::testing::Test
{
protected:
	ZmqnetUnitTest()
	{}

	~ZmqnetUnitTest() override
	{}
};

class ZmqnetFunctionTest : public ::testing::Test
{
protected:
	int app_id = 0;

	ZmqnetFunctionTest() {}

	virtual void SetUp()
	{}

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

	virtual void TearDown()
	{
		if (app_id > 0) {
			cti_deregisterApp(app_id);
			app_id = 0;
		}
	}
};

TEST_F(ZmqnetFunctionTest, LaunchExitTest)
{
	// Launch network with one node
	auto network = launchSyncNetwork(1, 1,
		zmqnet::Topology { .launch = zmqnet::Topology::Fcfs, .fanout = 1 });

	// Exit network
	network.runExit();
}

int main(int argc, char **argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
