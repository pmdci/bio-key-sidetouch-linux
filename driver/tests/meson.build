# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
#
# Offline tests for the SideTouch driver: synthetic or recorded non-biometric
# inputs only. Copied to tests/sidetouch/ by build.sh.

sidetouch_pure_tests = ['proto', 'format4', 'reference', 'print', 'match']

foreach name : sidetouch_pure_tests
    test('sidetouch-' + name,
        executable('test-sidetouch-' + name,
            sources: ['test-sidetouch-' + name + '.c', 'sidetouch-synth.c'],
            include_directories: sidetouch_inc,
            c_args: sidetouch_strict_args,
            link_with: [libsidetouch, libsidetouch_matcher],
            dependencies: libfprint_private_dep,
            install: false),
        suite: ['unit-tests', 'sidetouch'],
        env: envs,
        timeout: 120)
endforeach

# State machines over the scripted transport, on upstream's fake device.
sidetouch_machine_tests = ['exchange', 'bridge', 'acquire', 'open', 'device']

foreach name : sidetouch_machine_tests
    test('sidetouch-' + name,
        executable('test-sidetouch-' + name,
            sources: ['test-sidetouch-' + name + '.c', 'sidetouch-fake.c', 'sidetouch-synth.c', 'sidetouch-script.c'],
            include_directories: [sidetouch_inc, include_directories('..')],
            c_args: sidetouch_strict_args,
            link_with: [libsidetouch, libsidetouch_matcher],
            link_whole: test_utils,
            dependencies: libfprint_private_dep,
            install: false),
        suite: ['unit-tests', 'sidetouch'],
        env: envs,
        timeout: 120)
endforeach
