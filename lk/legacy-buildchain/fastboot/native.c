/* SPDX-License-Identifier: MIT
 * Polled adapter for the pinned LK DWC3 gadget. Never calls do_fastboot,
 * usb_fastboot_start, fb_usb_read/write or any native splash/LCD entry point.
 */
#include "service.h"
#include "native.h"
#include "flash.h"

#define WORD(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define PTR(a) (*(uintptr_t *)(uintptr_t)(a))
#define USB_STATE 0xb52aa828u
#define FBDEV (USB_STATE+8)

static void stop(void) {
    if(PTR(0xb50ffc20)) ((void (*)(void))0xb5085770)();
    PTR(0xb50ffc20)=0;
}
static int start(void) {
    int (*const init)(unsigned)=(void *)0xb5085724;
    int (*const bind)(void *)=(void *)0xb508674c;
    /* A RAM invocation may inherit an already connected native gadget. */
    stop();
    for(unsigned i=0;i<0x58;++i) *(volatile uint8_t *)(uintptr_t)(FBDEV+i)=0;
    if(init(3)) { PTR(0xb50ffc20)=0;return -1; }
    return bind((void *)0xb50ffb58);
}
static void poll(void) { ((void (*)(void))0xb50857b4)(); }
static int ready(void) {
    /* config, not network_started: native disconnect clears config only. */
    return *(volatile uint8_t *)(uintptr_t)(FBDEV+0x18)!=0;
}
static int queue(int tx,void *buffer,unsigned bytes) {
    uintptr_t ep=PTR(USB_STATE+(tx?0x28:0x30));
    uintptr_t req=PTR(USB_STATE+(tx?0x48:0x50));
    if(!ep || !req) return -1;
    unsigned packet=*(uint16_t *)(ep+0x28);
    if(!packet || (packet&(packet-1)) || packet>1024) return -1;
    /* RX uses a rounded DMA length and the dedicated 16K IO buffer. The
     * parser checks actual against the unrounded command/download length. */
    unsigned count=tx?bytes:(bytes+packet-1)&~(packet-1);
    if(count>LK_FLASH_CHUNK) return -1;
    PTR(req)= (uintptr_t)buffer;WORD(req+8)=count;
    WORD(USB_STATE)=0;WORD(USB_STATE+4)=0;WORD(req+0x44)=0;
    int (*submit)(void *,void *,unsigned)=(void *)PTR(PTR(ep+0x10)+0x20);
    return submit((void *)ep,(void *)req,0);
}
/* TX and RX share txn_done. service.c guarantees one outstanding request. */
static int complete(unsigned *actual) {
    if(!WORD(USB_STATE+4)) return 0;
    if((int32_t)WORD(USB_STATE)<0) return -1;
    /* The completed req is selected by queue() in a reserved context word. */
    *actual=WORD(PTR(0x86408000+0x3f0)+0x44);
    return 1;
}
static int queue_record(int tx,void *buffer,unsigned bytes) {
    PTR(0x86408000+0x3f0)=PTR(USB_STATE+(tx?0x48:0x50));
    return queue(tx,buffer,bytes);
}
static uint64_t clock_ms(void) {
    uint64_t count,freq;
    __asm__ volatile("mrs %0, cntpct_el0":"=r"(count));
    __asm__ volatile("mrs %0, cntfrq_el0":"=r"(freq));
    return freq?count/(freq/1000):0;
}
static void reset_reason(unsigned bootloader) {
    uint32_t (*const read)(uint32_t)=(void *)0xb506ce54;
    void (*const write)(uint32_t,uint32_t,uint32_t)=(void *)0xb506cf70;
    write(0x644223ac,(read(0x644223ac)&~255u)|(bootloader?0x30:0x40),1);
}
const struct lk_fb_ops lk_fb_native={start,stop,poll,ready,queue_record,complete,
    clock_ms,lk_partition_size,lk_raw_read,lk_raw_write,reset_reason};
