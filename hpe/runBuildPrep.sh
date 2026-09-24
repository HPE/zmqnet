#!/bin/bash
#
# runBuildPrep.sh - Preps the build environment
#
# Copyright 2019-2024 Hewlett Packard Enterprise Development LP.
#
# Unpublished Proprietary Information.
# This unpublished work is protected to trade secret, copyright and other laws.
# Except as permitted by contract or express written permission of Hewlett Packard Enterprise Development LP.,
# no part of this work or its content may be used, reproduced or disclosed
# in any form.
#

# directory of this script
hpe="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$hpe/external/cdst_build_library/build_lib"

echo "############################################"
echo "#          Install dependencies            #"
echo "############################################"

target_pm=$(get_pm)
if [[ "$target_pm" == "$cdst_pm_zypper" ]]; then
    zypper --non-interactive install \
        environment-modules \
        libtool \
        binutils \
        gcc \
        glibc-devel \
        make \
        cmake \
        libuuid-devel \
        meson \
        openssl-devel \
        python3-pip \
        python311 \
        rpm-build \
        libsodium-devel
    check_exit_status
elif [[ "$target_pm" == "$cdst_pm_yum" ]]; then
    yum --assumeyes install \
        environment-modules \
        libtool \
        make \
        cmake \
        libuuid-devel \
        meson \
        openssl-devel \
        pandoc \
        python3-pip \
        rpm-build \
        python3.11 \
        wget \
        libsodium-devel
    check_exit_status
fi

install_common_pe
check_exit_status

install_cti
check_exit_status

echo "############################################"
echo "#          Ensuring build system           #"
echo "############################################"

# meson requires python 3.7 or newer
python3.11 -m ensurepip --upgrade
check_exit_status

#python3.11 -m pip install meson ninja --trusted-host https://arti.hpc.amslabs.hpecorp.net \
#      --extra-index-url https://arti.hpc.amslabs.hpecorp.net:443/artifactory/api/pypi/dst-pip-master-local/simple \
#      --index-url https://arti.hpc.amslabs.hpecorp.net:443/artifactory/api/pypi/pypi-remote/simple
#check_exit_status

capture_jenkins_build
check_exit_status

echo "############################################"
echo "#          Done with build prep            #"
echo "############################################"

exit_with_status
