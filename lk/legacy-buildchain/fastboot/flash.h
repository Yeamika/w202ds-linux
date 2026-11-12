/* SPDX-License-Identifier: MIT */
#ifndef LK_VERIFIED_FLASH_H
#define LK_VERIFIED_FLASH_H
#include <stdint.h>
#define LK_FLASH_CHUNK 16384u
/* Raw GPT slot names only. In particular, never pass LK's "uboot" alias,
 * boot-area aliases, mounted ESP, or backup/special-partition handlers. */
static inline int lk_flash_target(const char *name)
{
    unsigned n = 0;
    if (!name) return 0;
    while (name[n]) {
        char c = name[n++];
        if (n > 35 || !((c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_')) return 0;
    }
    return n > 2 && name[n-2] == '_' && (name[n-1] == 'a' || name[n-1] == 'b');
}
static inline int lk_flash_size(const char *name, const void *data,
                               uint64_t bytes, uint64_t capacity)
{
    const uint8_t *p = data;
    /* Complete partition images, no partial tail or Android sparse stream. */
    return lk_flash_target(name) && data && bytes && !(bytes & 511) &&
        bytes == capacity && !(p[0] == 0x3a && p[1] == 0xff &&
                               p[2] == 0x26 && p[3] == 0xed);
}
#endif
