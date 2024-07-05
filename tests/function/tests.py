# Copyright 2022 Hewlett Packard Enterprise Development LP.

from avocado import Test

import os
import subprocess
import socket

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

            # If the test returned non-zero, before marking it as a failure, make sure
            # there was no CANCEL: strings emitted indicating that a prerequisite couldn't be met
            if p.returncode != 0:
                skipped = False
                with open(tmpfile, "r") as tmpfr:
                    for line in tmpfr.readlines():
                        if "CANCEL:" in line:
                            skipped = True
                            break

                if skipped:
                    test.cancel("Couldn't meet a prerequisite. Check logs.")

                test.assertTrue(p.returncode == 0, f"{testname} returned non-zero ({p.returncode}). Check logs.")
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
            os.environ["LIBZMQNET_DEBUG"] = str(self.debug)
            print(f"LIBZMQNET_DEBUG = {str(self.debug)}")
        else:
            self.debug = False
            if "LIBZMQNET_DEBUG" in os.environ:
                del os.environ["LIBZMQNET_DEBUG"]

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

    def test_HeartbeatTest(self):
        google_test(self, "HeartbeatTest")

    def test_UnexpectedExitTest(self):
        google_test(self, "UnexpectedExitTest")

class CleanupTest(Test):
    def test_cleanup(self):
        # XXX: Compute nodes on slice have extremely small /tmp directories.
        #      A single test suite run will fill them up about 20% with CTI files.
        #      Until CTI gets proper clean up strategies, we try to mitigate it here.

        if socket.gethostname() == "uan01": # slice's uan
            # The full test suite will run this "test" twice, we only want to clean up once.
            # Checking for debug == False instead of True means that cleanup will also be
            # run when running the small test suite.

            debug = self.params.get("debug")
            if debug == None or debug == False:
                # Attempt to clean up the cti_* directories we created on compute nodes
                p = subprocess.run(["./cleanup.sh"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                print(p.stdout.decode())
                print(p.stderr.decode())

                # Dump into /tmp so nightly test picks results up as logs
                with open(os.getcwd() + "/tmp/cleanup.out", "w") as outf:
                    outf.write(p.stdout.decode())
                    outf.write(p.stderr.decode())
            else:
                self.cancel("Don't run cleanup twice")
        else:
            self.cancel("Not running cleanup on non-slice")

        self.cancel("Cleanup complete")

