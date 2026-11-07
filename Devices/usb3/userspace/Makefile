# SPDX-License-Identifier: MIT
# Stage only. Enabling and choosing between controller stacks belong to integration.
.PHONY: install
install:
	test -n "$(DESTDIR)" && test "$(DESTDIR)" != /
	install -D -m 0644 rootfs/etc/modprobe.d/w202-usbc-pd.conf "$(DESTDIR)/etc/modprobe.d/w202-usbc-pd.conf"
	install -D -m 0644 rootfs/etc/udev/rules.d/73-w202-usbc-drswap.rules "$(DESTDIR)/etc/udev/rules.d/73-w202-usbc-drswap.rules"
	install -D -m 0755 rootfs/usr/local/sbin/w202-usbc-drswap "$(DESTDIR)/usr/local/sbin/w202-usbc-drswap"
	install -D -m 0644 integration/udev/rules.d/72-w202-mutter.rules "$(DESTDIR)/usr/lib/udev/rules.d/72-w202-mutter.rules"
