/*
 * Copyright 2021 Hewlett Packard Enterprise Development LP.
 *
 * Unpublished Proprietary Information.
 * This unpublished work is protected to trade secret, copyright and other laws.
 * Except as permitted by contract or express written permission of Hewlett Packard Enterprise Development LP.,
 * no part of this work or its content may be used, reproduced or disclosed
 * in any form.
 */

#include "gtest/gtest.h"

#include "zmqnet.hpp"

class ZmqnetUnitTest : public ::testing::Test
{
protected:
	ZmqnetUnitTest()
	{}

	~ZmqnetUnitTest() override
	{}
};

TEST_F(ZmqnetUnitTest, ByteArrayTest)
{
	// Test byte array from C string
	{ auto const c_string = "zmqnet";
		auto const c_string_len = ::strlen(c_string) + 1;

		auto const c_string_array = zmqnet::make_ByteArray(c_string, c_string_len);

		EXPECT_EQ(c_string_len, c_string_array.size());
		EXPECT_EQ(::strncmp(c_string, (char const*)c_string_array.begin(), c_string_len), 0);
	}

	// Test byte array from byte buffer
	{ unsigned char buf[] = {0x00, 0xff, 0x00};
		auto const buf_len = sizeof(buf);

		auto const buf_array = zmqnet::make_ByteArray(buf, buf_len);

		EXPECT_EQ(buf_len, buf_array.size());
		EXPECT_EQ(::memcmp(buf, buf_array.begin(), buf_len), 0);
	}
}

TEST_F(ZmqnetUnitTest, SyncBackendParamsTest)
{
	// Test no additional backend parameters
	{ char const* backend_argv[] = { "argv0", "0", "controlEndpoint", "dataEndpoint", "publicKey", "1",
		"a.out", nullptr };
		auto const backend_argc = (sizeof(backend_argv) / sizeof(backend_argv[0])) - 1;

		auto const [beParams, rest_argc, rest_argv] =
			zmqnet::SyncBackend::getParams(backend_argc, backend_argv);

		EXPECT_EQ((int)beParams.authType, 0);
		EXPECT_EQ(beParams.upstreamControlEndpoint, "controlEndpoint");
		EXPECT_EQ(beParams.upstreamDataEndpoint, "dataEndpoint");
		EXPECT_EQ(beParams.upstreamPublicKey, "publicKey");
		EXPECT_EQ(beParams.node_id, 1);

		EXPECT_EQ(rest_argc, 1);
		EXPECT_EQ(rest_argv[0], "a.out");
		EXPECT_EQ(rest_argv[1], nullptr);
	}

	// Test additional backend parameters
	{ char const* backend_argv[] = { "argv0", "0", "controlEndpoint", "dataEndpoint", "publicKey", "1",
		"argv1", "argv2", nullptr };
		auto const backend_argc = (sizeof(backend_argv) / sizeof(backend_argv[0])) - 1;

		auto const [beParams, rest_argc, rest_argv] =
			zmqnet::SyncBackend::getParams(backend_argc, backend_argv);

		EXPECT_EQ((int)beParams.authType, 0);
		EXPECT_EQ(beParams.upstreamControlEndpoint, "controlEndpoint");
		EXPECT_EQ(beParams.upstreamDataEndpoint, "dataEndpoint");
		EXPECT_EQ(beParams.upstreamPublicKey, "publicKey");
		EXPECT_EQ(beParams.node_id, 1);

		EXPECT_EQ(rest_argc, 2);
		EXPECT_EQ(rest_argv[0], "argv1");
		EXPECT_EQ(rest_argv[1], "argv2");
		EXPECT_EQ(rest_argv[2], nullptr);
	}
}

int main(int argc, char **argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
