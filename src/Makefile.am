# Copyright 2019,2020 Hewlett Packard Enterprise Development LP.

SRC = $(top_srcdir)/src
AM_CPPFLAGS = -I$(SRC) -I$(prefix)/include -I$(top_srcdir)/include

lib_LTLIBRARIES = libzmqnet.la

libzmqnet_la_SOURCES = libzmqnet.cpp
libzmqnet_la_CXXFLAGS = -fPIC -fvisibility=hidden -fno-rtti $(AM_CXXFLAGS) $(COMMON_TOOLS_FE_CFLAGS)
libzmqnet_la_LDFLAGS = $(AM_LDFLAGS) $(COMMON_TOOLS_FE_LIBS)
