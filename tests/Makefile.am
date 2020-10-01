# Copyright 2019,2020 Hewlett Packard Enterprise Development LP.

SRC = $(top_srcdir)/src
AM_CXXFLAGS = -Wl,-E -Wl,-rpath,$(prefix)/lib
AM_CPPFLAGS = -I$(top_srcdir)/tests

TESTS = inproc
check_PROGRAMS = inproc topotest stop

BUILT_SOURCES = libfilter.so

ZMQNET_LIBS = `PKG_CONFIG_PATH=$(prefix)/lib/pkgconfig pkg-config libzmqnet --libs`
ZMQNET_CFLAGS = `PKG_CONFIG_PATH=$(prefix)/lib/pkgconfig pkg-config libzmqnet --cflags`

inproc_SOURCES = inproc.cpp
inproc_CXXFLAGS = $(AM_CXXFLAGS) $(ZMQNET_CFLAGS)
inproc_LDFLAGS = $(AM_LDFLAGS) $(ZMQNET_LIBS)

stop_SOURCES = stop

libfilter.so: libfilter.cpp
	$(CC) $(AM_CXXFLAGS) -fPIC -shared $(AM_LDFLAGS) $< -o $@

topotest_SOURCES = topotest.cpp
topotest_CXXFLAGS = $(AM_CXXFLAGS) $(COMMON_TOOLS_FE_CFLAGS) $(COMMON_TOOLS_BE_CFLAGS) $(ZMQNET_CFLAGS)
topotest_LDFLAGS = $(AM_LDFLAGS) $(COMMON_TOOLS_FE_LIBS) $(COMMON_TOOLS_BE_LIBS) $(ZMQNET_LIBS)
