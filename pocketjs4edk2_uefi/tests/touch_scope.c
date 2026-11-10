/* Execute the real diagnostic application against queued EFI input events.
 * Timer-only wakes must never consume input. Pixel checks cover gesture gaps,
 * release coordinates, malformed reports and clear/return behavior. */
#include "../efi/Diagnostics/TouchScope/TouchScope.c"
extern int printf(const char *, ...) __attribute__((visibility("default")));
#define CHECK(x) do { if (!(x)) { printf("touch_scope.c:%d: %s\n",__LINE__,#x);return 1; } } while (0)
#define KEY_EVENT ((EFI_EVENT)(UINTN)1)
#define TOUCH_EVENT ((EFI_EVENT)(UINTN)2)
#define TIMER_EVENT ((EFI_EVENT)(UINTN)3)
#define NS_MS(n) ((UINT64)(n)*1000000)
typedef struct {
  UINTN selected;
  UINT64 ns;
  EFI_STATUS status;
  UINT64 x,y;
  UINT32 buttons;
  UINT16 key;
} STEP;
#define KEY(k,ms) {.selected=0,.ns=NS_MS(ms),.key=(k)}
#define TOUCH(xv,yv,b,ms) {.selected=1,.ns=NS_MS(ms),.x=(xv),.y=(yv),.buttons=(b)}
#define TICK(ms) {.selected=2,.ns=NS_MS(ms)}
static EFI_GRAPHICS_OUTPUT_BLT_PIXEL pixels[1200*1920];
static const STEP *steps,*current;
static UINTN step_count,next,waits,reads,keys,blits,fills,allocations,frees;
static UINTN creates,closes,starts,stops,stalls,unexpected,timer_count,cancels;
static UINTN fail_blit,fail_timer;
static EFI_STATUS create_error,stop_error;
static UINT64 now,periods[16];
static BOOLEAN consumed;
EFI_GUID gEfiGraphicsOutputProtocolGuid,gEfiAbsolutePointerProtocolGuid;

BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) { return FALSE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) {}
UINTN EFIAPI Print(CONST CHAR16 *Format, ...) { return 0; }
VOID *EFIAPI ZeroMem(VOID *Buffer,UINTN Size) {
  UINT8 *p=Buffer;while(Size--)*p++=0;return Buffer;
}
VOID *EFIAPI CopyMem(VOID *Destination,CONST VOID *Source,UINTN Size) {
  UINT8 *d=Destination;const UINT8 *s=Source;while(Size--)*d++=*s++;return Destination;
}
VOID *EFIAPI AllocateZeroPool(UINTN Size) {
  ++allocations;
  if(Size!=sizeof(pixels)) {++unexpected;return NULL;}
  return ZeroMem(pixels,sizeof(pixels));
}
VOID EFIAPI FreePool(VOID *Buffer) { if(Buffer!=pixels)++unexpected;++frees; }
UINT64 EFIAPI GetPerformanceCounter(VOID) { return now; }
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 Ticks) { return Ticks; }

static PJB_STATUS EFIAPI Quiet(VOID *This,UINT32 Quiesced) {
  if(Quiesced) {++stops;return stop_error;}
  ++starts;return EFI_SUCCESS;
}
static PJB_PLATFORM_PROTOCOL platform={.set_quiesced=Quiet};
static EFI_STATUS EFIAPI Blit(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Buffer,EFI_GRAPHICS_OUTPUT_BLT_OPERATION Operation,
    UINTN SourceX,UINTN SourceY,UINTN DestinationX,UINTN DestinationY,
    UINTN Width,UINTN Height,UINTN Delta) {
  ++blits;
  if(fail_blit==blits)return EFI_DEVICE_ERROR;
  if(Operation==EfiBltVideoFill) {
    ++fills;
    if(Width!=32 || Height!=32 || DestinationX!=1152 || DestinationY!=96)++unexpected;
  } else if(Operation!=EfiBltBufferToVideo || Buffer!=pixels ||
      DestinationX+Width>1200 || DestinationY+Height>1920) ++unexpected;
  return EFI_SUCCESS;
}
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info={
  .HorizontalResolution=1200,.VerticalResolution=1920,.PixelsPerScanLine=1200};
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode={.Info=&info};
static EFI_GRAPHICS_OUTPUT_PROTOCOL gop={.Blt=Blit,.Mode=&mode};
static EFI_STATUS EFIAPI Pointer(EFI_ABSOLUTE_POINTER_PROTOCOL *This,
                                 EFI_ABSOLUTE_POINTER_STATE *State) {
  ++reads;
  if(!current || current->selected!=1 || consumed) {++unexpected;return EFI_DEVICE_ERROR;}
  consumed=TRUE;
  if(EFI_ERROR(current->status))return current->status;
  State->CurrentX=current->x;State->CurrentY=current->y;State->CurrentZ=0;
  State->ActiveButtons=current->buttons;return EFI_SUCCESS;
}
static EFI_ABSOLUTE_POINTER_MODE pointer_mode={.AbsoluteMaxX=1199,.AbsoluteMaxY=1919};
static EFI_ABSOLUTE_POINTER_PROTOCOL pointer={
  .GetState=Pointer,.WaitForInput=TOUCH_EVENT,.Mode=&pointer_mode};
static EFI_STATUS EFIAPI Key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,EFI_INPUT_KEY *Key) {
  ++keys;
  if(!current || current->selected!=0 || consumed) {++unexpected;return EFI_DEVICE_ERROR;}
  consumed=TRUE;
  Key->ScanCode=current->key;Key->UnicodeChar=0;return current->status;
}
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL input={.ReadKeyStroke=Key,.WaitForKey=KEY_EVENT};
static EFI_SYSTEM_TABLE table={.ConIn=&input,.ConsoleInHandle=(EFI_HANDLE)(UINTN)5};
static EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,VOID *Registration,VOID **Interface) {
  *Interface=Guid==&gEfiGraphicsOutputProtocolGuid ? (VOID *)&gop : (VOID *)&platform;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE Handle,EFI_GUID *Guid,VOID **Interface) {
  if(Handle!=table.ConsoleInHandle || Guid!=&gEfiAbsolutePointerProtocolGuid)++unexpected;
  *Interface=&pointer;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Create(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,
                                VOID *Context,EFI_EVENT *Event) {
  ++creates;
  if(Type!=EVT_TIMER || Notify!=NULL || Context!=NULL)++unexpected;
  if(EFI_ERROR(create_error))return create_error;
  *Event=TIMER_EVENT;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Timer(EFI_EVENT Event,EFI_TIMER_DELAY Type,UINT64 Trigger) {
  if(Event!=TIMER_EVENT)++unexpected;
  if(Type==TimerCancel) {++cancels;return EFI_SUCCESS;}
  if(Type!=TimerPeriodic || timer_count==ARRAY_SIZE(periods)) {++unexpected;return EFI_DEVICE_ERROR;}
  periods[timer_count++]=Trigger;
  return fail_timer==timer_count ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Close(EFI_EVENT Event) {
  if(Event!=TIMER_EVENT)++unexpected;++closes;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Wait(UINTN Number,EFI_EVENT *Events,UINTN *Selected) {
  ++waits;
  if(Number!=3 || Events[0]!=KEY_EVENT || Events[1]!=TOUCH_EVENT || Events[2]!=TIMER_EVENT)
    {++unexpected;return EFI_INVALID_PARAMETER;}
  if(next==step_count) {++unexpected;return EFI_ABORTED;}
  current=&steps[next++];consumed=FALSE;now=current->ns;
  if(current->selected==3)return current->status;
  *Selected=current->selected;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Stall(UINTN Microseconds) {++stalls;return EFI_SUCCESS;}
static EFI_BOOT_SERVICES bs={.LocateProtocol=Locate,.HandleProtocol=Handle,.CreateEvent=Create,
  .SetTimer=Timer,.CloseEvent=Close,.WaitForEvent=Wait,.Stall=Stall};
EFI_BOOT_SERVICES *gBS=&bs;
static VOID Scenario(const STEP *Steps,UINTN Count) {
  steps=Steps;step_count=Count;current=NULL;next=0;now=0;consumed=FALSE;
  waits=reads=keys=blits=fills=allocations=frees=creates=closes=starts=stops=stalls=unexpected=0;
  timer_count=cancels=fail_blit=fail_timer=0;create_error=stop_error=EFI_SUCCESS;
}
static BOOLEAN InkAt(UINTN X,UINTN Y) {return pixels[Y*1200+X].Blue!=0;}
int main(void) {
  static const STEP idle[]={TICK(20),TICK(40),TICK(60),KEY(SCAN_ESC,70)};
  Scenario(idle,ARRAY_SIZE(idle));
  CHECK(TouchScopeMain(NULL,&table)==EFI_SUCCESS);
  CHECK(waits==4 && reads==0 && keys==1 && stalls==0 && fills==3 && blits==4);
  CHECK(starts==1 && stops==1 && allocations==1 && frees==1 && creates==1 && closes==1 && cancels==1);
  CHECK(periods[0]==200000 && mPixels==NULL && unexpected==0);

  static const STEP draw[]={TOUCH(100,200,1,10),TOUCH(120,220,1,20),
    TOUCH(0,0,0,30),TOUCH(600,600,1,40),KEY(SCAN_ESC,50)};
  Scenario(draw,ARRAY_SIZE(draw));
  CHECK(TouchScopeMain(NULL,&table)==EFI_SUCCESS);
  CHECK(reads==4 && blits==4 && fills==0 && InkAt(100,200) && InkAt(110,210) && InkAt(600,600));
  CHECK(!InkAt(0,0) && !InkAt(300,350) && unexpected==0);

  static const STEP gap[]={TOUCH(100,200,1,10),
    {.selected=1,.ns=NS_MS(20),.status=EFI_DEVICE_ERROR},
    TOUCH(600,600,1,30),TOUCH(MAX_UINT64,MAX_UINT64,1,40),
    TOUCH(900,900,1,50),KEY(SCAN_ESC,60)};
  Scenario(gap,ARRAY_SIZE(gap));
  CHECK(TouchScopeMain(NULL,&table)==EFI_SUCCESS);
  CHECK(reads==5 && blits==4 && !InkAt(300,350) && !InkAt(750,750) && unexpected==0);

  static const STEP stale[]={
    {.selected=0,.ns=NS_MS(10),.status=EFI_NOT_READY},
    {.selected=1,.ns=NS_MS(20),.status=EFI_NOT_READY},KEY(SCAN_ESC,30)};
  Scenario(stale,ARRAY_SIZE(stale));
  CHECK(TouchScopeMain(NULL,&table)==EFI_SUCCESS);
  CHECK(waits==3 && keys==2 && reads==1 && blits==1 && stalls==0 && unexpected==0);

  static const STEP toggle[]={KEY(SCAN_UP,10),TICK(10010),KEY(SCAN_UP,10020),TICK(10040),KEY(SCAN_ESC,10050)};
  Scenario(toggle,ARRAY_SIZE(toggle));
  CHECK(TouchScopeMain(NULL,&table)==EFI_SUCCESS);
  CHECK(timer_count==3 && periods[0]==200000 && periods[1]==100000000 && periods[2]==200000);
  CHECK(fills==3 && reads==0 && keys==3 && blits==4 && unexpected==0);

  static const STEP clear[]={TOUCH(100,200,1,10),KEY(SCAN_DOWN,20),TOUCH(600,600,1,30),KEY(SCAN_DOWN,600)};
  Scenario(clear,ARRAY_SIZE(clear));
  CHECK(TouchScopeMain(NULL,&table)==EFI_SUCCESS);
  CHECK(!InkAt(100,200) && InkAt(600,600) && blits==4 && reads==2 && keys==2 && unexpected==0);

  static const STEP failed_wait[]={{.selected=3,.status=EFI_TIMEOUT}};
  Scenario(failed_wait,ARRAY_SIZE(failed_wait));
  CHECK(TouchScopeMain(NULL,&table)==EFI_TIMEOUT);
  CHECK(closes==1 && cancels==1 && frees==1 && stops==1 && unexpected==0);

  Scenario(idle,ARRAY_SIZE(idle));create_error=EFI_OUT_OF_RESOURCES;
  CHECK(TouchScopeMain(NULL,&table)==EFI_OUT_OF_RESOURCES);
  CHECK(waits==0 && closes==0 && timer_count==0 && frees==1 && stops==1 && unexpected==0);

  Scenario(idle,ARRAY_SIZE(idle));fail_timer=1;
  CHECK(TouchScopeMain(NULL,&table)==EFI_DEVICE_ERROR);
  CHECK(waits==0 && closes==1 && cancels==1 && frees==1 && stops==1 && unexpected==0);

  Scenario(toggle,ARRAY_SIZE(toggle));fail_timer=2;
  CHECK(TouchScopeMain(NULL,&table)==EFI_DEVICE_ERROR);
  CHECK(keys==1 && reads==0 && closes==1 && frees==1 && stops==1 && unexpected==0);

  Scenario(draw,ARRAY_SIZE(draw));fail_blit=2;
  CHECK(TouchScopeMain(NULL,&table)==EFI_DEVICE_ERROR);
  CHECK(reads==1 && closes==1 && frees==1 && stops==1 && unexpected==0);

  Scenario(idle,ARRAY_SIZE(idle));stop_error=EFI_ACCESS_DENIED;
  CHECK(TouchScopeMain(NULL,&table)==EFI_ACCESS_DENIED);
  CHECK(closes==1 && frees==1 && stops==1 && unexpected==0);
  Scenario(failed_wait,ARRAY_SIZE(failed_wait));stop_error=EFI_ACCESS_DENIED;
  CHECK(TouchScopeMain(NULL,&table)==EFI_TIMEOUT);
  CHECK(closes==1 && frees==1 && stops==1 && unexpected==0);
  printf("TOUCH_SCOPE_EVENT_DISPATCH_GESTURES_AND_CLEANUP_PASS scenarios=13\n");
  return 0;
}
