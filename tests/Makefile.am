# Copyright 2020 Hewlett Packard Enterprise Development LP.

SRC = $(top_srcdir)/src
AM_CXXFLAGS = -Wl,-E -Wl,-rpath,$(prefix)/lib
AM_CPPFLAGS = -I$(top_srcdir)/tests

TESTS = libzmqnet_unit_tests
check_PROGRAMS = topotest stop test_backend libzmqnet_unit_tests libzmqnet_function_tests

ZMQNET_LIBS = `PKG_CONFIG_PATH=$(prefix)/lib/pkgconfig pkg-config libzmqnet --libs`
ZMQNET_CFLAGS = `PKG_CONFIG_PATH=$(prefix)/lib/pkgconfig pkg-config libzmqnet --cflags`

# Googletest library support
GTEST = $(top_srcdir)/tests/googletest/googletest
GTEST_CFLAGS = -I$(GTEST)/include -I$(GTEST)
GTEST_LIBS = -pthread

stop: stop.c
	cc -g stop.c -o stop

libfilter.so: libfilter.cpp
	$(CC) $(AM_CXXFLAGS) $(ZMQNET_CFLAGS) -fPIC -shared $(AM_LDFLAGS) $(ZMQNET_LIBS) $< -o $@

topotest_SOURCES = topotest.cpp libfilter.so
topotest_CXXFLAGS = $(AM_CXXFLAGS) $(COMMON_TOOLS_FE_CFLAGS) $(COMMON_TOOLS_BE_CFLAGS) $(ZMQNET_CFLAGS)
topotest_LDFLAGS = $(AM_LDFLAGS) $(COMMON_TOOLS_FE_LIBS) $(COMMON_TOOLS_BE_LIBS) $(ZMQNET_LIBS)

test_backend_SOURCES = test_backend.cpp libfilter.so
test_backend_CXXFLAGS = $(AM_CXXFLAGS) $(ZMQNET_CFLAGS)
test_backend_LDFLAGS = $(AM_LDFLAGS) $(ZMQNET_LIBS)

libzmqnet_unit_tests_SOURCES = libzmqnet_unit_tests.cpp googletest/googletest/src/gtest-all.cc libfilter.so
libzmqnet_unit_tests_CXXFLAGS = $(AM_CXXFLAGS) $(GTEST_CFLAGS) $(ZMQNET_CFLAGS)
libzmqnet_unit_tests_LDFLAGS = $(AM_LDFLAGS) $(GTEST_LIBS) $(ZMQNET_LIBS)

libzmqnet_function_tests_SOURCES = libzmqnet_function_tests.cpp googletest/googletest/src/gtest-all.cc libfilter.so
libzmqnet_function_tests_CXXFLAGS = $(AM_CXXFLAGS) $(GTEST_CFLAGS) $(COMMON_TOOLS_FE_CFLAGS) $(ZMQNET_CFLAGS)
libzmqnet_function_tests_LDFLAGS = $(AM_LDFLAGS) $(GTEST_LIBS) $(COMMON_TOOLS_FE_LIBS) $(ZMQNET_LIBS)
