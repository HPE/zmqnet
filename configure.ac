# Copyright 2011-2019,2020 Hewlett Packard Enterprise Development LP.
#
AC_PREREQ([2.69])

AC_INIT([zmqnet], m4_esyscmd([source $PWD/release_versioning; echo $major.$minor | tr -d '\n']))
AC_COPYRIGHT([Copyright 2019-2020 Hewlett Packard Enterprise Development LP.])

AC_CONFIG_SRCDIR([src/Makefile.am])
AC_CONFIG_MACRO_DIRS([m4])
AC_CONFIG_HEADERS([config.h])

AC_SUBST([ZMQNET_DIR], ["$(cd $srcdir && pwd)"])

dnl Turn on Posix extensions
AC_USE_SYSTEM_EXTENSIONS

AM_INIT_AUTOMAKE([foreign subdir-objects -Wall])

AC_LANG(C++)

###########################
### Check for programs. ###
###########################
AM_PROG_AR
AC_PROG_CXX
AC_PROG_CC
AX_CHECK_COMPILE_FLAG([-std=c++17], [
                        CXXFLAGS="$CXXFLAGS -Wall -std=c++17"])

LT_INIT

#
### Check for pkgconfig files. ###
#
PKG_CHECK_MODULES([LIBZMQ], [libzmq])

#
### Check for headers. ###
#
save_CXXFLAGS="$CXXFLAGS"
CPPZMQ_CFLAGS="-I$prefix/include"
CXXFLAGS="$CXXFLAGS $CPPZMQ_CFLAGS $LIBZMQ_CFLAGS"
AC_CHECK_HEADER([zmq.hpp],
        [],
        [AC_MSG_FAILURE([zmq.hpp not found.])],
        []
        )
dnl restore flags
CXXFLAGS="$save_CXXFLAGS"
AC_SUBST([CPPZMQ_CFLAGS], ["$CPPZMQ_CFLAGS"])

AC_CONFIG_FILES([
	libzmqnet.pc
	Makefile
	src/Makefile
])

AC_OUTPUT
