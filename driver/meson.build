# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
#
# BIO-key SideTouch (2963:2001), built inside the pinned libfprint tree as two
# static libraries linked into fprint-drivers, so that stricter warnings, and
# the matcher's arithmetic flags, apply to this driver's code only.
#
# Copied to libfprint/drivers/sidetouch/ by build.sh, with the matcher sources.

sidetouch_strict_args = cc.get_supported_arguments([
    '-Werror',
    '-Wextra',
    '-Wconversion',
])

sidetouch_inc = include_directories('.', '../..')

# The frozen keypoint candidate. No fused multiply-adds, so that a
# distribution's target flags cannot change its arithmetic.
libsidetouch_matcher = static_library('sidetouch-matcher',
    sources: files('kpcand.c', 'sift.c', 'kpmatch.c', 'kptemplate.c'),
    c_args: sidetouch_strict_args + ['-ffp-contract=off'],
    dependencies: mathlib_dep,
    install: false)

libsidetouch = static_library('sidetouch',
    sources: files(
        'sidetouch-proto.c',
        'sidetouch-format4.c',
        'sidetouch-reference.c',
        'sidetouch-print.c',
        'sidetouch-match.c',
        'sidetouch-exchange.c',
        'sidetouch-bridge.c',
        'sidetouch-acquire.c',
        'sidetouch-open.c',
        'sidetouch-usb.c',
        'sidetouch.c',
    ),
    include_directories: sidetouch_inc,
    c_args: sidetouch_strict_args,
    dependencies: deps,
    link_with: [libfprint_private, libsidetouch_matcher],
    install: false)
