# Copyright 2019,2020 Hewlett Packard Enterprise Development LP.

SRC = $(top_srcdir)/src
AM_CPPFLAGS = -I$(SRC)

lib_LTLIBRARIES = libzmqnet.la

libzmqnet_la_SOURCES = libzmqnet.cpp
libzmqnet_la_CFLAGS = -fPIC -fvisibility=hidden $(AM_CFLAGS)
libzmqnet_la_CXXFLAGS = -fPIC -fvisibility=hidden -fno-rtti $(AM_CXXFLAGS)
