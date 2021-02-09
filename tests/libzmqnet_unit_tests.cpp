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

#include "libzmqnet_tests.hpp"

class ZmqnetUnitTest : public ::testing::Test
{
protected:
	ZmqnetUnitTest()
	{}

	~ZmqnetUnitTest() override
	{}
};

TEST_F(ZmqnetUnitTest, EmptyTest)
{}

int main(int argc, char **argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
