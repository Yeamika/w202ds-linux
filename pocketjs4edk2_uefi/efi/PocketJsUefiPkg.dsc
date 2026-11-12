# SPDX-License-Identifier: BSD-2-Clause-Patent
# Standalone ESP loaders and diagnostics for the PocketJS UEFI composition.
[Defines]
  PLATFORM_NAME = PocketJsUefi
  PLATFORM_GUID = 1e51c2f4-7b21-4d0e-9d1a-2f6a5c9b8e31
  PLATFORM_VERSION = 0.1
  DSC_SPECIFICATION = 0x0001001A
  OUTPUT_DIRECTORY = Build/PocketJsUefi
  SUPPORTED_ARCHITECTURES = AARCH64
  BUILD_TARGETS = RELEASE
  SKUID_IDENTIFIER = DEFAULT
  DEFINE W202_ANDROID_BOOT_ENTER = FALSE
  DEFINE W202_LK_RETURN_PROBE_ONLY = FALSE
!include MdePkg/MdeLibs.dsc.inc
[BuildOptions]
  GCC:*_*_AARCH64_CC_FLAGS = -fno-jump-tables -ffixed-x18 -I"$(PJB_BOOT_ABI_INCLUDE)" -I"$(W202_PLATFORM_INCLUDE)"
  GCC:*_*_AARCH64_PP_FLAGS = -I"$(W202_PLATFORM_INCLUDE)"
  GCC:*_*_AARCH64_CC_XIPFLAGS == -mstrict-align
!if $(W202_ANDROID_BOOT_ENTER) == TRUE
  GCC:*_*_AARCH64_CC_FLAGS = -DW202_ANDROID_BOOT_ENTER
!endif
!if $(W202_LK_RETURN_PROBE_ONLY) == TRUE
  GCC:*_*_AARCH64_CC_FLAGS = -DW202_LK_RETURN_PROBE_ONLY
!endif
[LibraryClasses]
  ArmLib|ArmPkg/Library/ArmLib/ArmBaseLib.inf
  ArmGenericTimerCounterLib|ArmPkg/Library/ArmGenericTimerPhyCounterLib/ArmGenericTimerPhyCounterLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  CacheMaintenanceLib|ArmPkg/Library/ArmCacheMaintenanceLib/ArmCacheMaintenanceLib.inf
  CpuLib|MdePkg/Library/BaseCpuLib/BaseCpuLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  FdtLib|EmbeddedPkg/Library/FdtLib/FdtLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  SerialPortLib|Ums9620Pkg/W202ds/Library/RamTraceSerialPortLib/RamTraceSerialPortLib.inf
  SynchronizationLib|MdePkg/Library/BaseSynchronizationLib/BaseSynchronizationLib.inf
  TimerLib|ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  NULL|ArmPkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf
  NULL|MdePkg/Library/BaseStackCheckLib/BaseStackCheckLib.inf
[Components]
  efi/Android/AndroidBoot/AndroidBoot.inf
  efi/Android/AndroidLk/AndroidLk.inf
  efi/LinuxBoot/DebianServer.inf
  efi/LinuxBoot/DebianKde.inf
  efi/Diagnostics/HandoffProbe/HandoffProbe.inf
  efi/Diagnostics/TouchScope/TouchScope.inf
