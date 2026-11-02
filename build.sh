#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
#
# Builds libfprint v1.94.100 with the BIO-key SideTouch driver overlaid, and
# runs the driver's offline tests. Needs no privileges.
#
#   ./build.sh BUILD_DIR
#
# BUILD_DIR/libfprint  a clone of the pinned tag, owned by this script: it is
#                      reset to the pinned commit on every run, discarding any
#                      change made inside it
# BUILD_DIR/build      the meson build directory
#
# Build dependencies are upstream libfprint's own (meson, ninja, a C compiler,
# GLib, gusb, pixman, and gudev for the default set's SPI driver).
#
# DRIVERS (environment, default "default") is passed to -Ddrivers. The default
# set makes the result a superset of a distribution's library; a narrower set
# is for development only. It is read when BUILD_DIR/build is first set up.
set -eu

TAG=v1.94.100
COMMIT=80a4b5ec612892c5c056c48dddaa561452cf37ec
UPSTREAM=https://gitlab.freedesktop.org/libfprint/libfprint.git

if [ "$#" -ne 1 ]; then
  echo "usage: $0 BUILD_DIR" >&2
  exit 2
fi

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
case $1 in
  /*) out=$1 ;;
  *) out=$PWD/$1 ;;
esac
case $out/ in
  "$here"/*) echo "BUILD_DIR must be outside the source tree" >&2; exit 2 ;;
esac
mkdir -p "$out"
src=$out/libfprint
build=$out/build

if [ ! -d "$src/.git" ]; then
  git clone --quiet --depth 1 --branch "$TAG" "$UPSTREAM" "$src"
fi
head=$(git -C "$src" rev-parse HEAD)
if [ "$head" != "$COMMIT" ]; then
  echo "libfprint at $head, expected $TAG = $COMMIT" >&2
  exit 1
fi
git -C "$src" reset --quiet --hard "$COMMIT"
git -C "$src" clean --quiet -fdx

git -C "$src" apply "$here/overlay/libfprint-v1.94.100.patch"

drv=$src/libfprint/drivers/sidetouch
tst=$src/tests/sidetouch
mkdir -p "$drv" "$tst"
cp "$here"/driver/*.c "$here"/driver/*.h "$here"/driver/meson.build "$drv"/
for f in kpcand sift kpmatch kptemplate; do
  cp "$here/matcher/$f.c" "$here/matcher/$f.h" "$drv"/
done
cp "$here"/driver/tests/*.c "$here"/driver/tests/*.h "$here"/driver/tests/meson.build "$tst"/

if [ ! -f "$build/build.ninja" ]; then
  meson setup "$build" "$src" \
    -Ddrivers="${DRIVERS:-default}" -Dintrospection=false -Ddoc=false -Dinstalled-tests=false \
    -Dudev_rules=disabled -Dudev_hwdb=disabled -Dgtk-examples=false
fi
ninja -C "$build"
meson test -C "$build" --suite sidetouch --print-errorlogs
