#!/bin/bash
#
# Copyright 2010-2020 Hewlett Packard Enterprise Development LP.
#

echo "############################################"
echo "#            Setup environment.            #"
echo "############################################"

top_level=$PWD

source ./external/cdst_build_library/build_lib

setup_modules

module load cray-cdst-support
check_exit_status

module load cray-cti-devel
check_exit_status

# Create install directory
source $top_level/release_versioning
zmqnet_version="$major.$minor.$revision"
echo "zmqnet version: \"$zmqnet_version\""
install_dir="/opt/cray/pe/zmqnet/$zmqnet_version"
echo "Install dir: \"$install_dir\""
mkdir -p $install_dir
check_exit_status

echo "############################################"
echo "#               Building libzmq            #"
echo "############################################"

# ensure submodule is checked out
if [ ! -f $top_level/external/libzmq/README.md ]
then
    echo "libzmq submodule not checked out..."
    exit 1
fi

pushd $top_level/external/libzmq
check_exit_status

autoreconf -ifv
check_exit_status

CFLAGS="-g -O2 -Wall" ./configure --prefix=$install_dir
check_exit_status

make $cdst_j_flags
check_exit_status

make $cdst_j_flags install
check_exit_status

popd
check_exit_status

echo "############################################"
echo "#               Installing cppzmq          #"
echo "############################################"

# ensure submodule is checked out
if [ ! -f $top_level/external/cppzmq/README.md ]
then
    echo "cppzmq submodule not checked out..."
    exit 1
fi

pushd $top_level/external/cppzmq
check_exit_status

rm -rf build
check_exit_status
mkdir build
check_exit_status
pushd build
check_exit_status

CFLAGS="-g -O2 -Wall" PKG_CONFIG_PATH=$install_dir/lib/pkgconfig:$PKG_CONFIG_PATH cmake -DCMAKE_INSTALL_PREFIX:PATH=$install_dir ..
check_exit_status

make $cdst_j_flags
check_exit_status

make $cdst_j_flags install
check_exit_status

popd
check_exit_status
popd
check_exit_status

echo "############################################"
echo "#             Building zmqnet              #"
echo "############################################"

echo $PWD
autoreconf -ifv
check_exit_status

PKG_CONFIG_PATH=$install_dir/lib/pkgconfig ./configure --prefix=$install_dir
check_exit_status

make $cdst_j_flags
check_exit_status

make $cdst_j_flags install
check_exit_status

echo "############################################"
echo "#              Done with build             #"
echo "############################################"

exit_with_status
