/* Actual handoff C against a locked PMIC: preserve unrelated bits and fail bounded. */
#include "../efi/LinuxBoot/LinuxHandoff.c"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
static UINT32 address,control=0xe;
static unsigned locked=1,changes,bad,busy,fifo_busy,delays;
UINT32 EFIAPI MmioRead32(UINTN a) {
  if (a==0x64400030) return fifo_busy?0:BIT10;
  if (a==0x6440002c) return busy?BIT31:(address==0x64420048?control:0);
  ++bad;return 0;
}
UINT32 EFIAPI MmioWrite32(UINTN a,UINT32 v) {
  if (a==0x64400028) address=v;
  else if (a==0x64420060) locked=v!=0xe551;
  else if (a==0x64420048 && !locked) {control=v;++changes;}
  else ++bad;
  return v;
}
UINTN EFIAPI MicroSecondDelay(UINTN us) {delays+=(unsigned)us;return us;}
int main(void) {
  CHECK(LinuxStopWatchdog()==EFI_SUCCESS && control==0xc && locked && changes==1 && !bad);
  CHECK(LinuxStopWatchdog()==EFI_SUCCESS && changes==1);
  busy=1;control=0xa;
  CHECK(LinuxStopWatchdog()==EFI_NOT_READY && control==0xa && locked && changes==1);
  busy=0;fifo_busy=1;
  CHECK(LinuxStopWatchdog()==EFI_TIMEOUT && control==0xa && locked && changes==1 && delays==4000);
  return 0;
}
