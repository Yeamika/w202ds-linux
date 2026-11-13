# SPDX-License-Identifier: GPL-2.0-only
.DEFAULT_GOAL := help
.PHONY: help install
help:
	@printf '%s\n' 'Audio: install DESTDIR=/target/staging (no module loads or service activation)'
install:
	test -n "$(DESTDIR)" && test "$(DESTDIR)" != /
	install -D -m 0755 rootfs/usr/libexec/w202-audio-start "$(DESTDIR)/usr/libexec/w202-audio-start"
	install -D -m 0644 rootfs/usr/lib/systemd/system/w202-audio.service "$(DESTDIR)/usr/lib/systemd/system/w202-audio.service"
	install -D -m 0644 rootfs/usr/lib/modprobe.d/w202-audio.conf "$(DESTDIR)/usr/lib/modprobe.d/w202-audio.conf"
	install -D -m 0644 rootfs/usr/lib/udev/rules.d/89-w202ds-audio.rules "$(DESTDIR)/usr/lib/udev/rules.d/89-w202ds-audio.rules"
	install -D -m 0644 rootfs/usr/share/alsa-card-profile/mixer/profile-sets/w202ds.conf "$(DESTDIR)/usr/share/alsa-card-profile/mixer/profile-sets/w202ds.conf"
