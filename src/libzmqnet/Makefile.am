# Copyright 2020 Hewlett Packard Enterprise Development LP.

SRC = $(top_srcdir)/src
AM_CPPFLAGS = -I$(SRC) -I$(top_srcdir)/include -I$(prefix)/include

NetworkSchema.capnp.c++: NetworkSchema.capnp
	PATH=$(prefix)/support/bin:$(PATH) capnp compile -oc++ NetworkSchema.capnp

BUILT_SOURCES = NetworkSchema.capnp.c++

lib_LTLIBRARIES = libzmqnet.la

libzmqnet_la_SOURCES = Frontend.cpp AsyncFrontend.cpp Backend.cpp NetworkSchema.capnp.c++
libzmqnet_la_CXXFLAGS = -fPIC -fno-rtti $(AM_CXXFLAGS) $(LIBZMQ_CFLAGS) $(CAPNP_CFLAGS) $(LRLE_BITSET_CFLAGS) $(COMMON_TOOLS_FE_CFLAGS) $(COMMON_TOOLS_BE_CFLAGS)
libzmqnet_la_LDFLAGS = $(AM_LDFLAGS)
libzmqnet_la_LDFLAGS += $(LIBZMQ_LIBS) $(CAPNP_LIBS) $(LRLE_BITSET_LIBS) \
	$(COMMON_TOOLS_FE_LIBS) $(COMMON_TOOLS_BE_LIBS) -luuid -version-info @ZMQNET_VERSION@

clean-local:
	rm -f NetworkSchema.capnp.*
