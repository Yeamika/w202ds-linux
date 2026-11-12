PYTHON ?= python3
CROSS_COMPILE ?= aarch64-linux-gnu-
DEBUG ?= 1
WATCHDOG ?= $(DEBUG)
USB_RECOVERY ?= $(DEBUG)
SCREEN ?= $(DEBUG)
FASTBOOT_FLASH ?= 1
WATCHDOG_MS ?= 30000
ifndef OUT
$(error Set OUT to a fresh build directory outside the source tree)
endif
ifneq ($(filter-out 0 1,$(DEBUG) $(WATCHDOG) $(USB_RECOVERY) $(SCREEN) $(FASTBOOT_FLASH)),)
$(error DEBUG, WATCHDOG, USB_RECOVERY, SCREEN and FASTBOOT_FLASH must be 0 or 1)
endif

.PHONY: all lk-debuger
all: lk-debuger
lk-debuger:
	$(PYTHON) build.py --output "$(OUT)" --cross-compile "$(CROSS_COMPILE)" \
		$(if $(filter 1,$(DEBUG)),--debug,--no-debug) \
		$(if $(filter 1,$(WATCHDOG)),--watchdog,--no-watchdog) \
		$(if $(filter 1,$(USB_RECOVERY)),--usb-recovery,--no-usb-recovery) \
		$(if $(filter 1,$(SCREEN)),--screen,--no-screen) \
		$(if $(filter 1,$(FASTBOOT_FLASH)),--fastboot-flash,--no-fastboot-flash) \
		--watchdog-ms "$(WATCHDOG_MS)"
