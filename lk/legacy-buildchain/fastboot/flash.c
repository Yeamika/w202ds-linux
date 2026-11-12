/* SPDX-License-Identifier: MIT */
#include "flash.h"
#include "native.h"

/* Replace the native flash: handler; download is patched separately to keep
 * data in RAM. RAMBoot and the USB transport remain native. A successful reply
 * means the entire named image was read back.
 * The caller's native Fastboot buffer remains read-only throughout. */
void lk_debug_flash(const char *name, const uint8_t *data, uint64_t bytes)
{
    void (*const reply)(const char *, const char *) = (void *)0xb500e11c;
    uint64_t capacity = 0;
    /* Native Fastboot's stack size is inherited, not chosen by this patch. */
    uint8_t verify[512] __attribute__((aligned(64)));
    if (!lk_flash_target(name) || lk_partition_size(name, &capacity) ||
        !lk_flash_size(name, data, bytes, capacity)) {
        reply("FAIL", "use explicit _a/_b and complete raw partition image");
        return;
    }
    for (uint64_t offset = 0; offset < bytes; offset += sizeof(verify)) {
        unsigned count = bytes-offset < sizeof(verify) ? bytes-offset : sizeof(verify);
        if (lk_raw_write(name, count, 0, offset, data+offset) ||
            lk_raw_read(name, count, offset, verify)) {
            reply("FAIL", "partition I/O failed; image may be incomplete");
            return;
        }
        for (unsigned i = 0; i < count; ++i) if (verify[i] != data[offset+i]) {
            reply("FAIL", "partition readback mismatch; retry flash");
            return;
        }
    }
    reply("OKAY", "raw image written and readback verified");
}
