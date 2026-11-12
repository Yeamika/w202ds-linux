ENTRY(probe_start)
SECTIONS {
  . = 0x86000000;
  .header : { KEEP(*(.header)) }
  .text : { *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  __image_end = .;
  /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
  ASSERT(__image_end <= 0x86200000, "probe exceeds the executable LK download block")
}
