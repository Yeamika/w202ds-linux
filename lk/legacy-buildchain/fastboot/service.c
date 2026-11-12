/* SPDX-License-Identifier: MIT
 * Serialized, polled Fastboot 0.4. Each call queues one USB request, consumes
 * one completion, or writes and compares one storage chunk. No host wait loop,
 * heap, display, filesystem access or persistent slot selection.
 */
#include "service.h"
#include "flash.h"
#include "LkFastboot.h"

enum { IDLE, RX_COMMAND, TX_REPLY, RX_DATA, FLASH_IMAGE };
struct fb_state {
    unsigned active, phase, next_phase, queued, action, tx_bytes, queue_bytes, connected;
    uint64_t deadline, expected, received, downloaded, offset;
    char name[36];
    uint8_t tx[64] __attribute__((aligned(64)));
    uint8_t io[LK_FLASH_CHUNK] __attribute__((aligned(64)));
    uint8_t verify[LK_FLASH_CHUNK] __attribute__((aligned(64)));
};
_Static_assert(sizeof(struct fb_state)<=0x10000,"Fastboot state reservation");
#ifdef LK_FASTBOOT_TEST
static struct fb_state storage;
static struct fb_state *const s = &storage;
extern uint8_t test_download[];
#define DOWNLOAD test_download
#else
static struct fb_state *const s = (void *)0x86410000;
#define DOWNLOAD ((uint8_t *)(uintptr_t)LK_FB_DOWNLOAD_BASE)
#endif
static unsigned length(const char *a) { unsigned n=0; while(a[n]) ++n; return n; }
static int equal(const char *a, const char *b) {
    while(*a && *a==*b) { ++a; ++b; } return *a==*b;
}
static int prefix(const char *a, const char *b) {
    while(*b) { if(*a++!=*b++) return 0; }
    return 1;
}
static void copy(void *to, const void *from, unsigned count) {
    uint8_t *d=to; const uint8_t *p=from; while(count--) *d++=*p++;
}
static void hex(char *out, uint64_t value, unsigned count) {
    const char *digits="0123456789abcdef";
    for(unsigned i=0;i<count;++i) out[i]=digits[(value>>((count-i-1)*4))&15];
    out[count]=0;
}
static void reply(const char *code, const char *message, unsigned next) {
    unsigned n=length(message); if(n>60) n=60;
    copy(s->tx,code,4); copy(s->tx+4,message,n);
    s->tx_bytes=n+4; s->phase=TX_REPLY; s->next_phase=next; s->queued=0;
    s->deadline=lk_fb_native.clock_ms()+30000;
}
static void fail(const char *message) {
    s->downloaded=s->expected=s->received=s->offset=0;
    s->action=0; reply("FAIL",message,RX_COMMAND);
}
static void command(char *cmd) {
    char value[32];
    if(prefix(cmd,"getvar:")) {
        const char *key=cmd+7;
        if(equal(key,"version")) reply("OKAY","0.4",RX_COMMAND);
        else if(equal(key,"product")) reply("OKAY","w202ds",RX_COMMAND);
        else if(equal(key,"is-userspace")) reply("OKAY","no",RX_COMMAND);
        else if(equal(key,"max-download-size")) reply("OKAY","0x08000000",RX_COMMAND);
        else if(equal(key,"current-slot")) fail("specify _a or _b explicitly");
        // Prevent the SDK client silently choosing its current slot.
        else if(prefix(key,"has-slot:")) reply("OKAY","no",RX_COMMAND);
        else if(prefix(key,"is-logical:")) reply("OKAY","no",RX_COMMAND);
        else if(prefix(key,"partition-size:")) {
            uint64_t bytes=0;
            if(!lk_flash_target(key+15) || lk_fb_native.size(key+15,&bytes) || !bytes)
                fail("unknown explicit slot partition");
            else { value[0]='0';value[1]='x';hex(value+2,bytes,16);reply("OKAY",value,RX_COMMAND); }
        } else if(prefix(key,"partition-type:")) reply("OKAY","raw",RX_COMMAND);
        else fail("unknown variable");
    } else if(prefix(cmd,"download:")) {
        const char *n=cmd+9; uint64_t count=0;
        s->downloaded=s->received=0;
        if(length(n)!=8) { fail("invalid download length"); return; }
        for(unsigned i=0;i<8;++i) {
            char c=n[i]; unsigned v=c>='0'&&c<='9'?c-'0':
                c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:16;
            if(v==16) { fail("invalid download length");return; } count=(count<<4)|v;
        }
        if(!count || count>LK_FB_DOWNLOAD_MAX) { fail("download too large or empty");return; }
        s->expected=count; hex(value,count,8); reply("DATA",value,RX_DATA);
    } else if(prefix(cmd,"flash:")) {
        uint64_t bytes=0; const char *name=cmd+6;
        if(!lk_flash_target(name) || lk_fb_native.size(name,&bytes) ||
            !lk_flash_size(name,DOWNLOAD,s->downloaded,bytes)) {
            fail("use explicit _a/_b and complete raw partition image");return;
        }
        copy(s->name,name,length(name)+1);s->offset=0;s->phase=FLASH_IMAGE;
        s->deadline=lk_fb_native.clock_ms()+30000;
    } else if(equal(cmd,"continue") || equal(cmd,"reboot") || equal(cmd,"reboot-bootloader")) {
        s->action=equal(cmd,"continue")?W202_LK_FB_CONTINUE:
            equal(cmd,"reboot")?W202_LK_FB_REBOOT:W202_LK_FB_BOOTLOADER;
        reply("OKAY","",RX_COMMAND);
    } else fail("unsupported command");
}
static void reset_parser(void) {
    s->phase=RX_COMMAND;s->queued=0;s->action=0;
    s->expected=s->received=s->downloaded=s->offset=0;
    s->deadline=lk_fb_native.clock_ms()+30000;
}
static void stop(void) { lk_fb_native.stop();s->active=0;s->connected=0;s->queued=0;s->downloaded=0; }
uint64_t lk_fb_service(uint64_t operation) {
    if(operation==W202_LK_FB_START) {
        if(s->active) return 0;
        reset_parser();
        if(lk_fb_native.start()) { stop();return UINT64_MAX; }
        s->active=1;return 0;
    }
    if(operation==W202_LK_FB_STOP) { if(s->active) stop();return 0; }
    if(operation!=W202_LK_FB_POLL || !s->active) return UINT64_MAX;
    lk_fb_native.poll();
    if(!lk_fb_native.ready()) {
        /* A reset or unplug invalidates every in-flight download. */
        if(s->connected) {
            // This LK frees bulk requests on disconnect. Rebind once so a
            // later SET_CONFIGURATION receives freshly allocated requests.
            stop();
            if(lk_fb_native.start()) return UINT64_MAX;
            s->active=1;
        }
        reset_parser();return 0;
    }
    s->connected=1;
    if(s->phase==FLASH_IMAGE) {
        unsigned n=s->downloaded-s->offset<LK_FLASH_CHUNK?s->downloaded-s->offset:LK_FLASH_CHUNK;
        if(lk_fb_native.write(s->name,n,0,s->offset,DOWNLOAD+s->offset) ||
           lk_fb_native.read(s->name,n,s->offset,s->verify)) fail("partition I/O failed; retry full image");
        else {
            unsigned i=0;while(i<n && s->verify[i]==DOWNLOAD[s->offset+i]) ++i;
            if(i!=n) fail("partition readback mismatch; retry full image");
            else {
                s->offset+=n;
                if(s->offset==s->downloaded) {
                    s->downloaded=0;reply("OKAY","raw image readback verified",RX_COMMAND);
                }
            }
        }
        return W202_LK_FB_BUSY;
    }
    if(s->queued) {
        unsigned actual=0;int status=lk_fb_native.complete(&actual);
        if(!status) {
            if(s->phase!=RX_COMMAND && lk_fb_native.clock_ms()>s->deadline) {
                stop();reset_parser();return UINT64_MAX;
            }
            return s->phase==RX_COMMAND?0:W202_LK_FB_BUSY;
        }
        s->queued=0;
        if(status<0) { stop();return UINT64_MAX; }
        if(s->phase==TX_REPLY) {
            if(actual!=s->tx_bytes) { stop();return UINT64_MAX; }
            s->phase=s->next_phase;
            if(s->action) {
                unsigned action=s->action;s->action=0;
                if(action==W202_LK_FB_BOOTLOADER || action==W202_LK_FB_REBOOT)
                    lk_fb_native.reset_reason(action==W202_LK_FB_BOOTLOADER);
                return action;
            }
        } else if(s->phase==RX_COMMAND) {
            if(!actual || actual>64) fail("invalid command packet");
            else {
                s->io[actual]=0;
                /* Embedded NULs must not turn a malformed packet into flash. */
                if(length((char *)s->io)!=actual) fail("invalid command packet");
                else command((char *)s->io);
            }
        } else if(s->phase==RX_DATA) {
            // Fastboot data phases allow short packets and ignore USB ZLPs.
            if(!actual) return W202_LK_FB_BUSY;
            if(actual>s->queue_bytes || actual>s->expected-s->received)
                fail("invalid download transfer");
            else {
                copy(DOWNLOAD+s->received,s->io,actual);s->received+=actual;
                if(s->received==s->expected) {
                    s->downloaded=s->received;reply("OKAY","",RX_COMMAND);
                } else s->deadline=lk_fb_native.clock_ms()+30000;
            }
        }
    } else {
        unsigned bytes=s->phase==TX_REPLY?s->tx_bytes:
            s->phase==RX_COMMAND?64:
            s->expected-s->received<LK_FLASH_CHUNK?s->expected-s->received:LK_FLASH_CHUNK;
        s->queue_bytes=bytes;
        if(lk_fb_native.queue(s->phase==TX_REPLY,s->phase==TX_REPLY?s->tx:s->io,bytes)) {
            stop();return UINT64_MAX;
        }
        s->queued=1;
    }
    return s->phase==RX_COMMAND?0:W202_LK_FB_BUSY;
}
void lk_fb_prepare(void) {
    volatile uint8_t *p=(void *)s;
    for(unsigned i=0;i<sizeof(*s);++i) p[i]=0;
}
