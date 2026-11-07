
$(eval $(call module,w202ds-usb3-legacy-phy,usb3/source/legacy-phy,phy-sprd-w202-dummy.ko,,CONFIG_USB_W202DS_DUMMY_PHY=m))
# USB-C DP alt mode; needs the kernel dw-dp, ssphy DP and ump9620-tcpc changes (config/fragments/usb-single-cable.config).
$(eval $(call module,w202ds-usb3-dp,usb3/source/w202-dp,w202_dp.ko))
.PHONY: w202ds-usb3-driver
w202ds-usb3-driver: w202ds-usb3-legacy-phy
