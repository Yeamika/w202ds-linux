// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <AsmMacroIoLibV8.h>
#include <W202dsMemory.h>
.text
.align 4
ASM_FUNC(W202ReturnLk)
  mov x19, x0
  mov x20, x1
  msr daifset, #0xf
  // Stackless cleanup of all cacheable mappings of the normal UEFI platform.
  // LK code/context are outside these mappings and have not been touched.
  mrs x4, ctr_el0
  ubfx x4, x4, #16, #4
  mov x5, #4
  lsl x5, x5, x4
  ldr x6, =W202DS_UEFI_BOOT_MEMORY_BASE
  ldr x7, =W202DS_UEFI_BOOT_MEMORY_END
1:
  dc civac, x6
  add x6, x6, x5
  cmp x6, x7
  b.lo 1b
  // The EFI image and its stack may be allocated in the additional bank.
  ldr x6, =W202DS_UEFI_EXTRA_MEMORY_BASE
  ldr x7, =W202DS_UEFI_EXTRA_MEMORY_END
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
  ldr x6, =0x82fff000
  mov x7, #0x60
  str x7, [x6, #8]
  dsb sy
  mov x0, x20
  br x19
