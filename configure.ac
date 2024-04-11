# Copyright 2020 Hewlett Packard Enterprise Development LP.
#
AC_PREREQ([2.69])

AC_INIT([zmqnet], m4_esyscmd([source $PWD/release_versioning; echo $major.$minor | tr -d '\n']))
AC_COPYRIGHT([Copyright 2020 Hewlett Packard Enterprise Development LP.])

AC_SUBST([ZMQNET_VERSION], m4_esyscmd([source $PWD/release_versioning; echo $current:$revision:$age | tr -d '\n']))

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
PKG_CHECK_MODULES([CAPNP], [capnp])
PKG_CHECK_MODULES([LRLE_BITSET], [libLrleBitset])
PKG_CHECK_MODULES([COMMON_TOOLS_FE], [common_tools_fe])
PKG_CHECK_MODULES([COMMON_TOOLS_BE], [common_tools_be])

#
### Check for libraries. ###
#
AC_CHECK_LIB(dl, dlopen)
AC_CHECK_LIB(dl, dlclose)

### Select whether encryption will be available at runtime
AC_ARG_ENABLE([encryption],
  [AS_HELP_STRING([--enable-encryption], [Build encryption support into ZMQNet])],,
  [enable_encryption=yes])
# Strip whitespace and verify yes / no
enable_encryption=`echo $enable_encryption`
case $enable_encryption in
  yes | no) ;;
  *) AC_MSG_ERROR([Unknown option '$enable_encryption' for --enable-encryption, expected 'yes' or 'no']) ;;
esac
AM_CONDITIONAL([ENABLE_ENCRYPTION], [test x$enable_encryption = xyes])

AM_COND_IF([ENABLE_ENCRYPTION],
  [AC_DEFINE( [HAVE_ENCRYPTION], [1], [Define to 1 if encryption enabled.])],
  [AC_DEFINE( [HAVE_ENCRYPTION], [0], [Define to 0 if encryption disabled.])]
)

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

# Config header defines
AC_SUBST([LIBZMQNET_PREFIX], ["$prefix"])
AC_DEFINE_UNQUOTED([LIBZMQNET_PREFIX], ["$LIBZMQNET_PREFIX"], [Library installation location])

# --enable-install-directly-in-lib=true will cause zmqnet and all of its
# components to be installed in the /lib directory of its install prefix.
# otherwise, its supporting components (capnp, libzmq, etc) will get their own
# /support directory in the install prefix.
#
# XXX: when all products use standalone zmqnet, this can be removed.
AC_ARG_ENABLE([install-directly-in-lib],
[  --enable-install-directly-in-lib   Install all components in /lib instead of a /support directory],
[case "${enableval}" in
  yes) install_directly_in_lib=true ;;
  no)  install_directly_in_lib=false ;;
  *) AC_MSG_ERROR([bad value ${enableval} for --enable-install-directly-in-lib]) ;;
esac],[install_directly_in_lib=false])
AM_CONDITIONAL([INSTALL_DIRECTLY_IN_LIB], [test x$install_directly_in_lib = xtrue])

AC_CONFIG_FILES([
	libzmqnet.pc
	Makefile
	src/Makefile
	src/libzmqnet/Makefile
	tests/unit/Makefile
	doc/Makefile
])

AC_OUTPUT
