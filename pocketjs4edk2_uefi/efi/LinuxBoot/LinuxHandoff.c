// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

// UMP9620 ADI protocol, same bounded transactions as BoardServicesDxe.
// Called only after successful ExitBootServices: no other firmware user runs.
// Stop the inherited countdown before Linux can probe its custom UMP9620
// watchdog driver; do not change reboot reason or clocks.
STATIC EFI_STATUS Read(UINT32 Address, UINT32 *Value) {
  UINTN I; UINT32 Data;
  if (MmioRead32(0x6440002c)&BIT31) return EFI_NOT_READY;
  MmioWrite32(0x64400028,Address);
  for (I=0;I<2000;++I) {
    Data=MmioRead32(0x6440002c);
    if (!(Data&BIT31)) { *Value=Data&0xffff; return EFI_SUCCESS; }
    MicroSecondDelay(1);
  }
  return EFI_TIMEOUT;
}
STATIC EFI_STATUS Write(UINT32 Address, UINT32 Value) {
  UINTN I; UINT32 Fifo;
  for (I=0;I<2000;++I) {
    Fifo=MmioRead32(0x64400030);
    if ((Fifo&BIT10) && !(Fifo&(BIT11|BIT31))) break;
    MicroSecondDelay(1);
  }
  if (I==2000) return EFI_TIMEOUT;
  MmioWrite32(Address,Value);
  for (I=0;I<2000;++I) {
    Fifo=MmioRead32(0x64400030);
    if ((Fifo&BIT10) && !(Fifo&BIT31)) return EFI_SUCCESS;
    MicroSecondDelay(1);
  }
  return EFI_TIMEOUT;
}
EFI_STATUS LinuxStopWatchdog(VOID) {
  UINT32 Control, Verify; EFI_STATUS Status, Lock;
  Status=Read(0x64420048,&Control); if (EFI_ERROR(Status)) return Status;
  if (!(Control&BIT1)) return EFI_SUCCESS;
  Status=Write(0x64420060,0xe551);
  if (!EFI_ERROR(Status)) Status=Write(0x64420048,Control&~BIT1);
  Lock=Write(0x64420060,0x1aae);
  if (!EFI_ERROR(Status)) Status=Lock;
  if (!EFI_ERROR(Status)) Status=Read(0x64420048,&Verify);
  if (!EFI_ERROR(Status) && Verify!=(Control&~BIT1)) Status=EFI_DEVICE_ERROR;
  return Status;
}
