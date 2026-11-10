// SPDX-License-Identifier: BSD-2-Clause-Patent
// Development-only raw ARM64 boot. No reset, LK calls or partition writes.
#include "BootImage.h"
#include <Guid/Gpt.h>
#include <Guid/FileInfo.h>
#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <libfdt.h>

VOID W202EnterKernel(UINT64 Entry, UINT64 Dtb);
STATIC EFI_GUID mEspId={0xd41bf147,0x3971,0x41ed,{0xa3,0x9b,0xd5,0x32,0x30,0x2c,0xbb,0x00}};
STATIC UINT64 Be64(CONST VOID *Value) {
  CONST UINT8 *P=Value; UINT64 N=0; UINTN I;
  for (I=0;I<8;++I) N=(N<<8)|P[I];
  return N;
}
STATIC BOOLEAN Overlap(UINT64 Base,UINT64 Size,UINT64 Other,UINT64 OtherSize) {
  if (!Size || !OtherSize) return FALSE;
  if (Size>MAX_UINT64-Base || OtherSize>MAX_UINT64-Other) return TRUE;
  return Base<Other+OtherSize && Other<Base+Size;
}
STATIC EFI_STATUS FindPartition(CONST CHAR16 *Name,EFI_BLOCK_IO_PROTOCOL **Block,EFI_HANDLE *Handle) {
  EFI_HANDLE *Handles=NULL; EFI_PARTITION_INFO_PROTOCOL *Part;
  EFI_BLOCK_IO_PROTOCOL *Candidate; EFI_STATUS Status;
  UINTN Count=0,I; *Block=NULL; if (Handle) *Handle=NULL;
  Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiPartitionInfoProtocolGuid,NULL,&Count,&Handles);
  if (EFI_ERROR(Status)) return Status;
  for (I=0;I<Count;++I) {
    Status=gBS->HandleProtocol(Handles[I],&gEfiPartitionInfoProtocolGuid,(VOID **)&Part);
    if (EFI_ERROR(Status) || Part->Type!=PARTITION_TYPE_GPT ||
        StrnCmp(Part->Info.Gpt.PartitionName,Name,StrLen(Name)+1)) continue;
    Status=gBS->HandleProtocol(Handles[I],&gEfiBlockIoProtocolGuid,(VOID **)&Candidate);
    if (EFI_ERROR(Status) || !Candidate->Media || !Candidate->Media->MediaPresent ||
        !Candidate->Media->LogicalPartition || Candidate->Media->BlockSize!=512) continue;
    if (Handle && (!CompareGuid(&Part->Info.Gpt.UniquePartitionGUID,&mEspId) ||
        !CompareGuid(&Part->Info.Gpt.PartitionTypeGUID,&gEfiPartTypeSystemPartGuid) ||
        Part->Info.Gpt.StartingLBA!=109006848 || Part->Info.Gpt.EndingLBA!=110055423)) continue;
    if (*Block) { FreePool(Handles);return EFI_NO_MAPPING; }
    *Block=Candidate; if (Handle) *Handle=Handles[I];
  }
  FreePool(Handles);
  return *Block?EFI_SUCCESS:EFI_NOT_FOUND;
}
STATIC EFI_STATUS Read(EFI_BLOCK_IO_PROTOCOL *Block,UINT64 Offset,UINT64 Bytes,VOID *Buffer) {
  EFI_STATUS Status; UINT8 Tail[512]; UINTN N; UINT8 *Out=Buffer;
  if (Block->Media->LastBlock>=MAX_UINT64/512 || Offset&511 ||
      Offset>(Block->Media->LastBlock+1)*512 || Bytes>(Block->Media->LastBlock+1)*512-Offset)
    return EFI_BAD_BUFFER_SIZE;
  while (Bytes>=512) {
    N=(UINTN)MIN(Bytes&~511ULL,0x10000ULL);
    Status=Block->ReadBlocks(Block,Block->Media->MediaId,Offset/512,N,Out);
    if (EFI_ERROR(Status)) return Status;
    Offset+=N; Out+=N; Bytes-=N;
  }
  if (Bytes) {
    Status=Block->ReadBlocks(Block,Block->Media->MediaId,Offset/512,512,Tail);
    if (EFI_ERROR(Status)) return Status;
    CopyMem(Out,Tail,(UINTN)Bytes);
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS LoadDtb(VOID **Dtb) {
  EFI_BLOCK_IO_PROTOCOL *Block; EFI_HANDLE Esp;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs; EFI_FILE_PROTOCOL *Root=NULL,*File=NULL;
  EFI_STATUS Status; EFI_FILE_INFO *Info=NULL; UINTN Size=0,Bytes;
  *Dtb=NULL;
  Status=FindPartition(L"esp",&Block,&Esp); if (EFI_ERROR(Status)) return Status;
  Status=gBS->HandleProtocol(Esp,&gEfiSimpleFileSystemProtocolGuid,(VOID **)&Fs);
  if (EFI_ERROR(Status)) return Status;
  Status=Fs->OpenVolume(Fs,&Root); if (EFI_ERROR(Status)) return Status;
  Status=Root->Open(Root,&File,L"\\EFI\\W202DS\\ANDROID.DTB",EFI_FILE_MODE_READ,0);
  if (EFI_ERROR(Status)) goto Done;
  Status=File->GetInfo(File,&gEfiFileInfoGuid,&Size,NULL);
  if (Status!=EFI_BUFFER_TOO_SMALL || Size<SIZE_OF_EFI_FILE_INFO || Size>65536) {Status=EFI_VOLUME_CORRUPTED;goto Done;}
  Info=AllocatePool(Size);if (!Info) {Status=EFI_OUT_OF_RESOURCES;goto Done;}
  Status=File->GetInfo(File,&gEfiFileInfoGuid,&Size,Info);if (EFI_ERROR(Status)) goto Done;
  if ((Info->Attribute&EFI_FILE_DIRECTORY) || Info->FileSize<sizeof(struct fdt_header) ||
      Info->FileSize>0x100000) {Status=EFI_BAD_BUFFER_SIZE;goto Done;}
  Bytes=(UINTN)Info->FileSize; *Dtb=AllocatePool(Bytes);
  if (!*Dtb) {Status=EFI_OUT_OF_RESOURCES;goto Done;}
  Status=File->Read(File,&Bytes,*Dtb);
  if (!EFI_ERROR(Status) && (Bytes!=Info->FileSize || fdt_check_header(*Dtb) ||
      fdt_totalsize(*Dtb)>Bytes)) Status=EFI_VOLUME_CORRUPTED;
Done:
  if (Info) FreePool(Info);
  if (File) File->Close(File);
  Root->Close(Root);
  if (EFI_ERROR(Status) && *Dtb) {FreePool(*Dtb);*Dtb=NULL;}
  return Status;
}
STATIC EFI_STATUS ValidateDtb(CONST VOID *Dtb) {
  INT32 Node,Length,Count,I,Depth=0,Offset=0,Next; CONST UINT8 *Reg;
  UINT32 Tag; BOOLEAN HaveRoot=FALSE;
  UINT64 Address,Size;
  CONST UINT64 Start=W202_OS_INITRD_BASE,LengthNeeded=W202_OS_DTB_BASE+W202_OS_DTB_CAPACITY-W202_OS_INITRD_BASE;
  if (fdt_node_check_compatible(Dtb,0,"sprd,ums9620") || fdt_address_cells(Dtb,0)!=2 ||
      fdt_size_cells(Dtb,0)!=2) return EFI_UNSUPPORTED;
  // Walk the complete structure before writing anything outside UEFI's pool.
  for (;;) {
    Tag=fdt_next_tag(Dtb,Offset,&Next);
    if (Next<0 || Next<=Offset) return EFI_VOLUME_CORRUPTED;
    if (Tag==FDT_BEGIN_NODE) {++Depth;HaveRoot=TRUE;}
    else if (Tag==FDT_END_NODE) {if (--Depth<0) return EFI_VOLUME_CORRUPTED;}
    else if (Tag==FDT_PROP) {if (!Depth) return EFI_VOLUME_CORRUPTED;}
    else if (Tag==FDT_END) {if (Depth || !HaveRoot) return EFI_VOLUME_CORRUPTED;break;}
    else if (Tag!=FDT_NOP) return EFI_VOLUME_CORRUPTED;
    Offset=Next;
  }
  Node=fdt_path_offset(Dtb,"/memory@80000000");
  if (Node<0) return EFI_NOT_FOUND;
  Reg=fdt_getprop(Dtb,Node,"reg",&Length);
  if (!Reg || Length!=16 || Be64(Reg)!=0x80000000 || Be64(Reg+8)<Start+LengthNeeded-0x80000000)
    return EFI_NO_MAPPING;
  Count=fdt_num_mem_rsv(Dtb);if (Count<0) return EFI_VOLUME_CORRUPTED;
  for (I=0;I<Count;++I) {
    if (fdt_get_mem_rsv(Dtb,I,&Address,&Size) || Overlap(Start,LengthNeeded,Address,Size)) return EFI_NO_MAPPING;
  }
  Node=fdt_path_offset(Dtb,"/reserved-memory");
  if (Node<0 || fdt_address_cells(Dtb,Node)!=2 || fdt_size_cells(Dtb,Node)!=2) return EFI_NO_MAPPING;
  fdt_for_each_subnode(Node,Dtb,Node) {
    Reg=fdt_getprop(Dtb,Node,"reg",&Length);
    if (!Reg) {if (Length!=-FDT_ERR_NOTFOUND) return EFI_VOLUME_CORRUPTED;continue;}
    if (Length%16) return EFI_VOLUME_CORRUPTED;
    for (I=0;I<Length;I+=16) if (Overlap(Start,LengthNeeded,Be64(Reg+I),Be64(Reg+I+8))) return EFI_NO_MAPPING;
  }
  return Node==-FDT_ERR_NOTFOUND?EFI_SUCCESS:EFI_VOLUME_CORRUPTED;
}
STATIC EFI_STATUS PrepareDtb(CONST VOID *Source,UINT64 InitrdBytes) {
  VOID *Dtb=(VOID *)(UINTN)W202_OS_DTB_BASE; INT32 Chosen,Length;
  CONST CHAR8 *Args; CHAR8 Bootargs[2048]; UINTN I;
  CONST CHAR8 *Drop[]={"kaslr-seed","rng-seed","linux,uefi-system-table","linux,uefi-mmap-start",
    "linux,uefi-mmap-size","linux,uefi-mmap-desc-size","linux,uefi-mmap-desc-ver"};
  if (fdt_open_into(Source,Dtb,(INT32)W202_OS_DTB_CAPACITY)) return EFI_VOLUME_CORRUPTED;
  Chosen=fdt_path_offset(Dtb,"/chosen");if (Chosen<0) return EFI_NOT_FOUND;
  Args=fdt_getprop(Dtb,Chosen,"bootargs",&Length);
  if (!Args || Length<1 || Length>1980 || Args[Length-1] ||
      !AsciiStrStr(Args,"androidboot.slot_suffix=_a") ||
      !AsciiStrStr(Args,"androidboot.verifiedbootstate=orange")) return EFI_UNSUPPORTED;
  CopyMem(Bootargs,Args,(UINTN)Length);
  AsciiStrCatS(Bootargs,sizeof(Bootargs)," w202.uefi_direct=1 loglevel=5 initcall_debug=0");
  if (fdt_setprop_string(Dtb,Chosen,"bootargs",Bootargs) ||
      fdt_setprop_u64(Dtb,Chosen,"linux,initrd-start",W202_OS_INITRD_BASE) ||
      fdt_setprop_u64(Dtb,Chosen,"linux,initrd-end",W202_OS_INITRD_BASE+InitrdBytes)) return EFI_VOLUME_CORRUPTED;
  for (I=0;I<ARRAY_SIZE(Drop);++I) {
    INT32 Result=fdt_delprop(Dtb,Chosen,Drop[I]);
    if (Result && Result!=-FDT_ERR_NOTFOUND) return EFI_VOLUME_CORRUPTED;
  }
  return fdt_pack(Dtb)?EFI_VOLUME_CORRUPTED:EFI_SUCCESS;
}
#ifdef W202_ANDROID_BOOT_ENTER
STATIC EFI_STATUS ExitToKernel(EFI_HANDLE Image,UINT64 Entry) {
  EFI_MEMORY_DESCRIPTOR *Map; UINTN Capacity=0,Size,Key,DescriptorSize;
  UINT32 Version; EFI_STATUS Status; UINTN Attempt;
  Status=gBS->GetMemoryMap(&Capacity,NULL,&Key,&DescriptorSize,&Version);
  if (Status!=EFI_BUFFER_TOO_SMALL || !DescriptorSize || Capacity>0x100000) return EFI_DEVICE_ERROR;
  Capacity+=32*DescriptorSize; Map=AllocatePool(Capacity);
  if (!Map) return EFI_OUT_OF_RESOURCES;
  for (Attempt=0;Attempt<2;++Attempt) {
    Size=Capacity;
    Status=gBS->GetMemoryMap(&Size,Map,&Key,&DescriptorSize,&Version);
    if (EFI_ERROR(Status)) {
      if (!Attempt) {FreePool(Map);return Status;}
      break;
    }
    Status=gBS->ExitBootServices(Image,Key);
    if (!EFI_ERROR(Status)) W202EnterKernel(Entry,W202_OS_DTB_BASE);
    if (Status!=EFI_INVALID_PARAMETER) break;
  }
  // After attempting ExitBootServices, drivers may already be stopped.
  // Preserve the loader's PMIC recovery watchdog instead of returning to a VM.
  DEBUG((DEBUG_ERROR,"ANDROID_DIRECT exit failed=%r; awaiting recovery\n",Status));
  CpuDeadLoop();return EFI_DEVICE_ERROR;
}
#endif
EFI_STATUS EFIAPI UefiMain(EFI_HANDLE Image,EFI_SYSTEM_TABLE *System) {
  EFI_BLOCK_IO_PROTOCOL *Boot,*Vendor; W202_BOOT_IMAGE B; W202_VENDOR_IMAGE V; W202_KERNEL_IMAGE K;
  EFI_STATUS Status; VOID *Dtb=NULL; UINT8 Header[4096],Table[512];
  UINT64 InitrdBytes,Started=GetPerformanceCounter(); UINT32 KernelCrc,RamdiskCrc; UINT8 *Initrd;
  CONST CHAR8 *Phase="dtb-load";
  DEBUG((DEBUG_WARN,"ANDROID_DIRECT begin; partition reads only\n"));
  Status=LoadDtb(&Dtb);if (EFI_ERROR(Status)) goto Done;
  Phase="dtb-validate";
  Status=ValidateDtb(Dtb);if (EFI_ERROR(Status)) goto Done;
  Phase="boot-partition";
  Status=FindPartition(L"boot_a",&Boot,NULL);if (EFI_ERROR(Status)) goto Done;
  Phase="vendor-partition";
  Status=FindPartition(L"vendor_boot_a",&Vendor,NULL);if (EFI_ERROR(Status)) goto Done;
  Phase="image-headers";
  if (Boot->Media->LastBlock!=131071 || Vendor->Media->LastBlock!=204799) {Status=EFI_UNSUPPORTED;goto Done;}
  Status=Read(Boot,0,sizeof(Header),Header);if (EFI_ERROR(Status)) goto Done;
  Status=W202ParseBoot(Header,sizeof(Header),0x4000000,&B);if (EFI_ERROR(Status)) goto Done;
  Status=Read(Vendor,0,sizeof(Header),Header);if (EFI_ERROR(Status)) goto Done;
  Status=W202ParseVendor(Header,sizeof(Header),0x6400000,&V);if (EFI_ERROR(Status)) goto Done;
  Status=Read(Vendor,V.TableOffset,sizeof(Table),Table);if (EFI_ERROR(Status)) goto Done;
  if (ReadUnaligned32((UINT32 *)Table)!=V.RamdiskBytes || ReadUnaligned32((UINT32 *)(Table+4))!=0 ||
      ReadUnaligned32((UINT32 *)(Table+8))!=1) {Status=EFI_UNSUPPORTED;goto Done;}
  InitrdBytes=V.RamdiskBytes+B.RamdiskBytes+20;
  if (InitrdBytes>W202_OS_INITRD_CAPACITY) {Status=EFI_BAD_BUFFER_SIZE;goto Done;}
  Status=Read(Boot,B.KernelOffset,sizeof(Header),Header);if (EFI_ERROR(Status)) goto Done;
  Status=W202ParseKernel(Header,sizeof(Header),B.KernelBytes,&K);if (EFI_ERROR(Status)) goto Done;
  DEBUG((DEBUG_WARN,"ANDROID_DIRECT load kernel=%Lu initrd=%Lu entry=%lx\n",B.KernelBytes,InitrdBytes,K.Entry));
  Phase="kernel-read";
  Status=Read(Boot,B.KernelOffset,B.KernelBytes,(VOID *)(UINTN)K.Entry);if (EFI_ERROR(Status)) goto Done;
  ZeroMem((VOID *)(UINTN)(K.Entry+B.KernelBytes),(UINTN)(K.MemoryBytes-B.KernelBytes));
  Initrd=(VOID *)(UINTN)W202_OS_INITRD_BASE;
  Phase="vendor-ramdisk-read";
  Status=Read(Vendor,V.RamdiskOffset,V.RamdiskBytes,Initrd);if (EFI_ERROR(Status)) goto Done;
  Phase="generic-ramdisk-read";
  Status=Read(Boot,B.RamdiskOffset,B.RamdiskBytes,Initrd+V.RamdiskBytes);if (EFI_ERROR(Status)) goto Done;
  ZeroMem(Initrd+InitrdBytes-20,8);CopyMem(Initrd+InitrdBytes-12,"#BOOTCONFIG\n",12);
  Phase="data-crc";
  Status=gBS->CalculateCrc32((VOID *)(UINTN)K.Entry,(UINTN)B.KernelBytes,&KernelCrc);if (EFI_ERROR(Status)) goto Done;
  Status=gBS->CalculateCrc32(Initrd,(UINTN)InitrdBytes,&RamdiskCrc);if (EFI_ERROR(Status)) goto Done;
  DEBUG((DEBUG_WARN,"ANDROID_DIRECT data kernel_crc=%08x initrd_crc=%08x\n",KernelCrc,RamdiskCrc));
  // Pins the captured development inputs. CRC is a transfer check, not AVB.
  if (KernelCrc!=0x68c42eb7 || RamdiskCrc!=0xc6a7116c) {Status=EFI_CRC_ERROR;goto Done;}
  Phase="dtb-prepare";
  Status=PrepareDtb(Dtb,InitrdBytes);if (EFI_ERROR(Status)) goto Done;
  FreePool(Dtb);Dtb=NULL;
  DEBUG((DEBUG_WARN,"ANDROID_DIRECT prepared elapsed_ms=%Lu dtb=%lx bytes=%u\n",
    GetTimeInNanoSecond(GetPerformanceCounter()-Started)/1000000,W202_OS_DTB_BASE,
    fdt_totalsize((VOID *)(UINTN)W202_OS_DTB_BASE)));
#ifdef W202_ANDROID_BOOT_ENTER
  Phase="kernel-entry";
  DEBUG((DEBUG_WARN,"ANDROID_DIRECT exit boot services and enter kernel\n"));
  Status=ExitToKernel(Image,K.Entry);
#else
  Phase="preflight-complete";
  Status=EFI_SUCCESS;
  DEBUG((DEBUG_WARN,"ANDROID_DIRECT PREFLIGHT_OK kernel entry disabled\n"));
#endif
Done:
  if (Dtb) FreePool(Dtb);
  DEBUG((DEBUG_WARN,"ANDROID_DIRECT returned phase=%a status=%r\n",Phase,Status));
  return Status;
}
