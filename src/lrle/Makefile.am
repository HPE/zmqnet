# Copyright 2019 Cray Inc.  All Rights Reserved.
# Unpublished Proprietary Information.
# This unpublished work is protected to trade secret, copyright and other laws.
# Except as permitted by contract or express written permission of Cray Inc.,
# no part of this work or its content may be used, reproduced or disclosed
# in any form.

AM_CXXFLAGS = -g -Wall -std=c++17 -O2 -DNDEBUG

include_HEADERS = lrle_bitset.h

lib_LTLIBRARIES = libLrleBitset.la

libLrleBitset_la_SOURCES = lrle_bitset.cpp
libLrleBitset_la_CXXFLAGS = $(AM_CXXFLAGS) -fPIC -fno-rtti -fno-exceptions

libLrleBitset_la_LINK = $(LIBTOOL) $(AM_V_lt) --tag=CC \
        $(AM_LIBTOOLFLAGS) $(LIBTOOLFLAGS) --mode=link $(CC) \
        $(CFLAGS) $(AM_LDFLAGS) \
        $(LDFLAGS) -o $@

#libLrleBitset.la: $(libLrleBitset_la_OBJECTS) $(libLrleBitset_la_DEPENDENCIES) $(EXTRA_libLrleBitset_la_DEPENDENCIES)
