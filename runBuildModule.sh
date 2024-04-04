#!/bin/bash
#
# Copyright 2010-2021 Hewlett Packard Enterprise Development LP.
#
# This build script is used when zmqnet is being built as a dependency of another
# product. It has the ability to specify the installation directory as an input
# environment variable.
#
# e.g. in the parent build script, "install_dir=/opt/cray/pe/other ./runBuildModule.sh"

echo "############################################"
echo "#            Setup environment.            #"
echo "############################################"

top_level=$PWD

source ./external/cdst_build_library/build_lib_gcc

setup_modules

module load cray-cdst-support
check_exit_status

module load cray-cti-devel
check_exit_status

module list

# Create install directory
source $top_level/release_versioning
zmqnet_version="$major.$minor.$revision"
echo "zmqnet version: \"$zmqnet_version\""

# allow install_dir to be specified externally by products that depend on zmqnet
if [ -z "$install_dir" ]; then
    install_dir="/opt/cray/pe/zmqnet/$zmqnet_version"
fi

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

CFLAGS="-g -O2 -Wall" ./configure --prefix=$install_dir --with-libsodium=yes
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

CFLAGS="-g -O2 -Wall" PKG_CONFIG_PATH=$install_dir/lib/pkgconfig:$PKG_CONFIG_PATH cmake -DCMAKE_INSTALL_PREFIX:PATH=$install_dir -DCPPZMQ_BUILD_TESTS=OFF ..
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
echo "#           Building Cap'n Proto           #"
echo "############################################"

# ensure submodule is checked out
if [[ ! -f $top_level/external/capnproto/README.md ]]; then
    echo "capnproto submodule not checked out..."
    exit 1
fi

pushd $top_level/external/capnproto/c++
check_exit_status

autoreconf -ifv
check_exit_status

./configure --prefix=$install_dir
check_exit_status

make $cdst_j_flags
check_exit_status

make $cdst_j_flags install
check_exit_status

popd
check_exit_status

echo "############################################"
echo "#           Building LRLE Bitset           #"
echo "############################################"

# ensure submodule is checked out
if [[ ! -f $top_level/external/lrle_bitset/configure.ac ]]; then
    echo "lrle_bitset submodule not checked out..."
    exit 1
fi

pushd $top_level/external/lrle_bitset
check_exit_status

autoreconf -ifv
check_exit_status

./configure --prefix=$install_dir
check_exit_status

make $cdst_j_flags
check_exit_status

make $cdst_j_flags install
check_exit_status

popd
check_exit_status

echo "############################################"
echo "#             Building zmqnet              #"
echo "############################################"

echo $PWD
autoreconf -ifv
check_exit_status

PKG_CONFIG_PATH=$install_dir/lib/pkgconfig:$PKG_CONFIG_PATH ./configure --prefix=$install_dir --enable-install_directly_in_lib
check_exit_status

make $cdst_j_flags
check_exit_status

make $cdst_j_flags install
check_exit_status

echo "############################################"
echo "#              Done with build             #"
echo "############################################"

exit_with_status
