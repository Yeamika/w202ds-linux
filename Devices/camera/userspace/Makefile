# SPDX-License-Identifier: GPL-2.0-only
.DEFAULT_GOAL := help
.PHONY: help install
help:
	@printf '%s\n' 'Torch: install DESTDIR=/target/staging (udev template only)'
install:
	test -n "$(DESTDIR)" && test "$(DESTDIR)" != /
	install -D -m 0644 rootfs/usr/lib/udev/rules.d/70-w202-torch.rules "$(DESTDIR)/usr/lib/udev/rules.d/70-w202-torch.rules"
