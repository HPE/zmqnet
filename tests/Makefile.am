# Copyright 2019,2020 Hewlett Packard Enterprise Development LP.

SRC = $(top_srcdir)/src
AM_CXXFLAGS = -Wl,-E -Wl,-rpath,$(prefix)/lib
AM_CPPFLAGS = -I$(top_srcdir)/tests

TESTS = inproc
check_PROGRAMS = inproc

ZMQNET_LIBS = `PKG_CONFIG_PATH=$(prefix)/lib/pkgconfig pkg-config libzmqnet --libs`
ZMQNET_CFLAGS = `PKG_CONFIG_PATH=$(prefix)/lib/pkgconfig pkg-config libzmqnet --cflags`

inproc_SOURCES = inproc.cpp
inproc_CXXFLAGS = $(AM_CXXFLAGS) $(ZMQNET_CFLAGS)
inproc_LDFLAGS = $(AM_LDFLAGS) $(ZMQNET_LIBS)
