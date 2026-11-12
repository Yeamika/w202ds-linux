/* SPDX-License-Identifier: MIT */
#include <stdint.h>

/* Addresses belong only to the pinned W202DS factory LK image. */
static int (*const lk_printf)(const char *, ...) = (void *)0xb5035e94;
static const volatile uint64_t *const lk_l2 = (void *)0xb5f01000;
static const volatile uint32_t *const lk_cpu_count = (void *)0xb50eed8c;

int ramboot_probe(const char *operation)
{
    uint64_t el, mpidr, sctlr, tcr, ttbr, daif, sp;
    const char expected[] = ":probe";

    if (!operation)
        return -1;
    for (unsigned i = 0; i < sizeof(expected); ++i)
        if (operation[i] != expected[i])
            return -1;

    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    lk_printf("RAMBOOT-PROBE: EL=%lu MPIDR=%lx DAIF=%lx SP=%lx\n",
              (unsigned long)(el >> 2), (unsigned long)mpidr,
              (unsigned long)daif, (unsigned long)sp);
    if (el != 8)
        return -1;

    __asm__ volatile("mrs %0, sctlr_el2" : "=r"(sctlr));
    __asm__ volatile("mrs %0, tcr_el2" : "=r"(tcr));
    __asm__ volatile("mrs %0, ttbr0_el2" : "=r"(ttbr));
    lk_printf("RAMBOOT-PROBE: SCTLR_EL2=%lx TCR_EL2=%lx TTBR0_EL2=%lx\n",
              (unsigned long)sctlr, (unsigned long)tcr, (unsigned long)ttbr);
    lk_printf("RAMBOOT-PROBE: LK active CPU count=%u\n", *lk_cpu_count);
    /* The installed jump-rmk hook has already validated the 0x86000000 L2
     * entry before reaching this code. Read adjacent entries from that table. */
    lk_printf("RAMBOOT-PROBE: L2[820]=%lx L2[828]=%lx L2[85e]=%lx L2[860]=%lx\n",
              (unsigned long)lk_l2[0x10], (unsigned long)lk_l2[0x14],
              (unsigned long)lk_l2[0x2f], (unsigned long)lk_l2[0x30]);
    return 0;
}
