/* SPDX-License-Identifier: MIT
 * Read DDR through native Fastboot INFO packets. No storage or reset calls.
 */
#include <stdint.h>

static void (*const status)(const char *, const char *) = (void *)0xb500e11c;
static const char digits[] = "0123456789abcdef";

static char *append(char *out, const char *text)
{
    while (*text)
        *out++ = *text++;
    return out;
}

static char *hex32(char *out, uint32_t value)
{
    for (unsigned i = 0; i < 8; ++i)
        *out++ = digits[(value >> (28 - i * 4)) & 15];
    return out;
}

int memory_read(const char *operation)
{
    extern const volatile uint64_t memory_parameters[];
    const volatile uint64_t *args = memory_parameters;
    uint64_t address = args[0], size = args[1];
    uint32_t crc = UINT32_MAX;
    char line[60], *out;
    const char *expected = ":probe";
    uint64_t end = address < UINT64_C(0xc0000000) ? UINT64_C(0xc0000000) :
        (address >= UINT64_C(0xc8000000) && address < UINT64_C(0xcc800000) ?
         UINT64_C(0xcc800000) : 0);
    if (!operation)
        return -1;
    while (*expected && *operation == *expected) {
        ++operation;
        ++expected;
    }
    if (*operation || *expected || address < UINT64_C(0x80000000) ||
        !end || !size || size > 0x10000 ||
        (address & 15) || (size & 15) || size > end - address)
        return -1;
    out = append(line, "PJ4EDK2 BEGIN ");
    out = hex32(out, (uint32_t)address);
    *out++ = ' ';
    out = hex32(out, (uint32_t)size);
    *out = 0;
    status("INFO", line);
    for (uint64_t offset = 0; offset < size; offset += 16) {
        const volatile uint8_t *memory = (const void *)(uintptr_t)(address + offset);
        out = append(line, "PJ4EDK2 MEM ");
        out = hex32(out, (uint32_t)(address + offset));
        *out++ = ' ';
        for (unsigned i = 0; i < 16; ++i) {
            uint8_t value = memory[i];
            *out++ = digits[value >> 4];
            *out++ = digits[value & 15];
            crc ^= value;
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0 - (crc & 1)));
        }
        *out = 0;
        status("INFO", line);
    }
    out = hex32(append(line, "PJ4EDK2 CRC "), ~crc);
    *out = 0;
    status("INFO", line);
    return 0;
}
