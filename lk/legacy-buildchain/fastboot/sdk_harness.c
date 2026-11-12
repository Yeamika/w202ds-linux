/* SPDX-License-Identifier: MIT
 * Host transport for testing the production engine with an unmodified SDK
 * client. No physical USB or block device is opened by this harness.
 */
#include <string.h>
#define LK_FASTBOOT_TEST
#include "service.c"
uint8_t test_download[LK_FB_DOWNLOAD_MAX];
static uint8_t partition[0x500000];
static void *pending;
static unsigned tx, capacity, completed, transferred, writes;
static uint64_t ticks;
static int start(void) { return 0; }
static void stop_usb(void) { pending=0;completed=0; }
static void poll(void) {}
static int ready(void) { return 1; }
static int queue(int direction,void *p,unsigned n) {
    if(pending)return -1;
    pending=p;capacity=n;tx=direction;return 0;
}
static int complete(unsigned *n) {
    if(!completed)return 0;
    *n=transferred;pending=0;completed=0;return 1;
}
static uint64_t clock_ms(void) { return ticks; }
static int size(const char *name,uint64_t *n) {
    if(strcmp(name,"uboot_b") && strcmp(name,"uboot_a"))return -1;
    *n=sizeof(partition);return 0;
}
static int read_raw(const char *name,uint64_t n,uint64_t off,void *p) {
    if(strcmp(name,"uboot_b") || off+n>sizeof(partition))return -1;
    memcpy(p,partition+off,n);return 0;
}
static int write_raw(const char *name,uint64_t n,uint64_t upd,uint64_t off,const void *p) {
    if(strcmp(name,"uboot_b") || upd || off+n>sizeof(partition))return -1;
    memcpy(partition+off,p,n);++writes;return 0;
}
static void reason(unsigned bootloader) { (void)bootloader; }
const struct lk_fb_ops lk_fb_native={start,stop_usb,poll,ready,queue,complete,
    clock_ms,size,read_raw,write_raw,reason};
void host_reset(void) {
    stop_usb();lk_fb_prepare();lk_fb_service(W202_LK_FB_START);
}
unsigned host_pending(void) {
    for(unsigned i=0;!pending && i<4096;++i) {
        ++ticks;if(lk_fb_service(W202_LK_FB_POLL)==UINT64_MAX)return 0;
    }
    return pending?capacity|(tx?0x80000000u:0):0;
}
int host_receive(const void *data,unsigned n) {
    if(!pending || tx || n>capacity)return -1;
    memcpy(pending,data,n);transferred=n;completed=1;
    lk_fb_service(W202_LK_FB_POLL);return 0;
}
unsigned host_transmit(void *data) {
    if(!pending || !tx)return 0;
    memcpy(data,pending,capacity);transferred=capacity;completed=1;
    unsigned n=capacity;lk_fb_service(W202_LK_FB_POLL);return n;
}
unsigned host_writes(void) { return writes; }
int host_matches(void) { return !memcmp(partition,test_download,sizeof(partition)); }
