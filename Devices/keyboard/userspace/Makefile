# SPDX-License-Identifier: GPL-2.0-only
.DEFAULT_GOAL := all
CROSS_COMPILE ?= aarch64-linux-gnu-
CC = $(CROSS_COMPILE)gcc
HOSTCC ?= cc
OUT ?= $(abspath ../../../output/userspace/keyboard)
CPPFLAGS ?=
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Werror
LDFLAGS ?= -static
HOSTLDFLAGS ?=
PROTOCOL_DIR := ../source/keyboard-m22

.PHONY: all diagnostics check install
all: $(OUT)/w202-keyboard-attach

$(OUT):
	mkdir -p "$@"

$(OUT)/w202-keyboard-attach: source/keyboard-attach.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o "$@"

$(OUT)/uart-capture: tests/uart-capture.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o "$@"

$(OUT)/protocol-test: tests/protocol-test.c $(PROTOCOL_DIR)/zsk-protocol.h | $(OUT)
	$(HOSTCC) $(CPPFLAGS) -I"$(PROTOCOL_DIR)" $(CFLAGS) $< $(HOSTLDFLAGS) -o "$@"

diagnostics: $(OUT)/uart-capture
check: $(OUT)/protocol-test
	"$(OUT)/protocol-test"

# Stage only. No module loads, service activation or desktop session access.
install: all
	test -n "$(DESTDIR)" && test "$(DESTDIR)" != /
	install -D -m 0755 "$(OUT)/w202-keyboard-attach" "$(DESTDIR)/usr/libexec/w202-keyboard-attach"
	install -D -m 0644 rootfs/usr/lib/systemd/system/w202-keyboard.service "$(DESTDIR)/usr/lib/systemd/system/w202-keyboard.service"
