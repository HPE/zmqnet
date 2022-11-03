# Copyright 2022 Hewlett Packard Enterprise Development LP.

# Requirements:
# - load cray-zmqnet-devel
# - load cray-cti-devel
# - export LIBZMQNET_INSTALL_DIR
#   - (you can do this by loading cray-zmqnet)
#
# Use via "make -f Makefile.manual"

CXX = g++
CXXFLAGS = -g -O0 -Wl,-E -Wl,-rpath,$(LIBZMQNET_INSTALL_DIR)/lib -std=c++17 -I. -I.. -I../include -I../external/cppzmq -I../external/libzmq/include -I../external/capnproto/c++/src

COMMON_TOOLS_FE_CFLAGS := $(shell pkg-config common_tools_fe --cflags)
COMMON_TOOLS_FE_LIBS := $(shell pkg-config common_tools_fe --libs)
COMMON_TOOLS_BE_CFLAGS := $(shell pkg-config common_tools_be --cflags)
COMMON_TOOLS_BE_LIBS := $(shell pkg-config common_tools_be --libs)

ZMQNET_CFLAGS := $(shell pkg-config libzmqnet --cflags)
ZMQNET_LIBS := $(shell pkg-config libzmqnet --libs)

# Googletest library support
GTEST = $(PWD)/googletest/googletest
GTEST_CFLAGS = -I$(GTEST)/include -I$(GTEST)
GTEST_LIBS = -pthread

MPI_BINARIES = stop
NON_MPI_BINARIES = topotest test_backend libzmqnet_function_tests libfilter.so

all:
	@echo Use \"make non-mpi\" or \"make mpi\"

non-mpi: $(NON_MPI_BINARIES)

mpi: $(MPI_BINARIES)

stop: stop.c
	cc -g stop.c -o stop

libfilter.so: libfilter.cpp
	$(CXX) $(CXXFLAGS) $(ZMQNET_CFLAGS) -fPIC -shared $(ZMQNET_LIBS) $< -o $@

topotest: topotest.cpp
	$(CXX) $(CXXFLAGS) $(COMMON_TOOLS_FE_CFLAGS) $(COMMON_TOOLS_BE_CFLAGS) $(ZMQNET_CFLAGS) $(ZMQNET_LIBS) $(COMMON_TOOLS_FE_LIBS) $(COMMON_TOOLS_BE_LIBS) $< -o $@

test_backend: test_backend.cpp
	$(CXX) $(CXXFLAGS) $(ZMQNET_CFLAGS) $(ZMQNET_LIBS) $< -o $@

libzmqnet_function_tests: libzmqnet_function_tests.cpp googletest/googletest/src/gtest-all.cc
	$(CXX) $(CXXFLAGS) $(GTEST_CFLAGS) $(ZMQNET_CFLAGS) $(COMMON_TOOLS_FE_CFLAGS) $(GTEST_LIBS) $(ZMQNET_LIBS) $(COMMON_TOOLS_FE_LIBS) $^ -o $@

clean:
	rm -f $(MPI_BINARIES) $(NON_MPI_BINARIES)
