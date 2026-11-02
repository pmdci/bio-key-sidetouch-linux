# AGENTS.md

This file provides guidance to AI/LLM agents when working with code in this repository.

A libfprint driver for the BIO-key SideTouch reader (`2963:2001`) plus a keypoint matcher (SIFT + SIGFM-style verification). It is not a standalone project: `build.sh` grafts it onto a pinned libfprint `v1.94.100` clone and builds the whole library.

## Commands

The driver build needs Linux (gusb, gudev, pixman, GLib, meson, ninja); on a macOS host, run it in an OrbStack Linux machine. On Windows, run it on WSL. The matcher builds anywhere with a C11 compiler.

```sh
./build.sh BUILD_DIR                          # clone/reset libfprint, apply overlay, copy sources in, build, run sidetouch suite
DRIVERS=sidetouch ./build.sh BUILD_DIR        # faster dev build; DRIVERS is read only when BUILD_DIR/build is first set up
meson test -C BUILD_DIR/build sidetouch-acquire --print-errorlogs   # one driver test (names: sidetouch-<module>)
make -C matcher BUILD=DIR test                # all matcher tests
make -C matcher BUILD=DIR DIR/test-sift && DIR/test-sift            # one matcher test
sudo ./install.sh --dry-run BUILD_DIR         # or --root DIR to stage without root/systemctl
```

`BUILD_DIR` (and the matcher's `BUILD`) must be outside this tree. Edit sources here, never in `BUILD_DIR/libfprint`: `build.sh` hard-resets and cleans that clone every run, then re-copies `driver/`, `driver/tests/` and the four matcher sources into `libfprint/drivers/sidetouch/` and `tests/sidetouch/`. So after any edit, re-run `build.sh`; `ninja`/`meson test` alone runs stale copies.

A new source or test file must be added to `driver/meson.build` or `driver/tests/meson.build` (and, for a new matcher file, to the copy loop in `build.sh` and to `matcher/Makefile`).

## Architecture

Layering, bottom up. Each header's opening comment is the spec for its module (wire sequences, status policies, error semantics); read it before changing the `.c`.

- **Pure modules, no I/O**: `sidetouch-proto` (serial-bridge framing, outer wrapper, Eagle frames with CRC), `sidetouch-format4` (decodes the 13,696-byte acquisition into a 115x113 u16 grid), `sidetouch-reference` (per-print background reference and its G2/G3 guards), `sidetouch-print` (`STPR` serialisation), `sidetouch-match` (adapter over the matcher).
- **Async state machines** (`FpiSsm`, non-blocking, cancellable): `sidetouch-bridge` (serial-bridge init) → `sidetouch-exchange` (one Eagle request/response) → `sidetouch-open` (claim, bridge, firmware query) and `sidetouch-acquire` (one touch or untouched acquisition).
- **Transport injection**: no state machine calls GUsb. They take a `SidetouchTransport` (`sidetouch-transport.h`); `sidetouch-usb` implements it over `FpiUsbTransfer`, `driver/tests/sidetouch-fake` implements it from a script. `fpi_device_sidetouch_set_transport` swaps both transport and interface ops so tests run the real device class on a virtual device.
- **Device**: `sidetouch.c` owns the lifecycle — enrol (5 touches, 1.5 s pause, 3 untouched frames for the reference, up to 3 attempts), verify/identify (settings check, per-print reference subtraction, score ≥ 7), sequence-number tracking, `reader_unknown` after a post-contact failure (forces reconnect), suspend/cancel.
- **Matcher** (`matcher/`): `sift` → `kpmatch` → `kpcand` (prepare/extract/compare) → `kptemplate` (`SKPT` template, enrol/verify). Treated as a **frozen** candidate: built with `-ffp-contract=off` so distribution flags cannot change its arithmetic; `KPT_THRESHOLD` and tolerances are fixed. `sidetouch-match` wraps it without modifying it. Any change to matcher arithmetic changes scores for already-enrolled prints.
- **Overlay** (`overlay/libfprint-v1.94.100.patch`): the minimal upstream edits — registers the `sidetouch` driver, links `libsidetouch` into `fprint-drivers`, and fixes an upstream `tests/meson.build` loop. Keep it minimal; the driver builds as its own static libraries so `-Werror -Wextra -Wconversion` apply to this code only.

## Rules the code depends on

- **Tests**: pure-module tests (`proto`, `format4`, `reference`, `print`, `match`) use `tests/check.h` and synthetic grids from `sidetouch-synth`; state-machine tests (`exchange`, `bridge`, `acquire`, `open`, `device`) script every transfer with `sidetouch-script` builders (`bridge_ok`, `EXCHANGE`, `serial_read`). The fake completes from idle sources, so a machine that blocks the main loop never finishes, and any out-of-script operation fails the test. Test inputs are synthetic or recorded non-biometric data only.
- **Untrusted input**: device replies and stored prints are validated before indexing. Sensor-derived buffers are wiped with `sidetouch_wipe*` (`sidetouch-wipe.h`) before release.
- **Print format**: changing the `STPR` or `SKPT` layout needs a version bump. Prints in a newer or unknown format block enrolment and verification until deleted (`fprintd-delete`), so treat format changes as user-visible.
- Single-reader provenance: the established sequences (firmware 5.9 only, exact status words, guard bounds) were measured on one unit. Keep them exact unless there is evidence; do not loosen them to make something pass.
