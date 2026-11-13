# Standalone rear torch; this does not build a camera sensor or ISP driver.
$(eval $(call module,w202ds-camera-flash,camera/source/w202-flash,w202_flash_aw36518.ko))
# Explicit opt-in target; no change to the current Server module selection.
