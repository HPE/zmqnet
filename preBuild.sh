#!/bin/bash

# This file is part of the the HPE build pipeline that is required to stay
# in the top level directory.

set -xe
echo Entering $0

echo Show TARGET_OS and TARGET_ARCH
set | grep TARGET || true

# For backward compatability with Jenkins builds
ln -s /workspace /home/jenkins 
git config --global --add safe.directory /workspace

test -f ./hpe/runBuildPrep.sh && ./hpe/runBuildPrep.sh

echo Exiting $0
