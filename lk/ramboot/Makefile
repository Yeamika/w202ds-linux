CROSS_COMPILE ?= aarch64-linux-gnu-
ifndef OUT
$(error Set OUT to a build directory outside the source tree)
endif
CC := $(CROSS_COMPILE)gcc
LD := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy
RAMBOOT_SRC := ../uefi_ramboot.c
RAMBOOT_HDR := ../uefi_ramboot.h
# Public loader ABI supplied by the caller; this checkout knows no other tree.
HANDOFF_HEADER :=
ifneq ($(strip $(HANDOFF_INCLUDE)),)
HANDOFF_HEADER := $(abspath $(HANDOFF_INCLUDE))/LoaderHandoff.h
endif
CFLAGS := -O2 -Wall -Wextra -Werror -std=c11 -ffreestanding -fno-builtin \
          -fno-stack-protector -fno-pie -fno-asynchronous-unwind-tables \
          -ffunction-sections -fdata-sections -mgeneral-regs-only -ffixed-x18
CFLAGS += -I$(dir $(RAMBOOT_HDR)) $(if $(HANDOFF_HEADER),-I$(dir $(HANDOFF_HEADER))) $(EXTRA_CFLAGS)
RETURN_OBJECTS :=
ifeq ($(LK_RETURN),1)
CFLAGS += -DW202_LK_RETURN_ENABLED
RETURN_OBJECTS := $(OUT)/return-c.o $(OUT)/return-asm.o
endif

.PHONY: all
all: $(OUT)/ramboot-probe.bin

.PHONY: check-loader
check-loader: $(OUT)/ramboot-check.bin

.PHONY: watchdog-probe
watchdog-probe: $(OUT)/ramboot-watchdog.bin

.PHONY: retention-probe
retention-probe: $(OUT)/ramboot-retention.bin

.PHONY: handoff-probe
handoff-probe: $(OUT)/ramboot-handoff.bin

.PHONY: enter-sec
enter-sec: $(OUT)/ramboot-enter.bin

.PHONY: resident
resident: $(OUT)/ramboot-resident.bin

.PHONY: resident-bootstrap
resident-bootstrap: $(OUT)/resident-loader.bin $(OUT)/resident-start.bin

.PHONY: check-handoff-interface
check-handoff-interface:
	@test -n "$(HANDOFF_HEADER)" && test -f "$(HANDOFF_HEADER)" || \
		{ printf '%s\n' 'Set HANDOFF_INCLUDE to the public LoaderHandoff.h directory'; exit 2; }

$(OUT):
	mkdir -p $@

$(OUT)/probe.o: probes/probe.c | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/entry.o: probes/probe.S | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/ramboot-probe.elf: $(OUT)/entry.o $(OUT)/probe.o probes/probe.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none,--gc-sections,-T,probes/probe.ld \
		$(OUT)/entry.o $(OUT)/probe.o -o $@

$(OUT)/ramboot-probe.bin: $(OUT)/ramboot-probe.elf
	$(OBJCOPY) -O binary $< $@

$(OUT)/check-entry.o: probes/check-entry.S | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/check.o: probes/check.c $(RAMBOOT_HDR) | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/uefi-ramboot.o: $(RAMBOOT_SRC) $(RAMBOOT_HDR) | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/ramboot-check.elf: $(OUT)/check-entry.o $(OUT)/check.o $(OUT)/uefi-ramboot.o probes/probe.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none,--gc-sections,-T,probes/probe.ld \
		$(OUT)/check-entry.o $(OUT)/check.o $(OUT)/uefi-ramboot.o -o $@

$(OUT)/ramboot-check.bin: $(OUT)/ramboot-check.elf
	$(OBJCOPY) -O binary $< $@

$(OUT)/watchdog-entry.o: probes/watchdog-entry.S | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/watchdog.o: probes/watchdog.c | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/ramboot-watchdog.elf: $(OUT)/watchdog-entry.o $(OUT)/watchdog.o probes/probe.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none,--gc-sections,-T,probes/probe.ld \
		$(OUT)/watchdog-entry.o $(OUT)/watchdog.o -o $@

$(OUT)/ramboot-watchdog.bin: $(OUT)/ramboot-watchdog.elf
	$(OBJCOPY) -O binary $< $@

$(OUT)/retention-entry.o: probes/retention-entry.S | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/retention.o: probes/retention.c | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/ramboot-retention.elf: $(OUT)/retention-entry.o $(OUT)/retention.o probes/probe.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none,--gc-sections,-T,probes/probe.ld \
		$(OUT)/retention-entry.o $(OUT)/retention.o -o $@

$(OUT)/ramboot-retention.bin: $(OUT)/ramboot-retention.elf
	$(OBJCOPY) -O binary $< $@

$(OUT)/handoff-entry.o: probes/handoff-entry.S | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/handoff.o: probes/handoff.c | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/ramboot-handoff.elf: $(OUT)/handoff-entry.o $(OUT)/handoff.o probes/probe.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none,--gc-sections,-T,probes/probe.ld \
		$(OUT)/handoff-entry.o $(OUT)/handoff.o -o $@

$(OUT)/ramboot-handoff.bin: $(OUT)/ramboot-handoff.elf
	$(OBJCOPY) -O binary $< $@

$(OUT)/enter-entry.o: loader/enter-entry.S | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/enter.o: loader/enter.c $(RAMBOOT_HDR) $(HANDOFF_HEADER) | $(OUT) check-handoff-interface
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/return-c.o: loader/return.c $(HANDOFF_INCLUDE)/LkReturn.h | $(OUT) check-handoff-interface
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/return-asm.o: loader/return.S $(HANDOFF_INCLUDE)/LkReturn.h | $(OUT) check-handoff-interface
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/ramboot-enter.elf: $(OUT)/enter-entry.o $(OUT)/enter.o $(OUT)/uefi-ramboot.o $(RETURN_OBJECTS) probes/probe.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none,--gc-sections,-T,probes/probe.ld \
		$(OUT)/enter-entry.o $(OUT)/enter.o $(OUT)/uefi-ramboot.o $(RETURN_OBJECTS) -o $@

$(OUT)/ramboot-enter.bin: $(OUT)/ramboot-enter.elf
	$(OBJCOPY) -O binary $< $@

$(OUT)/resident-entry.o: loader/resident-entry.S loader/enter-entry.S | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/resident.o: loader/resident.c | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/ramboot-resident.elf: $(OUT)/resident-entry.o $(OUT)/resident.o $(OUT)/enter.o $(OUT)/uefi-ramboot.o $(RETURN_OBJECTS) probes/probe.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none,--gc-sections,-T,probes/probe.ld \
		$(OUT)/resident-entry.o $(OUT)/resident.o $(OUT)/enter.o $(OUT)/uefi-ramboot.o $(RETURN_OBJECTS) -o $@

$(OUT)/ramboot-resident.bin: $(OUT)/ramboot-resident.elf
	$(OBJCOPY) -O binary $< $@

# package-resident.py prepare supplies this layout; retain the exact stock ABI.
$(OUT)/resident-loader.o: loader/resident-loader.S $(OUT)/resident-layout.inc | $(OUT)
	$(CC) -march=armv8-a -Wa,-I,$(OUT) -c $< -o $@

$(OUT)/resident-loader.elf: $(OUT)/resident-loader.o
	$(LD) -Ttext=0xb500f1fc --entry=resident_loader \
		--defsym=lk_partition_read=0xb50405ec --defsym=ptable_page=0xb5f01000 \
		--defsym=fastboot_page=0xb5122000 --defsym=lk_icache_sync=0xb5004dc0 \
		--defsym=okay_page=0xb50bc000 --defsym=lk_status=0xb500e11c \
		--defsym=resident_start=0xb500ed40 $< -o $@

$(OUT)/resident-start.o: loader/resident-start.S | $(OUT)
	$(CC) -march=armv8-a -c $< -o $@

$(OUT)/resident-start.elf: $(OUT)/resident-start.o
	$(LD) -Ttext=0xb500ed40 --entry=resident_start \
		--defsym=resident_loader=0xb500f1fc --defsym=ptable_page=0xb5f01000 \
		--defsym=bridge_page=0xb500f000 --defsym=lk_lcd_splash=0xb5088bd4 \
		--defsym=lk_printf=0xb5035e94 \
		--defsym=permissions=0xb500e2a0 --defsym=lk_fastboot_init=0xb5012a80 $< -o $@

$(OUT)/resident-loader.bin: $(OUT)/resident-loader.elf
	$(OBJCOPY) -O binary -j .text $< $@

$(OUT)/resident-start.bin: $(OUT)/resident-start.elf
	$(OBJCOPY) -O binary -j .text $< $@
