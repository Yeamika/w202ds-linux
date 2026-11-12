/* The pinned boot entry ends at +0x80; exception vectors start at +0x1000.
 * Use only the verified zero alignment padding between them.
 */
ENTRY(lk_debug_oem)
SECTIONS {
    . = 0xb5000100;
    .text : { *(.text*) *(.rodata*) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
    ASSERT(SIZEOF(.text) <= 0xf00, "LK debug extensions exceed boot padding")
}
