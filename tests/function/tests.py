# Copyright 2022 Hewlett Packard Enterprise Development LP.

from avocado import Test

import os
import subprocess

def google_test(test, testname):
    debugstr = "_debug" if test.debug == True else ""
    cmd = ["./libzmqnet_function_tests", "--gtest_filter=ZmqnetFunctionTest." + testname]
    tmpdir = os.getcwd() + "/tmp/"
    try:
        os.mkdir(tmpdir)
    except OSError:
        pass

    tmpfile = tmpdir + testname + debugstr + ".out"

    with open(tmpfile, "w") as tmpf:
        try:
            p = subprocess.Popen(cmd, stdout = tmpf, stderr = tmpf)
            p.wait(90)
            test.assertTrue(p.returncode == 0, f"{testname} returned non-zero ({p.returncode})")
        except subprocess.TimeoutExpired:
            p.kill()

            launch_complete = False
            with open(tmpfile, "r") as tmpfr:
                for line in tmpfr.readlines():
                    if "Launch complete" in line:
                        launch_complete = True
                        break

            if not launch_complete:
                test.cancel("Timed out after never successfully launching a job")

            test.fail("Timed out")

class ZmqnetTest(Test):
    def setUp(self):
        self.debug = self.params.get("debug")
        if self.debug != None and self.debug == True:
            os.environ["CTI_DEBUG"] = str(self.debug)
            print(f"CTI_DEBUG = {str(self.debug)}")
        else:
            self.debug = False
            if "CTI_DEBUG" in os.environ:
                del os.environ["CTI_DEBUG"]

    def test_LaunchExitTest(self):
        google_test(self, "LaunchExitTest")

    def test_LaunchExitTest4(self):
        google_test(self, "LaunchExitTest4")

    def test_LaunchExitTest4Packed(self):
        google_test(self, "LaunchExitTest4Packed")

    def test_SingleNodeLaunchTest(self):
        google_test(self, "SingleNodeLaunchTest")

    def test_SingleNodeStreamTest(self):
        google_test(self, "SingleNodeStreamTest")

    def test_SingleNodeCommandTest(self):
        google_test(self, "SingleNodeCommandTest")

    def test_SingleNodeCommandTestAsync(self):
        google_test(self, "SingleNodeCommandTestAsync")

    def test_MultiNodeCommandTest(self):
        google_test(self, "MultiNodeCommandTest")

    def test_MultiNodeCommandTestAsync(self):
        google_test(self, "MultiNodeCommandTestAsync")

    def test_SubsetCreationTest(self):
        google_test(self, "SubsetCreationTest")

    def test_SubsetCreationTestByRank(self):
        google_test(self, "SubsetCreationTestByRank")

    def test_PingTest(self):
        google_test(self, "PingTest")

    def test_PingTestAsync(self):
        google_test(self, "PingTestAsync")

    def test_PingTestPacked(self):
        google_test(self, "PingTestPacked")

    def test_PingTestAsyncPacked(self):
        google_test(self, "PingTestAsyncPacked")

    def test_SubsetCommandTest(self):
        google_test(self, "SubsetCommandTest")

    def test_SubsetCommandTestByRank(self):
        google_test(self, "SubsetCommandTestByRank")

    def test_SubsetCreationTestAsync(self):
        google_test(self, "SubsetCreationTestAsync")

    def test_SubsetCreationTestByRankAsync(self):
        google_test(self, "SubsetCreationTestByRankAsync")

    def test_SubsetCommandTestAsync(self):
        google_test(self, "SubsetCommandTestAsync")

    def test_SubsetCommandTestAsyncByRank(self):
        google_test(self, "SubsetCommandTestAsyncByRank")

    def test_ImmediateTestAsync(self):
        google_test(self, "ImmediateTestAsync")

    def test_TimeoutTestAsync(self):
        google_test(self, "TimeoutTestAsync")

    def test_CallbackLaunchTestFcfs(self):
        google_test(self, "CallbackLaunchTestFcfs")

    def test_CallbackLaunchTestFcfsAsync(self):
        google_test(self, "CallbackLaunchTestFcfsAsync")

