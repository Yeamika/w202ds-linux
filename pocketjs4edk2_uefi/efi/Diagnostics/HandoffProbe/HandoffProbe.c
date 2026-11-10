// SPDX-License-Identifier: BSD-2-Clause-Patent
// Opt-in RAMBoot integration test. Never installed in the ordinary platform FV.
#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#define PJB_EDK2
#include <pocketjs_boot_context.h>

STATIC VOID Trace(CONST CHAR8 *Text) {
  SerialPortWrite((UINT8 *)Text,AsciiStrLen(Text));
}

EFI_STATUS EFIAPI ProbeMain(EFI_HANDLE Image, EFI_SYSTEM_TABLE *Table) {
  PJB_GUID Wire=PJB_PLATFORM_GUID;
  EFI_GUID Guid;
  PJB_PLATFORM_PROTOCOL *Platform=NULL;
  PJB_POWER_SAMPLE Sample={0};
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_MEMORY_DESCRIPTOR *Map;
  UINTN Size=0,Capacity,Key,DescriptorSize,Attempt;
  UINT32 Version;
  EFI_STATUS Status;
  CopyMem(&Guid,&Wire,sizeof(Guid));
  Status=gBS->LocateProtocol(&Guid,NULL,(VOID **)&Platform);
  if (EFI_ERROR(Status) || Platform==NULL || Platform->revision!=PJB_PLATFORM_REVISION ||
      Platform->size<sizeof(*Platform)) return EFI_UNSUPPORTED;
  Sample.size=sizeof(Sample);
  Status=Platform->read_power(Platform,&Sample);
  if (Status!=EFI_NOT_READY || Sample.valid!=0) {
    Trace("HANDOFF_FAIL child hardware not quiesced\r\n"); return EFI_DEVICE_ERROR;
  }
  Trace("HANDOFF_CHILD quiesced power=NOT_READY valid=0\r\n");
  Status=gBS->HandleProtocol(Image,&gEfiLoadedImageProtocolGuid,(VOID **)&Loaded);
  if (EFI_ERROR(Status)) return Status;
  if (Loaded->LoadOptionsSize!=sizeof(L"exit") || Loaded->LoadOptions==NULL ||
      CompareMem(Loaded->LoadOptions,L"exit",sizeof(L"exit"))!=0) {
    Status=Platform->set_quiesced(Platform,0);
    if (EFI_ERROR(Status)) return Status;
    /* Power is sampled asynchronously. Wait for a fresh complete sample
     * rather than treating the deliberately empty post-resume cache as bad. */
    for (Attempt=0;Attempt<30;++Attempt) {
      gBS->Stall(100000);
      Sample.size=sizeof(Sample);
      Status=Platform->read_power(Platform,&Sample);
      if (!EFI_ERROR(Status) && (Sample.valid&PJB_POWER_VOLTAGE)) break;
      if (Status!=EFI_NOT_READY) break;
    }
    if (EFI_ERROR(Status) || !(Sample.valid&PJB_POWER_VOLTAGE)) {
      Platform->set_quiesced(Platform,1); return EFI_DEVICE_ERROR;
    }
    Trace("HANDOFF_CHILD resume power voltage valid\r\n");
    Status=Platform->set_quiesced(Platform,1);
    if (EFI_ERROR(Status)) return Status;
    Trace("HANDOFF_CHILD returning success\r\n"); return EFI_SUCCESS;
  }
  Status=gBS->GetMemoryMap(&Size,NULL,&Key,&DescriptorSize,&Version);
  if (Status!=EFI_BUFFER_TOO_SMALL) return Status;
  Capacity=Size+16*DescriptorSize;
  Map=AllocatePool(Capacity);
  if (Map==NULL) return EFI_OUT_OF_RESOURCES;
  Trace("HANDOFF_CHILD exiting Boot Services\r\n");
  for (Attempt=0;Attempt<2;++Attempt) {
    Size=Capacity;
    Status=gBS->GetMemoryMap(&Size,Map,&Key,&DescriptorSize,&Version);
    if (EFI_ERROR(Status)) break;
    Status=gBS->ExitBootServices(Image,Key);
    if (!EFI_ERROR(Status)) {
      // Only already retained pointers and the RAM trace writer after EBS.
      Sample.size=sizeof(Sample);
      Status=Platform->read_power(Platform,&Sample);
      if (Status==EFI_NOT_READY && Sample.valid==0 &&
          Platform->set_quiesced(Platform,0)==EFI_UNSUPPORTED)
        Trace("HANDOFF_EBS_OK hardware permanently stopped\r\n");
      else Trace("HANDOFF_EBS_FAIL hardware still available\r\n");
      CpuDeadLoop();
    }
    if (Status!=EFI_INVALID_PARAMETER) break;
  }
  // An EBS attempt may already have signalled events; never return to the UI.
  Trace("HANDOFF_EBS_FAIL awaiting recovery watchdog\r\n");
  CpuDeadLoop();
  return EFI_ABORTED;
}
