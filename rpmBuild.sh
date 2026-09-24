#!/bin/bash

# This file is part of the the HPE build pipeline that is required to stay
# in the top level directory.

set -xe
echo Entering $0

./hpe/runBuild.sh

echo Exiting $0
