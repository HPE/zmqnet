#!/bin/sh
set -eu

case "$MESON_INSTALL_DESTDIR_PREFIX" in
  *"$MESON_INSTALL_PREFIX")
    destdir=${MESON_INSTALL_DESTDIR_PREFIX%"$MESON_INSTALL_PREFIX"}
    ;;
  *)
    echo "MESON_INSTALL_DESTDIR_PREFIX does not end with MESON_INSTALL_PREFIX" >&2
    exit 1
    ;;
esac

make -C "$1" install-libLTLIBRARIES \
  DESTDIR="$destdir" \
  libdir="$MESON_INSTALL_PREFIX/lib/zmqnet"

make -C "$1" install-pkgconfigDATA \
  DESTDIR="$destdir" \
  libdir="$MESON_INSTALL_PREFIX/lib/zmqnet"

pc_file="$MESON_INSTALL_DESTDIR_PREFIX/lib/zmqnet/pkgconfig/libzmq.pc"
prefix=$(printf '%s' "$MESON_INSTALL_PREFIX" | sed 's/[&|\\]/\\&/g')
sed -i \
  -e "s|^prefix=.*$|prefix=$prefix|" \
  -e 's|^libdir=.*$|libdir=${exec_prefix}/lib/zmqnet|' \
  "$pc_file"

make -C "$1" install-includeHEADERS \
  DESTDIR="$destdir" \
  includedir="$MESON_INSTALL_PREFIX/include"

