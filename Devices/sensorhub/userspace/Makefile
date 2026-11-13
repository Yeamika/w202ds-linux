# SPDX-License-Identifier: GPL-2.0-only
.DEFAULT_GOAL := help
OUT ?= $(abspath ../../../output/userspace/sensorhub)
HOSTCC ?= cc
CPPFLAGS ?=
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
HOSTLDFLAGS ?=
PROTOCOL_DIR := ../source/w202-hub-iio

.PHONY: help check install
help:
	@printf '%s\n' \
	  'install DESTDIR=/target/staging: stage Sensor Hub service and udev templates' \
	  'check OUT=/path/to/output: native wire-protocol tests; no device access'

$(OUT):
	mkdir -p "$@"

$(OUT)/protocol-test: $(PROTOCOL_DIR)/test_protocol.c $(PROTOCOL_DIR)/w202_hub_protocol.h | $(OUT)
	$(HOSTCC) $(CPPFLAGS) $(CFLAGS) $< $(HOSTLDFLAGS) -o "$@"

check: $(OUT)/protocol-test
	"$(OUT)/protocol-test"

# Stage only. Module installation, depmod and service activation belong to integration.
install:
	test -n "$(DESTDIR)" && test "$(DESTDIR)" != /
	install -D -m 0755 rootfs/usr/libexec/w202-sensorhub-attach "$(DESTDIR)/usr/libexec/w202-sensorhub-attach"
	install -D -m 0644 rootfs/usr/lib/systemd/system/w202-sensorhub.service "$(DESTDIR)/usr/lib/systemd/system/w202-sensorhub.service"
	install -D -m 0644 rootfs/usr/lib/udev/rules.d/81-w202-sensorhub.rules "$(DESTDIR)/usr/lib/udev/rules.d/81-w202-sensorhub.rules"
