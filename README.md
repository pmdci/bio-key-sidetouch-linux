# BIO-key SideTouch driver for libfprint

A libfprint driver for the BIO-key SideTouch USB fingerprint reader
(`2963:2001`), with a keypoint matcher built from published methods. It is
built as an overlay on libfprint `v1.94.100` and installed for fprintd only, so
ordinary clients (`fprintd-enroll`, `fprintd-verify`, desktop login and lock
screens, PAM) use it.

**Not for security-critical use.** The driver has been tested on one reader.
How it behaves on other units of this reader is unknown. It makes no claim
about accuracy or about resistance to attack.

## Build

Needs: git, meson, ninja, a C compiler, GLib, gusb, pixman and gudev (upstream
libfprint's own build dependencies), and network access to clone libfprint.

```sh
./build.sh BUILD_DIR
```

`BUILD_DIR` must be outside this tree. The script clones libfprint at the
pinned tag, checks its commit, applies the overlay, adds the driver and builds.
It then runs the driver's offline tests, which must all pass. Re-running it
resets the clone to the pinned commit, discarding any change made inside it.

The matcher has its own tests: `make -C matcher BUILD=DIR test`.

## Install

```sh
sudo ./install.sh BUILD_DIR
```

This copies the built library to `/usr/local/lib/sidetouch-libfprint/lib` and
adds a systemd drop-in that points fprintd at it, then restarts fprintd if it
is running. Nothing the distribution's package manager owns is touched; other
programs keep the distribution's libfprint. `--dry-run` prints every action
without performing it.

Then enrol a finger, for example
`fprintd-enroll -f right-index-finger "$USER"`. Keep the finger off the
sensor after the last touch: the driver measures the empty sensor then.

## Uninstall

```sh
sudo ./install.sh --uninstall
```

This removes the drop-in and the library, and fprintd goes back to the
distribution's libfprint. Prints enrolled with this driver stay on disk;
remove them with `fprintd-delete "$USER"` if you no longer want them.

## Prints the driver cannot read

A print that this driver cannot read blocks enrolment and verification until
it is deleted (`fprintd-delete "$USER"`) and the fingers are enrolled again.
That covers a print in a later format, and a print left by another driver
registered under the name `sidetouch`.

## Limits

- One reader tested; behaviour on other units is unknown.
- Tested only with firmware 5.9. A reader reporting another version opens
  with a warning in the fprintd journal naming it; please report whether it
  works at <https://github.com/pmdci/bio-key-sidetouch-linux/issues>.
- Not for security-critical use; no accuracy or security claim.
- Prints the driver cannot read must be deleted and re-enrolled (above).
- If the host sleeps while the reader is open but idle, the driver's resume
  handling is never called. This path is untested.
- While installed, fprintd uses this driver's libfprint (`v1.94.100`), not the
  distribution's, so libfprint updates from the distribution, security fixes
  included, do not reach fprintd. They arrive only when this driver moves to a
  newer libfprint.
- The installed library survives distribution updates of libfprint, since no
  package-owned file is touched. A future fprintd built against a newer
  libfprint could need functions `v1.94.100` lacks; fprintd would then fail to
  start, for every reader, until the driver is rebuilt against a newer
  libfprint or uninstalled.

## How this driver was written

The reader's USB protocol (framing, commands, status words and image format)
was worked out from USB captures of the reader and from analysis of its
Windows driver, to make the reader work on Linux. This repository contains no
code from the Windows driver or from any other BIO-key or IDEX software, and
needs none of it at run time.

Minutiae matching (libfprint's NBIS) finds too few features in this sensor's
small images, so the driver uses its own keypoint matcher, built from
published methods and open-source code (see Credits). Its acceptance threshold
was set on the author's own test impressions.

## Credits

The matcher is written from published methods and open-source code, not from
any vendor code:

- SIFT keypoints and descriptors: D. G. Lowe, "Distinctive Image Features from
  Scale-Invariant Keypoints", IJCV 60(2), 2004, implemented to the
  specification and default parameters of I. Rey-Otero and M. Delbracio,
  "Anatomy of the SIFT Method", IPOL 4 (2014), doi:10.5201/ipol.2014.82.
- Matching and geometric verification: Lowe's ratio test, and the per-anchor
  geometric verification of SIGFM by Charette, England-Elbro and Mangliev
  (<https://github.com/goodix-fp-linux-dev/sigfm>), a keypoint matcher for
  small fingerprint sensors, ported to C from its `demo.cpp` with different
  tolerances. MIT licence; see `LICENSE.SIGFM`.

BIO-key and SideTouch are trademarks of their owners, used here only to name
the reader this driver supports. This project is not affiliated with or
endorsed by BIO-key International or IDEX Biometrics.

## Licence

Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>

GNU Lesser General Public License, version 2.1 or (at your option) any later
version, the same terms as libfprint. See `LICENSE`. The SIGFM-derived part of
`matcher/kpmatch.c` is also under the MIT licence; see `LICENSE.SIGFM`.
