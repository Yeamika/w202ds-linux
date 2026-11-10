// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#define W202_LK_RETURN_EFI
#include <LkReturn.h>

VOID W202ReturnLk(UINT64 Resume, UINT64 Action);

STATIC EFI_STATUS FlushDevices(VOID) {
  EFI_HANDLE *Handles;
  EFI_BLOCK_IO_PROTOCOL *Block;
  EFI_STATUS Status;
  UINTN Count, I;
  Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiBlockIoProtocolGuid,NULL,&Count,&Handles);
  if (EFI_ERROR(Status)) return Status;
  for (I=0;I<Count;++I) {
    Status=gBS->HandleProtocol(Handles[I],&gEfiBlockIoProtocolGuid,(VOID **)&Block);
    if (EFI_ERROR(Status)) break;
    if (!Block->Media || !Block->Media->MediaPresent || Block->Media->ReadOnly) continue;
    Status=Block->FlushBlocks(Block);
    if (EFI_ERROR(Status)) break;
  }
  FreePool(Handles);
  return Status;
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE Image, EFI_SYSTEM_TABLE *System) {
  W202_LK_RETURN Handoff;
  EFI_MEMORY_DESCRIPTOR *Map;
  UINTN Size, Key, DescriptorSize, Attempt;
  UINT32 Version;
  UINT64 El, Action=W202_LK_RETURN_ANDROID;
  EFI_STATUS Status;
  // The adapter consumes a suspended-LK contract, never a cold LK entrypoint.
  CopyMem(&Handoff,(VOID *)(UINTN)W202_LK_RETURN_ADDRESS,sizeof(Handoff));
  __asm__ volatile("mrs %0, CurrentEL":"=r"(El));
  if (El!=8 || !W202LkReturnValid(&Handoff)) return EFI_UNSUPPORTED;
#ifdef W202_LK_RETURN_PROBE_ONLY
  Action=W202_LK_RETURN_PROBE;
#endif
  Status=FlushDevices();
  if (EFI_ERROR(Status)) return Status;
  // Spare capacity lets EBS retry without allocating after partial shutdown.
  Map=AllocatePool(128*1024);
  if (!Map) return EFI_OUT_OF_RESOURCES;
  DEBUG((DEBUG_WARN,"ANDROID_LK resume=%lx action=%Lu context=%lx\n",
    Handoff.resume,Action,Handoff.context));
  for (Attempt=0;Attempt<4;++Attempt) {
    Size=128*1024;
    Status=gBS->GetMemoryMap(&Size,Map,&Key,&DescriptorSize,&Version);
    if (EFI_ERROR(Status)) {
      if (Attempt==0) {FreePool(Map);return Status;}
      break;
    }
    Status=gBS->ExitBootServices(Image,Key);
    if (!EFI_ERROR(Status)) W202ReturnLk(Handoff.resume,Action);
    if (Status!=EFI_INVALID_PARAMETER) break;
  }
  // EBS may already have shut down drivers: do not return to the menu.
  // The LK entry armed the PMIC recovery watchdog before entering EDK2.
  DisableInterrupts();
  CpuDeadLoop();
  return EFI_DEVICE_ERROR;
}
