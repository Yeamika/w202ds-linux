/* SPDX-License-Identifier: MIT */
#ifndef LK_FB_SERVICE_H
#define LK_FB_SERVICE_H
#include <stdint.h>
#define LK_FB_DOWNLOAD_BASE 0x88000000u
#define LK_FB_DOWNLOAD_MAX 0x08000000u
struct lk_fb_ops {
    int (*start)(void);
    void (*stop)(void);
    void (*poll)(void);
    int (*ready)(void);
    int (*queue)(int tx, void *buffer, unsigned bytes);
    int (*complete)(unsigned *actual);
    uint64_t (*clock_ms)(void);
    int (*size)(const char *, uint64_t *);
    int (*read)(const char *, uint64_t, uint64_t, void *);
    int (*write)(const char *, uint64_t, uint64_t, uint64_t, const void *);
    void (*reset_reason)(unsigned bootloader);
};
extern const struct lk_fb_ops lk_fb_native;
uint64_t lk_fb_service(uint64_t operation);
void lk_fb_prepare(void);
#endif
