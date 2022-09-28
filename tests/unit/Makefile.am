# Copyright 2020 Hewlett Packard Enterprise Development LP.

SRC = $(top_srcdir)/src
AM_CXXFLAGS = -Wl,-E -Wl,-rpath,$(prefix)/lib -std=c++17
AM_CPPFLAGS = -I$(top_srcdir)/tests/unit

TESTS = libzmqnet_unit_tests
check_PROGRAMS = libzmqnet_unit_tests

ZMQNET_LIBS = `PKG_CONFIG_PATH=$(prefix)/lib/pkgconfig pkg-config libzmqnet --libs`
ZMQNET_CFLAGS = `PKG_CONFIG_PATH=$(prefix)/lib/pkgconfig pkg-config libzmqnet --cflags`

# Googletest library support
GTEST = $(top_srcdir)/tests/support/googletest/googletest
GTEST_CFLAGS = -I$(GTEST)/include -I$(GTEST)
GTEST_LIBS = -pthread

libzmqnet_unit_tests_SOURCES = libzmqnet_unit_tests.cpp googletest/googletest/src/gtest-all.cc
libzmqnet_unit_tests_CXXFLAGS = $(AM_CXXFLAGS) $(GTEST_CFLAGS) $(ZMQNET_CFLAGS)
libzmqnet_unit_tests_LDFLAGS = $(AM_LDFLAGS) $(GTEST_LIBS) $(ZMQNET_LIBS)

