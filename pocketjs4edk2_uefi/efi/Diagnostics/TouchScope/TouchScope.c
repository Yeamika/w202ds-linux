// SPDX-License-Identifier: BSD-2-Clause-Patent
// Optional direct GOP/AbsolutePointer diagnostic, outside the JS page.
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/AbsolutePointer.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/TimerLib.h>
#define PJB_EDK2
#include <pocketjs_boot_context.h>

STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL *mPixels;
STATIC UINTN mWidth,mHeight;
STATIC VOID Dot(INTN X,INTN Y,EFI_GRAPHICS_OUTPUT_BLT_PIXEL Color) {
  INTN I,J;
  for (J=Y-3;J<=Y+3;++J) for (I=X-3;I<=X+3;++I)
    if (I>=0 && J>=0 && I<(INTN)mWidth && J<(INTN)mHeight)
      mPixels[J*mWidth+I]=Color;
}
STATIC VOID Line(INTN X,INTN Y,INTN EndX,INTN EndY,EFI_GRAPHICS_OUTPUT_BLT_PIXEL Color) {
  INTN Dx=ABS(EndX-X), Sx=X<EndX?1:-1;
  INTN Dy=-ABS(EndY-Y), Sy=Y<EndY?1:-1, Err=Dx+Dy,E2;
  for (;;) {
    Dot(X,Y,Color);
    if (X==EndX && Y==EndY) break;
    E2=2*Err;
    if (E2>=Dy) { Err+=Dy; X+=Sx; }
    if (E2<=Dx) { Err+=Dx; Y+=Sy; }
  }
}
EFI_STATUS EFIAPI TouchScopeMain(EFI_HANDLE Image,EFI_SYSTEM_TABLE *Table) {
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop=NULL;
  EFI_ABSOLUTE_POINTER_PROTOCOL *Touch=NULL;
  EFI_ABSOLUTE_POINTER_STATE State;
  PJB_PLATFORM_PROTOCOL *Platform=NULL;
  PJB_GUID Wire=PJB_PLATFORM_GUID;
  EFI_GUID Guid;
  EFI_STATUS Status,InputStatus,StopStatus;
  EFI_EVENT Events[3],Wake=NULL;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Ink={0xff,0xff,0x30,0};
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Pulse={0,0xff,0,0};
  UINTN X=0,Y=0,LastX=0,LastY=0,X0,Y0,X1,Y1,Count=0;
  UINTN Presents=0,Selected;
  UINT64 Start,Now,LastLog,LastClear=0;
  BOOLEAN Down=FALSE,Scan=TRUE,HaveClear=FALSE;
  if (!Table || !Table->ConIn || !Table->ConIn->WaitForKey)
    return EFI_INVALID_PARAMETER;
  CopyMem(&Guid,&Wire,sizeof(Guid));
  Status=gBS->LocateProtocol(&Guid,NULL,(VOID **)&Platform);
  if (EFI_ERROR(Status)) return Status;
  Status=Platform->set_quiesced(Platform,0);
  if (EFI_ERROR(Status)) return Status;
  Status=gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&Gop);
  if (EFI_ERROR(Status)) goto Done;
  Status=gBS->HandleProtocol(Table->ConsoleInHandle,&gEfiAbsolutePointerProtocolGuid,(VOID **)&Touch);
  if (EFI_ERROR(Status)) goto Done;
  if (!Touch || !Touch->WaitForInput || !Touch->Mode ||
      !Touch->Mode->AbsoluteMaxX || !Touch->Mode->AbsoluteMaxY) { Status=EFI_UNSUPPORTED; goto Done; }
  mWidth=Gop->Mode->Info->HorizontalResolution;mHeight=Gop->Mode->Info->VerticalResolution;
  if (mWidth!=1200 || mHeight!=1920) {Status=EFI_UNSUPPORTED;goto Done;}
  mPixels=AllocateZeroPool(mWidth*mHeight*sizeof(*mPixels));
  if (!mPixels) {Status=EFI_OUT_OF_RESOURCES;goto Done;}
  Status=Gop->Blt(Gop,mPixels,EfiBltBufferToVideo,0,0,0,0,mWidth,mHeight,0);
  if (EFI_ERROR(Status)) goto Free;
  Print(L"TOUCH TRACE - drag to draw\r\nVolume down: clear; twice: return.\r\nVolume up: scan on/off. Green pulse: on, blue: off.\r\n");
  Status=gBS->CreateEvent(EVT_TIMER,TPL_APPLICATION,NULL,NULL,&Wake);
  if (EFI_ERROR(Status)) goto Free;
  Status=gBS->SetTimer(Wake,TimerPeriodic,200000);
  if (EFI_ERROR(Status)) goto Free;
  Events[0]=Table->ConIn->WaitForKey;
  Events[1]=Touch->WaitForInput;
  Events[2]=Wake;
  DEBUG((DEBUG_WARN,"TOUCH_SCOPE started %ux%u scan=1 period_ms=20 event-wait IRQ-only SPI\n",mWidth,mHeight));
  Start=GetTimeInNanoSecond(GetPerformanceCounter());
  LastLog=Start;
  for (;;) {
    EFI_INPUT_KEY Key;
    Status=gBS->WaitForEvent(ARRAY_SIZE(Events),Events,&Selected);
    if (EFI_ERROR(Status)) break;
    if (Selected>=ARRAY_SIZE(Events)) {Status=EFI_DEVICE_ERROR;break;}
    Now=GetTimeInNanoSecond(GetPerformanceCounter());
    if (Selected==0) {
      InputStatus=Table->ConIn->ReadKeyStroke(Table->ConIn,&Key);
      if (InputStatus==EFI_NOT_READY) continue;
      if (EFI_ERROR(InputStatus)) {Status=InputStatus;break;}
      if (Key.ScanCode==SCAN_ESC) {Status=EFI_SUCCESS;break;}
      if (Key.ScanCode==SCAN_UP) {
        Scan=!Scan;
        /* The timer only presents the pulse/logs. It never reads input or SPI.
         * Static mode sleeps until input, with one log wake every ten seconds. */
        Status=gBS->SetTimer(Wake,TimerPeriodic,Scan?200000:100000000);
        if (EFI_ERROR(Status)) break;
        Pulse.Blue=Scan?0:0xff;Pulse.Green=Scan?0xff:0;Pulse.Red=0;
        Status=Gop->Blt(Gop,&Pulse,EfiBltVideoFill,0,0,mWidth-48,96,32,32,0);
        if (EFI_ERROR(Status)) break;
        DEBUG((DEBUG_WARN,"TOUCH_SCOPE scan=%u events=%u presents=%u\n",Scan,Count,Presents));
      }
      if (Key.ScanCode==SCAN_DOWN) {
        if (HaveClear && Now-LastClear<=800000000) {Status=EFI_SUCCESS;break;}
        LastClear=Now;HaveClear=TRUE;
        ZeroMem(mPixels,mWidth*mHeight*sizeof(*mPixels));Down=FALSE;
        Status=Gop->Blt(Gop,mPixels,EfiBltBufferToVideo,0,0,0,0,mWidth,mHeight,0);
        if (EFI_ERROR(Status)) break;
      }
      continue;
    }
    if (Selected==2) {
      /* Present from the application only; there is no GOP reentry callback. */
      if (Scan) {
        Pulse.Green=((Now-Start)/500000000)&1 ? 0xff : 0x60;
        Status=Gop->Blt(Gop,&Pulse,EfiBltVideoFill,0,0,mWidth-48,96,32,32,0);
        if (EFI_ERROR(Status)) break;
        ++Presents;
      }
      if (Now-LastLog>=10000000000ULL) {
        DEBUG((DEBUG_WARN,"TOUCH_SCOPE elapsed_ms=%Lu scan=%u events=%u presents=%u\n",
          (Now-Start)/1000000,Scan,Count,Presents));
        LastLog=Now;
      }
      continue;
    }
    /* Only WaitForInput permits consuming the IRQ-fed queue. An empty/stale
     * event returns to WaitForEvent rather than starting a polling fallback. */
    InputStatus=Touch->GetState(Touch,&State);
    if (InputStatus==EFI_NOT_READY) continue;
    if (EFI_ERROR(InputStatus)) {Down=FALSE;continue;}
    ++Count;
    if (!(State.ActiveButtons&EFI_ABSP_TouchActive)) {
      Down=FALSE;
      continue;
    }
    if (State.CurrentX>Touch->Mode->AbsoluteMaxX || State.CurrentY>Touch->Mode->AbsoluteMaxY) {
      Down=FALSE;
      continue;
    }
    X=(UINTN)(State.CurrentX*(mWidth-1)/Touch->Mode->AbsoluteMaxX);
    Y=(UINTN)(State.CurrentY*(mHeight-1)/Touch->Mode->AbsoluteMaxY);
    if (X>=mWidth || Y>=mHeight) {Down=FALSE;continue;}
    if (!Down) {LastX=X;LastY=Y;}
    Line(LastX,LastY,X,Y,Ink);
    X0=MIN(LastX,X);X0=X0>3?X0-3:0;
    Y0=MIN(LastY,Y);Y0=Y0>3?Y0-3:0;
    X1=MIN(MAX(LastX,X)+4,mWidth);Y1=MIN(MAX(LastY,Y)+4,mHeight);
    Status=Gop->Blt(Gop,mPixels,EfiBltBufferToVideo,X0,Y0,X0,Y0,X1-X0,Y1-Y0,mWidth*4);
    if (EFI_ERROR(Status)) break;
    Down=TRUE;LastX=X;LastY=Y;
    if (Count<=16) DEBUG((DEBUG_WARN,"TOUCH_SCOPE event=%u xy=%u,%u down=%u\n",Count,X,Y,Down));
  }
Free:
  if (Wake) {
    StopStatus=gBS->SetTimer(Wake,TimerCancel,0);
    if (!EFI_ERROR(Status) && EFI_ERROR(StopStatus)) Status=StopStatus;
    StopStatus=gBS->CloseEvent(Wake);
    if (!EFI_ERROR(Status) && EFI_ERROR(StopStatus)) Status=StopStatus;
  }
  FreePool(mPixels);
  mPixels=NULL;
Done:
  StopStatus=Platform->set_quiesced(Platform,1);
  if (!EFI_ERROR(Status) && EFI_ERROR(StopStatus)) Status=StopStatus;
  return Status;
}
