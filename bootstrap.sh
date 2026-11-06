#!/bin/bash

# Driver script for installing the prerequisites for CTI.
#
# sudo ./bootstrap.sh --prep
#   installs the build requirments from the OS
# install_dir=[prefix] ./boostrap.sh
#    will build is and install the package

set -e

install_dir="${install_dir=${HOME}/hpe-tools}"

reconfigure=
prep=
for arg in "$@"; do
    if [ $arg == "--reconfigure" ]; then
	reconfigure='--reconfigure'
    elif [ $arg == "--prep" ]; then
	prep=1
    else
	echo "usage $0 [--prep|--reconfigure]"
	exit 1
    fi
done

if [ -n "$prep" ]; then
    if [ "$(whoami)" != "root" ]; then
	echo "--prep must be run as root"
	exit 1
    fi
    echo prepping
    if [ -n "$(command -v dnf)" ]; then
	# some of the requirements need epel packages
	# sudo dnf config-manager --add-repo=https://arti.hpc.amslabs.hpecorp.net:443/artifactory/mirror-epel/epel/9/Everything/x86_64/
	# needed by cti
	dnf install gcc-toolset-14 meson cmake libssh2-devel boost-devel dyninst-devel pandoc

	# local to zmqnet
	dnf install libsodium-devel libuuid-devel
    elif [ -n "$(command -v zypper)" ]; then
	# needed by CTI
	zypper install gcc14-c++ meson cmake libssh2-devel libboost_headers1_66_0-devel

	# specific to zmqnet
	zypper install libsodium-devel libuuid-devel
    else
	echo "could find neither dnf nor zypper to install"
	exit 1
    fi
    exit 0
fi

if [ $(gcc --version | grep -c '\<14\.') != 1 ]; then
    source /opt/rh/gcc-toolset-14/enable
fi    

top_level="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
libdir="$install_dir/lib/zmqnet"

echo "############################################"
echo "#             Building zmqnet              #"
echo "############################################"

# we explicitly set CC/CXX here to their full paths so that the full paths are
# baked into the meson build configuration and meson doesn't use whatever gcc/g++ is
# in PATH on subsequent builds outside of runBuild.sh

export PKG_CONFIG_PATH="$install_dir/lib/zmqnet/pkgconfig:$install_dir/lib/cti/pkgconfig:$PKG_CONFIG_PATH"
echo PKG_CONFIG_PATH is $PKG_CONFIG_PATH
export PATH="$install_dir/bin:$PATH"

CC=$(which gcc) CXX=$(which g++) \
  meson setup \
    -Dprefix=$install_dir \
    -Dlibdir=lib/zmqnet \
    -Ddebug=true \
    -Doptimization=2 \
    -Db_ndebug=true \
    $reconfigure \
    ./build --force-fallback-for=cppzmq

if [ -z "$reconfigure" ]; then
   meson compile --verbose -C build

   meson install -C build
fi   

echo "############################################"
echo "#              Done with build             #"
echo "############################################"

