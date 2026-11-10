// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <AsmMacroIoLibV8.h>
.text
.align 4
ASM_FUNC(W202EnterKernel)
  mov x19, x0
  mov x20, x1
  msr daifset, #0xf

  // This port and the native RAMBoot entry run at EL2 with identity mappings.
  // Stay stackless: cache maintenance must not create fresh dirty stack data.
  // ArmCleanInvalidateDataCache asserts with the MMU enabled; use VA-based
  // maintenance over all mapped cacheable DRAM before disabling translation.
  // Ranges match LkInheritedPlatformLib/ArmPlatformLibNullMem.c. MMIO and the
  // uncached framebuffer are excluded. No firmware or DMA writes follow EBS.
  mrs x4, ctr_el0
  ubfx x4, x4, #16, #4
  mov x5, #4
  lsl x5, x5, x4
  ldr x6, =0x82800000
  ldr x7, =0x86000000
1:
  dc civac, x6
  add x6, x6, x5
  cmp x6, x7
  b.lo 1b
  ldr x6, =0xd0000000
  ldr x7, =0xd8200000
2:
  dc civac, x6
  add x6, x6, x5
  cmp x6, x7
  b.lo 2b
  dsb sy
  ic iallu
  dsb sy
  isb

  mrs x4, sctlr_el2
  bic x4, x4, #1
  bic x4, x4, #(1 << 2)
  bic x4, x4, #(1 << 12)
  msr sctlr_el2, x4
  isb
  tlbi alle2
  dsb sy
  isb

  // Last retained marker: translation and caches are off, next is the kernel.
  ldr x6, =0x82fff000
  mov x7, #0x54
  str x7, [x6, #8]
  str x4, [x6, #24]
  dsb sy
  mov x0, x20
  mov x1, xzr
  mov x2, xzr
  mov x3, xzr
  br x19
