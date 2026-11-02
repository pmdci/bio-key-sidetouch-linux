#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
#
# Installs the libfprint built by build.sh, with the SideTouch driver, for
# fprintd only: the library goes to a private prefix, and one systemd drop-in
# points fprintd at it. Nothing the distribution's package manager owns is
# touched; other programs keep the distribution's libfprint.
#
#   sudo ./install.sh BUILD_DIR       install, then restart fprintd if running
#   sudo ./install.sh --uninstall     remove the drop-in and the prefix
#
# Options (before the arguments):
#   --dry-run    print every action instead of performing it (no root needed)
#   --root DIR   install under DIR instead of /, and skip systemctl (staging
#                and tests; no root needed)
set -eu

TAG=v1.94.100
COMMIT=80a4b5ec612892c5c056c48dddaa561452cf37ec
PREFIX=/usr/local/lib/sidetouch-libfprint
DROPIN_DIR=/etc/systemd/system/fprintd.service.d
DROPIN_NAME=50-sidetouch-libfprint.conf
LIBRARY=libfprint-2.so.2.0.0
SONAME=libfprint-2.so.2
# what marks files as this installer's own; nothing without it is replaced or removed
MARKER='# Installed by the SideTouch driver install.sh; remove with install.sh --uninstall.'
OWNED=.sidetouch-install

dry=
root=
uninstall=
while [ "$#" -gt 0 ]; do
  case $1 in
    --dry-run) dry=1; shift ;;
    --root) root=${2:?--root needs a directory}; shift 2 ;;
    --uninstall) uninstall=1; shift ;;
    --) shift; break ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) break ;;
  esac
done

run() {
  if [ -n "$dry" ]; then
    echo "would run: $*"
  else
    "$@"
  fi
}

systemd() {
  if [ -n "$root" ]; then
    echo "skipped under --root: systemctl $*"
  else
    run systemctl "$@"
  fi
}

prefix=$root$PREFIX
dropin_dir=$root$DROPIN_DIR
dropin=$dropin_dir/$DROPIN_NAME

# present counts a symlink, dangling or not; only a real file or directory
# carrying the marker is this installer's own
present() { [ -e "$1" ] || [ -L "$1" ]; }
ours_dropin() { [ ! -L "$dropin" ] && [ -f "$dropin" ] && [ "$(head -n 1 "$dropin")" = "$MARKER" ]; }
ours_prefix() { [ ! -L "$prefix" ] && [ -d "$prefix" ] && [ ! -L "$prefix/$OWNED" ] && [ -f "$prefix/$OWNED" ]; }

if [ -z "$dry" ] && [ -z "$root" ] && [ "$(id -u)" -ne 0 ]; then
  echo "install.sh changes /etc and /usr/local and restarts fprintd; run it as root:" >&2
  echo "  sudo $0 $*" >&2
  exit 1
fi

if [ -n "$uninstall" ]; then
  [ "$#" -eq 0 ] || { echo "usage: $0 [--dry-run] [--root DIR] --uninstall" >&2; exit 2; }
  if present "$dropin" && ! ours_dropin; then
    echo "$dropin was not created by this installer; leaving it" >&2
  elif present "$dropin"; then
    run rm -f -- "$dropin"
    [ -d "$dropin_dir" ] && run rmdir --ignore-fail-on-non-empty -- "$dropin_dir"
  fi
  if present "$prefix" && ! ours_prefix; then
    echo "$prefix was not created by this installer; leaving it" >&2
  elif present "$prefix"; then
    run rm -f -- "$prefix/lib/$SONAME" "$prefix/lib/$LIBRARY" "$prefix/$OWNED"
    run rmdir -- "$prefix/lib" "$prefix"
  fi
  systemd daemon-reload
  systemd try-restart fprintd.service
  if [ -n "$dry" ]; then
    echo "dry run: nothing was changed"
  else
    echo "uninstalled; fprintd uses the distribution's libfprint again"
  fi
  exit 0
fi

[ "$#" -eq 1 ] || { echo "usage: $0 [--dry-run] [--root DIR] BUILD_DIR" >&2; exit 2; }
build_dir=$1
built=$build_dir/build/libfprint/$LIBRARY

# the library must come from build.sh at the pinned commit, with the driver
[ -f "$built" ] || { echo "no $built: run build.sh $build_dir first" >&2; exit 1; }
head=$(git -C "$build_dir/libfprint" rev-parse HEAD 2>/dev/null || echo none)
[ "$head" = "$COMMIT" ] || { echo "$build_dir/libfprint is at $head, not $TAG ($COMMIT)" >&2; exit 1; }
"$build_dir/build/libfprint/fprint-list-supported-devices" 2>/dev/null | grep -q '2963:2001' ||
  { echo "the built library does not list the SideTouch reader" >&2; exit 1; }

# another drop-in pointing fprintd at a different library would conflict:
# on a real system ask systemd for fprintd's effective environment (which
# covers /etc, /run and /usr drop-ins); when staging, scan the staged directory
if [ -z "$root" ]; then
  env=$(systemctl show -p Environment --value fprintd.service 2>/dev/null || true)
  case " $env " in
    *" LD_LIBRARY_PATH=$PREFIX/lib "*) ;;
    *LD_LIBRARY_PATH=*)
      echo "fprintd already runs with $env" >&2
      echo "(probably another custom libfprint build). Remove or disable that drop-in first;" >&2
      echo "nothing was changed." >&2
      exit 1 ;;
  esac
fi
if present "$dropin" && ! ours_dropin; then
  echo "$dropin exists and was not created by this installer; nothing was changed" >&2
  exit 1
fi
if present "$prefix" && ! ours_prefix; then
  echo "$prefix exists and was not created by this installer; nothing was changed" >&2
  exit 1
fi
for f in "$dropin_dir"/*.conf; do
  [ -e "$f" ] || continue
  [ "$f" = "$dropin" ] && continue
  if grep -q 'LD_LIBRARY_PATH' "$f"; then
    echo "$f already sets LD_LIBRARY_PATH for fprintd (probably another custom" >&2
    echo "libfprint build). Remove or disable it first; nothing was changed." >&2
    exit 1
  fi
done

[ -n "$dry" ] || trap 'rm -f -- "$prefix/lib/.$LIBRARY.new"' EXIT
# the working library stays in place until its replacement is complete:
# copy beside it, then rename over it (atomic within one directory)
run install -d -m 0755 -- "$prefix/lib"
run touch -- "$prefix/$OWNED"
run install -m 0755 -- "$built" "$prefix/lib/.$LIBRARY.new"
run mv -f -- "$prefix/lib/.$LIBRARY.new" "$prefix/lib/$LIBRARY"
run ln -sfn -- "$LIBRARY" "$prefix/lib/$SONAME"
run install -d -m 0755 -- "$dropin_dir"
if [ -n "$dry" ]; then
  echo "would write $dropin:"
  echo "  [Service]"
  echo "  Environment=LD_LIBRARY_PATH=$PREFIX/lib"
else
  tmp=$(mktemp "$dropin_dir/.sidetouch.XXXXXX")
  printf '%s\n[Service]\nEnvironment=LD_LIBRARY_PATH=%s/lib\n' "$MARKER" "$PREFIX" > "$tmp"
  chmod 0644 "$tmp"
  mv -f -- "$tmp" "$dropin"
fi
systemd daemon-reload
systemd try-restart fprintd.service
if [ -n "$dry" ]; then
  echo "dry run: nothing was changed"
else
  echo "installed: fprintd now loads $PREFIX/lib/$SONAME"
fi
