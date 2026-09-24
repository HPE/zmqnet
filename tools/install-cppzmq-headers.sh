#!/bin/sh
set -eu

install_dir="$MESON_INSTALL_DESTDIR_PREFIX/include"
mkdir -p "$install_dir"
install -m 644 "$MESON_SOURCE_ROOT/subprojects/cppzmq/zmq.hpp" "$install_dir/zmq.hpp"
install -m 644 "$MESON_SOURCE_ROOT/subprojects/cppzmq/zmq_addon.hpp" "$install_dir/zmq_addon.hpp"