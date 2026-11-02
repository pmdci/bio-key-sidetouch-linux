# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
#
# Keypoint matcher. Builds anywhere with a C11 compiler.
CC ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror
LDLIBS = -lm
ifndef BUILD
$(error set BUILD=<directory outside the source tree>, e.g. make BUILD=/tmp/x test)
endif

TESTS = $(BUILD)/test-kpmatch $(BUILD)/test-sift $(BUILD)/test-kpcand $(BUILD)/test-kptemplate

.PHONY: test clean
test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; $$t || exit 1; done

$(BUILD)/test-kpmatch: test-kpmatch.c kpmatch.c kpmatch.h | $(BUILD)
	$(CC) $(CFLAGS) test-kpmatch.c kpmatch.c -o $@ $(LDLIBS)

$(BUILD)/test-sift: test-sift.c sift.c sift.h kpmatch.c kpmatch.h | $(BUILD)
	$(CC) $(CFLAGS) test-sift.c sift.c kpmatch.c -o $@ $(LDLIBS)

$(BUILD)/test-kpcand: test-kpcand.c kpcand.c kpcand.h sift.c sift.h kpmatch.c kpmatch.h | $(BUILD)
	$(CC) $(CFLAGS) test-kpcand.c kpcand.c sift.c kpmatch.c -o $@ $(LDLIBS)

$(BUILD)/test-kptemplate: test-kptemplate.c kptemplate.c kptemplate.h kpcand.c kpcand.h sift.c sift.h kpmatch.c kpmatch.h | $(BUILD)
	$(CC) $(CFLAGS) test-kptemplate.c kptemplate.c kpcand.c sift.c kpmatch.c -o $@ $(LDLIBS)

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
