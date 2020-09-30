# Copyright 2019,2020 Hewlett Packard Enterprise Development LP.

SUBDIRS = libzmqnet

SRC = $(top_srcdir)/src
AM_CPPFLAGS = -I$(SRC) -I$(prefix)/include -I$(top_srcdir)/include

libexec_PROGRAMS = commnode

commnode_SOURCES = commnode.cpp
commnode_LDFLAGS = libzmqnet/libzmqnet.la
