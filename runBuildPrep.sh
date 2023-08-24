#!/bin/bash
#
# runBuildPrep.sh - Preps the build environment
#
# Copyright 2019-2023 Hewlett Packard Enterprise Development LP.
#
# Unpublished Proprietary Information.
# This unpublished work is protected to trade secret, copyright and other laws.
# Except as permitted by contract or express written permission of Hewlett Packard Enterprise Development LP.,
# no part of this work or its content may be used, reproduced or disclosed
# in any form.
#

source ./external/cdst_build_library/build_lib

echo "############################################"
echo "#          Install dependencies            #"
echo "############################################"

install_common_pe
check_exit_status

install_cti
check_exit_status

target_pm=$(get_pm)

if [[ "$target_pm" == "$cdst_pm_zypper" ]]; then
    zypper --non-interactive install \
        environment-modules \
        autoconf \
        automake \
        libtool \
        binutils \
        gcc \
        glibc-devel \
        make \
        cmake \
        libuuid-devel \
        openssl-devel \
        python3-pip \
        rpm-build
    check_exit_status
elif [[ "$target_pm" == "$cdst_pm_yum" ]]; then
    yum --assumeyes install \
        environment-modules \
        autoconf \
        automake \
        libtool \
        make \
        cmake \
        libuuid-devel \
        openssl-devel \
        python3-pip \
        rpm-build \
        wget
    check_exit_status
fi

echo "############################################"
echo "#          Done with build prep            #"
echo "############################################"

capture_jenkins_build
check_exit_status

exit_with_status
