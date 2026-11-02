/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* Minimal assertion helpers for the SideTouch driver's pure-module tests. */
#ifndef SIDETOUCH_CHECK_H
#define SIDETOUCH_CHECK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check_failures;

#define CHECK(cond) \
  do { \
      if (!(cond)) \
        { \
          fprintf (stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
          check_failures++; \
        } \
    } while (0)

#define CHECK_MEM(a, b, n) CHECK (memcmp ((a), (b), (n)) == 0)

#define CHECK_DONE(name) \
  do { \
      if (check_failures) \
        { \
          fprintf (stderr, "%s: %d failure(s)\n", (name), check_failures); \
          return EXIT_FAILURE; \
        } \
      printf ("%s: ok\n", (name)); \
      return EXIT_SUCCESS; \
    } while (0)

#endif
