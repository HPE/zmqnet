#!/bin/bash

# This file is part of the the HPE build pipeline that is required to stay
# in the top level directory.

set -xe
echo Entering $0

./hpe/runBuildPackage.sh
./hpe/runInstallTest.sh

find / -path /proc -prune -o -name \*.rpm

# ./runPostBuild.sh is not needed since the following put the RPMs in the correct location
mkdir -p $PWD/RPMS/$TARGET_OS
cp -r /workspace/hpe/rpmbuild/RPMS/* $PWD/RPMS/$TARGET_OS

echo Exiting $0
