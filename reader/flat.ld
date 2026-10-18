SECTIONS {
  . = 0;
  .text : { *(.text.entry) *(.text .text.*) *(.rodata .rodata.*) }
  /DISCARD/ : { *(.eh_frame .eh_frame.*) *(.note.*) *(.comment) *(.gnu.property) }
}
