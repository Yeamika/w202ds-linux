/* SPDX-License-Identifier: MIT
 * Suspend the pinned single-CPU LK while EFI runs, then restore its GIC.
 * No partition writes, security calls, or persistent slot changes.
 */
#include <stdint.h>
#include <stddef.h>
#include "LkReturn.h"
#ifdef W202_LK_FASTBOOT_ENABLED
#include "LkFastboot.h"
#include "../../fastboot/service.h"
extern void lk_fb_gateway(void);
#endif
#define GICD 0x12000000u
#define GICR 0x12050000u
#define MAX_IRQS 1024
#define REG(base, off) (*(volatile uint32_t *)(uintptr_t)((base)+(off)))
#define REG64(base, off) (*(volatile uint64_t *)(uintptr_t)((base)+(off)))
struct context {
    uint64_t cpu[64];
    uint32_t count, control, local_group, local_enable, local_pending;
    uint32_t local_config[2], local_priority[8];
    uint32_t group[32], enable[32], pending[32], config[64], priority[256];
    uint64_t route[MAX_IRQS];
};
_Static_assert(sizeof(struct context)<=W202_LK_CONTEXT_LIMIT,"context reservation");
#ifdef W202_LK_FASTBOOT_ENABLED
_Static_assert(sizeof(struct context)<=W202_LK_FB_CONTEXT-W202_LK_CONTEXT_ADDRESS,
               "saved LK context must not overlap Fastboot gateway context");
#endif
static struct context *const saved=(void *)(uintptr_t)W202_LK_CONTEXT_ADDRESS;
extern void lk_return_resume(void);
static int wait_rwp(uintptr_t base,uint32_t bit) {
    uint64_t start,now,freq;
    __asm__ volatile("mrs %0, cntpct_el0":"=r"(start));
    __asm__ volatile("mrs %0, cntfrq_el0":"=r"(freq));
    do {
        if (!(REG(base,0)&bit)) return 0;
        __asm__ volatile("mrs %0, cntpct_el0":"=r"(now));
    } while(now-start<freq/10);
    return -1;
}
int lk_return_save(void) {
    W202_LK_RETURN *desc=(void *)(uintptr_t)W202_LK_RETURN_ADDRESS;
    desc->magic=0;
    uint64_t mpidr,spsel;
    __asm__ volatile("mrs %0, mpidr_el1":"=r"(mpidr));
    __asm__ volatile("mrs %0, spsel":"=r"(spsel));
    /* This port only starts the boot CPU. Other redistributors stay untouched. */
    if (spsel!=1 || (mpidr&0xff00ffffffULL)!=0 || REG(GICR,0x300)!=0) return -1;
    static const uint32_t helper[]={0xf90002a7,0xd50b7a35,0xd5033a9f,
        0xd50c831f,0xd5033b9f,0xd5033fdf,0xd65f03c0};
    for(unsigned i=0;i<sizeof(helper)/sizeof(helper[0]);i++)
        if(REG(0xb500e2a0,i*4)!=helper[i])return -1;
    if (REG(0xb5000000,0xe2bc)!=0x900008a0 ||
        REG(0xb5000000,0xe2c0)!=0x91000000 ||
        REG(0xb5000000,0xe2c4)!=0xa9bf7bfd ||
        REG(0xb5000000,0xe2c8)!=0x910003fd ||
        REG(0xb5000000,0xe2e0)!=0x17fffdea ||
        REG(0xb5000000,0xda88)!=0xa9bf7bfd ||
        REG(0xb5000000,0xda8c)!=0x528001a0 ||
        REG(0xb5000000,0xdaa8)!=0xa8c17bfd ||
        REG64(0xb5f01000,0x180)!=0x0000000086000791ULL) return -1;
    saved->count=((REG(GICD,4)&31)+1)*32;
    if(saved->count<32 || saved->count>MAX_IRQS) return -1;
    saved->control=REG(GICD,0);
    /* Native affinity routing is required; do not change GIC security modes. */
    if (!(saved->control&0x10)) return -1;
    saved->local_group=REG(GICR,0x80);
    saved->local_enable=REG(GICR,0x100);
    saved->local_pending=REG(GICR,0x200);
    for(unsigned i=0;i<2;i++)saved->local_config[i]=REG(GICR,0xc00+i*4);
    for(unsigned i=0;i<8;i++)saved->local_priority[i]=REG(GICR,0x400+i*4);
    for(unsigned i=1;i<saved->count/32;i++) {
        if(REG(GICD,0x300+i*4))return -1;
        saved->group[i]=REG(GICD,0x80+i*4);
        saved->enable[i]=REG(GICD,0x100+i*4);
        saved->pending[i]=REG(GICD,0x200+i*4);
    }
    for(unsigned i=2;i<saved->count/16;i++)saved->config[i]=REG(GICD,0xc00+i*4);
    for(unsigned i=8;i<saved->count/4;i++)saved->priority[i]=REG(GICD,0x400+i*4);
    for(unsigned i=32;i<saved->count;i++)saved->route[i]=REG64(GICD,0x6000+i*8);
    *desc=(W202_LK_RETURN){W202_LK_RETURN_MAGIC,1,sizeof(*desc),0,
        (uintptr_t)lk_return_resume,W202_LK_CONTEXT_ADDRESS,sizeof(*saved),3};
    desc->checksum=0-W202LkReturnSum(desc);
#ifdef W202_LK_FASTBOOT_ENABLED
    /* Trampoline cache maintenance currently implements pre-CCIDX CCSIDR.
     * Refuse to publish the optional service on any other CPU architecture. */
    uint64_t mmfr2;
    __asm__ volatile("mrs %0, id_aa64mmfr2_el1":"=r"(mmfr2));
    if (((mmfr2>>20)&15)==0) {
        W202_LK_FASTBOOT_DESCRIPTOR *fb=(void *)(uintptr_t)W202_LK_FB_ADDRESS;
        lk_fb_prepare();
        *fb=(W202_LK_FASTBOOT_DESCRIPTOR){W202_LK_FB_MAGIC,1,sizeof(*fb),0,
                              (uintptr_t)lk_fb_gateway,3,0};
        fb->checksum=0-W202LkFastbootSum(fb);
        __asm__ volatile("dc cvac, %0; dsb sy"::"r"(fb):"memory");
    }
#endif
    return 0;
}
int lk_return_restore(void) {
    /* EFI ExitBootServices disabled its sources; keep the CPU masked until
     * its original stack/vector/timer state is restored by return.S. */
    REG(GICD,0)=saved->control&~3u;
    REG(GICR,0x180)=~0u;
    if(wait_rwp(GICD,1u<<31)||wait_rwp(0x12040000,1u<<3))return -1;
    REG(GICR,0x280)=~0u;
    REG(GICR,0x80)=saved->local_group;
    REG(GICR,0xc04)=saved->local_config[1];
    for(unsigned i=0;i<8;i++)REG(GICR,0x400+i*4)=saved->local_priority[i];
    for(unsigned i=1;i<saved->count/32;i++) {
        REG(GICD,0x180+i*4)=~0u;
        REG(GICD,0x280+i*4)=~0u;
        REG(GICD,0x80+i*4)=saved->group[i];
    }
    if(wait_rwp(GICD,1u<<31))return -1;
    for(unsigned i=2;i<saved->count/16;i++)REG(GICD,0xc00+i*4)=saved->config[i];
    for(unsigned i=8;i<saved->count/4;i++)REG(GICD,0x400+i*4)=saved->priority[i];
    for(unsigned i=32;i<saved->count;i++)REG64(GICD,0x6000+i*8)=saved->route[i];
    for(unsigned i=1;i<saved->count/32;i++) {
        REG(GICD,0x200+i*4)=saved->pending[i];
        REG(GICD,0x100+i*4)=saved->enable[i];
    }
    REG(GICR,0x200)=saved->local_pending;
    REG(GICR,0x100)=saved->local_enable;
    REG(GICD,0)=saved->control;
    __asm__ volatile("dsb sy; isb":::"memory");
    return wait_rwp(GICD,1u<<31)||wait_rwp(0x12040000,1u<<3)?-1:0;
}
