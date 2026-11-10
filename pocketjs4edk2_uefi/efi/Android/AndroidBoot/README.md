# AndroidBoot (experimental)

A development EFI application that reads the existing Android A boot image
through UEFI BlockIo and prepares a raw ARM64 handoff. It does not call LK,
reset the device, change slots or write partitions. This is not a production
Android bootloader. The native Android kernel starts in the physical RAM test,
but Android userspace requests recovery before reaching the UI.

## Inputs and build

Build with `efi/PocketJsUefiPkg.dsc` using `efi/build-efi.sh`. The application
is installed as `ESP/EFI/Tools/AndroidBoot.efi`; it is not embedded in the
platform FV. Launch it explicitly from the ESP for diagnostics. With
`W202_ANDROID_BOOT_ENTER=FALSE` (default), it only reads, validates and stages
inputs, prints CRC and `ANDROID_DIRECT PREFLIGHT_OK`, then returns.
Set `W202_ANDROID_BOOT_ENTER=TRUE` in the standalone EFI build environment to
attempt ExitBootServices and enter the raw kernel. Use an explicitly armed
RAMBoot recovery watchdog for device experiments. The bare FD/wrapper is never
a flashable uboot container. Historical FV/BDS auto-launch experiments below
predate this source migration.

Inputs are the board's 64 MiB `boot_a`, 100 MiB `vendor_boot_a`, and
`EFI/W202DS/ANDROID.DTB` on the known GPT ESP. The DT is an explicit snapshot
captured from a successful Android A boot. It contains applied board overlays
and LK fixups; it must be replaced when those inputs change. The current empty
`init_boot_a` is not used. The app supports boot/vendor header v4, one platform
vendor fragment and an empty bootconfig, assembling vendor + generic ramdisk
and the 20-byte bootconfig trailer. Other layouts are rejected.

## Memory and validation

- Initrd: `0xd0000000`, capacity 64 MiB.
- Kernel: `0xd4000000 + Image.text_offset`, within 64 MiB. The captured image
  has offset `0x80000` and entry `0xd4080000`, away from live UEFI memory.
- DT: `0xd8000000`, capacity 2 MiB.

These are identity mappings used only by the diagnostic build, outside the UEFI
allocator. The app checks the complete staging range against the DT's DRAM and
both reservation mechanisms before writing it. It checks file/header bounds,
ARM64 placement flags, the sole vendor fragment and CRCs for the captured
development kernel/initrd. CRC checks detect transfer/input mismatches; they
are not AVB authentication. The snapshot must report unlocked/orange Android A.
Captured RNG/KASLR seeds and any stale EFI properties are discarded. A successful
direct boot can be identified by `w202.uefi_direct=1` in the kernel command line.

The read-only physical preflight passed on 2026-11-04: 39,535,104 kernel bytes,
26,699,145 initrd bytes, matching CRCs `68c42eb7` / `c6a7116c`, and a prepared
217,175-byte DT. Reading, validating and staging took 10,295 ms. The RAM test
returned EFI_SUCCESS and recovered to native B Fastboot through its watchdog;
this preflight alone does not enter the Android kernel.

The first actual handoff hit ArmLib's `!ArmMmuEnabled()` assertion. The corrected
EL2 entry uses stackless VA-based cache maintenance over the platform's identity
mapped cacheable DRAM, then disables caches/MMU and branches to the kernel.
The next RAM test reached the Android kernel: retained ramoops records relocated
kernel code at `0xd56c6f98` and a `recovery` reboot at 2.956 seconds. The boot
message reported `init_user0_failed`; the subsequent native B boot entered the
existing Debian KDE root on p77. The recovery request and arguments were cleared
and read back afterwards. No userdata erase was performed. The app itself does
not write boot-control; Android writes the recovery request after it takes over.

The 2026-11-05 `enter4` RAM test with `loglevel=7 ignore_loglevel` also requested
recovery; verbose driver messages overwrote the useful part of the 32 KiB
console record. The subsequent `enter5` test uses `loglevel=5 initcall_debug=0`
and preserves the failure chain:

- At 16.703 seconds, vold reports Keystore `createOperation` error `-33`, then
  `decryptWithKeystoreKey failed` and `read_key failed in mountFstab`.
- Mounting userdata at `/data` fails. At 18.245 seconds, vold cannot prepare
  `/data/misc/vold/user_keys`; init requests recovery with `init_user0_failed`.
- The Android kernel reboots to recovery at 18.533 seconds. Native B then boots
  the existing Debian root on p77; this is not a direct UEFI Linux boot.

Error `-33` is `INVALID_KEY_BLOB` in the
[AOSP Keymaster interface](https://android.googlesource.com/platform/hardware/interfaces/+/de0e5293bd68b5956561c9e2547b9d61a08154ba/keymaster/4.0/types.hal).
The immediate failure is now established as userdata key decryption. Missing
native security/Keymaster boot setup remains a hypothesis for that failure,
not a verified root cause. The public LK boot flow calls `set_root_of_trust`
before loading the kernel; its board-specific ABI and inputs still need to be
verified before adding an equivalent handoff.

Evidence is in `output/controls-usb-20261104/linux-after-enter5/`, including the
console record and original pstore archive. After capture, the recovery request
was cleared, A was selected and read back, and Linux's filesystems were synced
and remounted read-only before an emergency restart. Normal Android A reached
`sys.boot_completed=1` with ADB/root. A/B uboot and boot hashes remain unchanged;
B still contains the verified ESP/charging-button firmware. Neither diagnostic
image was flashed, and no userdata erase was performed. Normal Linux reboot
still stalls during device shutdown; the emergency restart is a recovery route,
not a fix for that shutdown bug.

Host tests use explicitly supplied original boot/vendor images:
`W202_TEST_OUTPUT=... W202_TEST_BOOT=... W202_TEST_VENDOR_BOOT=... python3 -B tests/test_android_boot.py`
from the `pocketjs4edk2_uefi` project root. They cover the real image layouts, truncation,
overflow, unsupported variants and relocated kernel bounds. Successful Android
userspace startup and normal menu integration still require device validation.

## TODO

- Resolve the captured Keystore key-decryption failure that causes
  `init_user0_failed`; compare the native and direct-boot security handoffs.
- Replace the captured DT with current board fixups and current boot parameters.
- Port/verify LK's prerequisite firmware loading and security handoff.
- Implement image authentication and additional vendor/init_boot layouts before
  treating this as a general Android loader.
- Integrate the validated loader with the normal PocketJS menu. Server/KDE
  selection and their Linux handoff are separate unfinished work.
