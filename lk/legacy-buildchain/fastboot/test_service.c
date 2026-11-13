/* SPDX-License-Identifier: MIT
 * Drive the production parser with asynchronous USB and failing raw storage.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define LK_FASTBOOT_TEST
#include "service.c"

uint8_t test_download[65536];
static uint8_t partition[32768], image[32768], other_slot[32768];
static void *pending;
static unsigned pending_bytes, pending_tx, done, actual, writes, reads, stops;
static unsigned mismatch, fail_write, fail_read, fail_queue, link_ready=1, bootloader;
static uint64_t now;
static int start(void) { return 0; }
static void stop_usb(void) { ++stops;pending=NULL; }
static void poll(void) {}
static int ready(void) { return link_ready; }
static int queue(unsigned tx,void *p,unsigned n) {
    if(fail_queue) return -1;
    assert(!pending && !done);pending=p;pending_bytes=n;pending_tx=tx;return 0;
}
static int queue_int(int tx,void *p,unsigned n) { return queue(tx,p,n); }
static int complete(unsigned *n) {
    if(!done) return 0;
    *n=actual;done=0;pending=NULL;return 1;
}
static uint64_t clock_ms(void) { return now; }
static int size(const char *name,uint64_t *bytes) {
    if(strcmp(name,"boot_b") && strcmp(name,"boot_a")) return -1;
    *bytes=sizeof(partition);return 0;
}
static int read_raw(const char *name,uint64_t n,uint64_t off,void *p) {
    assert(!strcmp(name,"boot_b"));assert(off+n<=sizeof(partition));
    ++reads;if(fail_read)return -1;memcpy(p,partition+off,n);
    if(mismatch)((uint8_t *)p)[n-1]^=1;
    return 0;
}
static int write_raw(const char *name,uint64_t n,uint64_t upd,uint64_t off,const void *p) {
    assert(!strcmp(name,"boot_b"));assert(upd==0 && n<=LK_FLASH_CHUNK);
    assert(off+n<=sizeof(partition));++writes;if(fail_write)return -1;
    memcpy(partition+off,p,n);return 0;
}
static void reason(unsigned value) { bootloader=value; }
static int probe_failure;
static int probe(const void *data,uint64_t bytes) {
    assert(data==test_download && bytes==512);
    lk_fb_capture_status("INFO","PJ4EDK2 BEGIN 82ffe000 00000010");
    lk_fb_capture_status("INFO","PJ4EDK2 MEM 82ffe000 00000000000000000000000000000000");
    return probe_failure;
}
const struct lk_fb_ops lk_fb_native={start,stop_usb,poll,ready,queue_int,complete,
    clock_ms,size,read_raw,write_raw,reason,probe};
static uint64_t tick(void) { ++now;return lk_fb_service(W202_LK_FB_POLL); }
static void wait_queue(unsigned tx) {
    for(unsigned i=0;!pending && i<12;++i)tick();
    assert(pending && pending_tx==tx);
}
static void send_bytes(const void *p,unsigned n) {
    wait_queue(0);assert(n<=pending_bytes);memcpy(pending,p,n);done=1;actual=n;tick();
}
static void send_command(const char *p) { send_bytes(p,strlen(p)); }
static uint64_t response(const char *expected) {
    wait_queue(1);assert(pending_bytes>=4 && pending_bytes<=64);
    assert(!memcmp(pending,expected,strlen(expected)));
    done=1;actual=pending_bytes;return tick();
}
static void download(void) {
    send_command("download:00008000");response("DATA00008000");
    send_bytes(image,LK_FLASH_CHUNK);send_bytes(image+LK_FLASH_CHUNK,LK_FLASH_CHUNK);
    response("OKAY");
}
static void reset(void) {
    lk_fb_service(W202_LK_FB_STOP);lk_fb_prepare();
    mismatch=fail_read=fail_write=fail_queue=done=0;pending=NULL;link_ready=1;
    writes=reads=0;memset(partition,0,sizeof(partition));
    assert(lk_fb_service(W202_LK_FB_START)==0);
}
int main(void) {
    for(unsigned i=0;i<sizeof(image);++i)image[i]=i*13;
    memset(other_slot,0xa5,sizeof(other_slot));
    reset();send_command("getvar:version");response("OKAY0.4");
    send_command("getvar:has-slot:boot");response("OKAYno");
    send_command("getvar:partition-size:boot_b");response("OKAY0x0000000000008000");
    send_command("getvar:current-slot");response("FAIL");
    send_command("download:08000001");response("FAIL");
    send_command("download:00000000");response("FAIL");
    send_command("download:00000z01");response("FAIL");
    download();send_command("flash:boot_b");response("OKAY");
    assert(writes==2 && reads==2 && !memcmp(partition,image,sizeof(image)));
    for(unsigned i=0;i<sizeof(other_slot);++i)assert(other_slot[i]==0xa5);
    send_command("flash:boot_b");response("FAIL");assert(writes==2);
    const char *bad[]={"boot","uboot","esp","boot_c","boot_b/../../x","boot_b:extra","splloader"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        char cmd[64];download();snprintf(cmd,sizeof(cmd),"flash:%s",bad[i]);
        send_command(cmd);response("FAIL");assert(writes==2);
    }
    reset();send_command("download:00000200");response("DATA");
    send_bytes(image,512);response("OKAY");send_command("flash:boot_b");response("FAIL");
    assert(!writes);
    reset();image[0]=0x3a;image[1]=0xff;image[2]=0x26;image[3]=0xed;
    download();send_command("flash:boot_b");response("FAIL");assert(!writes);
    image[0]=0;image[1]=13;image[2]=26;image[3]=39;
    reset();download();mismatch=1;send_command("flash:boot_b");response("FAIL");
    assert(writes==1 && reads==1);
    reset();download();fail_write=1;send_command("flash:boot_b");response("FAIL");
    assert(writes==1 && !reads);
    reset();download();fail_read=1;send_command("flash:boot_b");response("FAIL");
    assert(writes==1 && reads==1);
    reset();send_command("download:00008000");response("DATA");
    send_bytes(image,512);link_ready=0;pending=NULL;done=0;tick();link_ready=1;
    send_command("flash:boot_b");response("FAIL");assert(!writes);
    reset();send_command("download:00008000");response("DATA");wait_queue(0);
    now+=31000;assert(tick()==UINT64_MAX);assert(!s->active && !s->downloaded);
    reset();send_command("download:00000001");response("DATA");
    wait_queue(0);pending_bytes=512;memset(pending,0,512);actual=512;done=1;tick();
    response("FAIL");assert(!s->downloaded);
    reset();send_command("reboot-bootloader");assert(response("OKAY")==W202_LK_FB_BOOTLOADER && bootloader==1);
    send_command("reboot");assert(response("OKAY")==W202_LK_FB_REBOOT && bootloader==0);
    send_command("continue");assert(response("OKAY")==W202_LK_FB_CONTINUE);
    reset();send_bytes("flash:boot_b\0bad",16);response("FAIL");assert(!writes);
    reset();fail_queue=1;assert(tick()==UINT64_MAX && !s->active);
    reset();send_command("download:00000200");response("DATA");
    send_bytes(image,512);response("OKAY");
    send_command("oem jump-rmk :probe");
    response("INFOPJ4EDK2 BEGIN");response("INFOPJ4EDK2 MEM");response("OKAY");
    assert(!s->downloaded && !writes && !reads);
    send_command("getvar:version");response("OKAY0.4");
    send_command("oem jump-rmk :init");response("FAIL");
    reset();send_command("download:00000200");response("DATA");
    send_bytes(image,512);response("OKAY");probe_failure=1;
    send_command("oem jump-rmk :probe");
    response("INFO");response("INFO");response("FAIL");probe_failure=0;
    reset();s->output_count=PROBE_LINES;
    lk_fb_capture_status("INFO","overflow");assert(s->output_overflow);
    puts("Fastboot asynchronous download, full flash, exact slot, readback and fault cases passed");
    return 0;
}
