# Copyright 2020 Hewlett Packard Enterprise Development LP.

SUBDIRS = libzmqnet

SRC = $(top_srcdir)/src
AM_CPPFLAGS = -I$(SRC) -I$(prefix)/include -I$(top_srcdir)/include

libexec_PROGRAMS = commnode

commnode_SOURCES = commnode.cpp
commnode_CXXFLAGS = -fPIC -fno-rtti $(AM_CXXFLAGS) $(LIBZMQ_CFLAGS) $(CAPNP_CFLAGS) $(COMMON_TOOLS_BE_CFLAGS)
commnode_LDFLAGS = libzmqnet/libzmqnet.la $(AM_LDFLAGS) $(LIBZMQ_LIBS) $(CAPNP_LIBS) $(COMMON_TOOLS_BE_LIBS)
