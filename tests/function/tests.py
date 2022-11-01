# Copyright 2022 Hewlett Packard Enterprise Development LP.

from avocado import Test

import os
import subprocess

def google_test(test):
    testname = test.__class__.__name__
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
            p.wait(60)
            test.assertTrue(p.returncode == 0, f"{testname} returned non-zero ({p.returncode})")
        except subprocess.TimeoutExpired:
            p.kill()

            # XXX: on pinoak, jobs can randomly fail due to MPI errors.
            with open(tmpfile, "r") as tmpfr:
                for line in tmpfr.readlines():
                    if "MPICH ERROR" in line:
                        test.cancel("Timed out due to MPICH ERROR")

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

class LaunchExitTest(ZmqnetTest):
    def test(self):
        google_test(self)

class SingleNodeLaunchTest(ZmqnetTest):
    def test(self):
        google_test(self)

class SingleNodeStreamTest(ZmqnetTest):
    def test(self):
        google_test(self)

class SingleNodeCommandTest(ZmqnetTest):
    def test(self):
        google_test(self)

class SingleNodeCommandTestAsync(ZmqnetTest):
    def test(self):
        google_test(self)

class MultiNodeCommandTest(ZmqnetTest):
    def test(self):
        google_test(self)

class MultiNodeCommandTestAsync(ZmqnetTest):
    def test(self):
        google_test(self)

class SubsetCreationTest(ZmqnetTest):
    def test(self):
        google_test(self)

class SubsetCreationTestByRank(ZmqnetTest):
    def test(self):
        google_test(self)

class PingTest(ZmqnetTest):
    def test(self):
        google_test(self)

class PingTestAsync(ZmqnetTest):
    def test(self):
        google_test(self)

class SubsetCommandTest(ZmqnetTest):
    def test(self):
        google_test(self)

class SubsetCommandTestByRank(ZmqnetTest):
    def test(self):
        google_test(self)

class SubsetCreationTestAsync(ZmqnetTest):
    def test(self):
        google_test(self)

class SubsetCreationTestByRankAsync(ZmqnetTest):
    def test(self):
        google_test(self)

class SubsetCommandTestAsync(ZmqnetTest):
    def test(self):
        google_test(self)

class SubsetCommandTestAsyncByRank(ZmqnetTest):
    def test(self):
        google_test(self)

class ImmediateTestAsync(ZmqnetTest):
    def test(self):
        google_test(self)

class TimeoutTestAsync(ZmqnetTest):
    def test(self):
        google_test(self)

class CallbackLaunchTestFcfs(ZmqnetTest):
    def test(self):
        google_test(self)

class CallbackLaunchTestFcfsAsync(ZmqnetTest):
    def test(self):
        google_test(self)

