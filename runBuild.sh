#!/bin/bash
#
# Copyright 2010-2022 Hewlett Packard Enterprise Development LP.
#

echo "############################################"
echo "#            Setup environment.            #"
echo "############################################"

top_level=$PWD

source ./external/cdst_build_library/build_lib_gcc

# Clean environment on Jenkins
if [[ ! -z $BRANCH_NAME ]]; then
    echo "Cleaning build env..."
    rm -rf /opt/cray/pe/zmqnet
fi

setup_modules

module load cray-cdst-support
check_exit_status

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
echo "#               Building libsodium         #"
echo "############################################"

# ensure submodule is checked out
if [ ! -f $top_level/external/libsodium/Makefile.am ]
then
    echo "libsodium submodule not checked out..."
    exit 1
fi

pushd $top_level/external/libsodium
check_exit_status

autoreconf -ifv
check_exit_status

CFLAGS="-g -O2 -Wall" ./configure --prefix=$support_dir --enable-soname-versions=no
check_exit_status

make $cdst_j_flags
check_exit_status

make $cdst_j_flags install
check_exit_status

# Rename libsodium to libzmqnet_libsodium in libtool
sed -i 's/libsodium.so/libzmqnet_libsodium.so/g' $support_dir/lib/libsodium.la
sed -i 's/libsodium.a/libzmqnet_libsodium.a/g' $support_dir/lib/libsodium.la

# Rename libsodium to libzmqnet_libsodium in pkgconfig
sed -i 's/lsodium/lzmqnet_libsodium/g' $support_dir/lib/pkgconfig/libsodium.pc

# Rename shared libraries libsodium to libzmq_libsodium
type -P patchelf || pip install patchelf
for lib in $support_dir/lib/libsodium.*; do
    if [ -L "$lib" ]; then
        fixed_target=$(readlink $lib)
        if [[ "$fixed_target" != "libzmqnet_*" ]]; then
            fixed_target=$(dirname $fixed_target)/libzmqnet_$(basename $fixed_target)
        fi
        fixed_source=$(dirname $lib)/libzmqnet_$(basename $lib)
        rm $lib $fixed_source
        ln -s $fixed_target $fixed_source

    else
        fixed_lib=$(dirname $lib)/libzmqnet_$(basename $lib)

        # Fix soname
        soname=$(objdump -p $lib 2>/dev/null | grep SONAME | tr -s ' ' | cut -d ' ' -f 3)
        if [ ! -z "$soname" ]; then
            fixed_soname="libzmqnet_$soname"
            patchelf --set-soname $fixed_soname $lib
            check_exit_status
        fi

        mv $lib $fixed_lib
    fi
    check_exit_status
done

popd
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

PKG_CONFIG_PATH="$support_dir/lib/pkgconfig:$PKG_CONFIG_PATH" CFLAGS="-g -O2 -Wall" \
    ./configure --prefix=$support_dir --with-libsodium=yes
check_exit_status

make $cdst_j_flags
check_exit_status

make $cdst_j_flags install
check_exit_status

# Rename libzmq to libzmqnet_libzmq in libtool
sed -i 's/libzmq.so/libzmqnet_libzmq.so/g' $support_dir/lib/libzmq.la
sed -i 's/libzmq.a/libzmqnet_libzmq.a/g' $support_dir/lib/libzmq.la

# Rename libzmq to libzmqnet_libzmq in pkgconfig
sed -i 's/lzmq/lzmqnet_libzmq/g' $support_dir/lib/pkgconfig/libzmq.pc

# Rename shared libraries libzmq to libzmqnet_libzmq
type -P patchelf || pip install patchelf
for lib in $support_dir/lib/libzmq.*; do
    if [ -L "$lib" ]; then
        fixed_target=$(readlink $lib)
        if [[ "$fixed_target" != "libzmqnet_*" ]]; then
            fixed_target=$(dirname $fixed_target)/libzmqnet_$(basename $fixed_target)
        fi
        fixed_source=$(dirname $lib)/libzmqnet_$(basename $lib)
        rm $lib $fixed_source
        ln -s $fixed_target $fixed_source

    else
        fixed_lib=$(dirname $lib)/libzmqnet_$(basename $lib)

        # Fix soname
        soname=$(objdump -p $lib 2>/dev/null | grep SONAME | tr -s ' ' | cut -d ' ' -f 3)
        if [ ! -z "$soname" ]; then
            fixed_soname="libzmqnet_$soname"
            patchelf --set-soname $fixed_soname $lib
            check_exit_status
        fi

        mv $lib $fixed_lib
    fi
    check_exit_status
done

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

CFLAGS="-g -O2 -Wall" PKG_CONFIG_PATH=$support_dir/lib/pkgconfig:$PKG_CONFIG_PATH cmake -DCMAKE_INSTALL_PREFIX:PATH=$support_dir -DCPPZMQ_BUILD_TESTS=OFF ..
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

./configure --prefix=$support_dir
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

./configure --prefix=$support_dir
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

PKG_CONFIG_PATH=$support_dir/lib/pkgconfig:$PKG_CONFIG_PATH ./configure --prefix=$install_dir
check_exit_status

make $cdst_j_flags
check_exit_status

make $cdst_j_flags install
check_exit_status

echo "############################################"
echo "#              Done with build             #"
echo "############################################"

exit_with_status
