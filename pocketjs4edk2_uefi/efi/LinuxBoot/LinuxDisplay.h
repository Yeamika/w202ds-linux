// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef W202_LINUX_DISPLAY_H
#define W202_LINUX_DISPLAY_H
#include <Uefi.h>
#define PJB_EDK2
#include "pocketjs_boot_context.h"
#include "common/include/pj4edk2_linux_display.h"
/* Validate only geometry here; GOP/MMIO/reserved-memory checks are separate. */
STATIC BOOLEAN LinuxDisplayCells(CONST PJB_BOOT_DISPLAY *D, UINT32 *V) {
  CONST struct pj4edk2_surface Surface={0xb0000000,0x9e4000,4608000,
    1200,1920,2400,1,90,180};
  struct pj4edk2_region Region;
  if (!D || D->revision!=PJB_BOOT_DISPLAY_REVISION || D->size<sizeof(*D) ||
      D->display_width!=1920 || D->display_height!=1200 || D->rotation!=90 ||
      !pj4edk2_display_rectangle_valid(D->display_width,D->display_height,
        D->x,D->y,D->width,D->height) || D->width<128 || D->height<80) return FALSE;
  Region=(struct pj4edk2_region){D->display_width,D->display_height,
    D->x,D->y,D->width,D->height};
  return pj4edk2_linux_display_cells(&Surface,&Region,V)?TRUE:FALSE;
}
#endif
