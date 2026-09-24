#!/bin/bash
#
# Copyright 2010-2022 Hewlett Packard Enterprise Development LP.
#

echo "############################################"
echo "#            Setup environment.            #"
echo "############################################"

hpe="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
top_level="$hpe/.."

source "$hpe/external/cdst_build_library/build_lib_gcc"

# Clean environment on Jenkins
if [[ ! -z $BRANCH_NAME ]]; then
    echo "Cleaning build env..."
    rm -rf /opt/cray/pe/zmqnet
fi

setup_modules

module load cray-cti-devel
check_exit_status

# Create install directory
source $top_level/release_versioning
branch_name=$(get_branch_info)
branch_type=$(echo "$branch_name" | cut -d'/' -f1)
if [ "$branch_type" != "release" ]; then
  zmqnet_version="$major.$minor.$revision.$build_number"
else
  zmqnet_version="$major.$minor.$revision"
fi
echo "zmqnet version: \"$zmqnet_version\""
install_dir="/opt/cray/pe/zmqnet/$zmqnet_version"
support_dir="$install_dir/support"
echo "Install dir: \"$install_dir\""
mkdir -p $install_dir
check_exit_status

echo "############################################"
echo "#      Copying subprojects                  "
echo "############################################"

"$hpe/wrap.sh" libzmq cppzmq capnproto googletest
check_exit_status

echo "############################################"
echo "#             Building zmqnet              #"
echo "############################################"

cd $top_level
echo $PWD

export PKG_CONFIG_PATH="$install_dir/lib/pkgconfig:$PKG_CONFIG_PATH"
check_exit_status

CC=$(which gcc) CXX=$(which g++) \
  meson setup \
    -Dprefix=$install_dir \
    -Dlibdir=lib \
    -Ddebug=true \
    -Doptimization=2 \
    -Db_ndebug=true \
    ./build --force-fallback-for=libzmq,cppzmq
check_exit_status

meson compile --verbose -C build
check_exit_status

meson install -C build
check_exit_status

echo "############################################"
echo "#              Done with build             #"
echo "############################################"

exit_with_status
