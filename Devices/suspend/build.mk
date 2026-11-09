# UMS9620 LP_AP_INTC wake table: cluster power-down wake for enabled SPIs and
# strict system-suspend wake (PMIC: power key and RTC alarm only).
$(eval $(call module,w202ds-suspend-lpintc,suspend/source/ums9620-lpintc,ums9620_lpintc.ko))
.PHONY: w202ds-suspend-driver
w202ds-suspend-driver: w202ds-suspend-lpintc
