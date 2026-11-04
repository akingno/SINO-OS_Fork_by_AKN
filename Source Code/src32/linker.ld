OUTPUT_FORMAT(pei-i386)
ENTRY(_start)
SECTIONS
{
  . = 0x11000;
  .text : { *(.start) *(.text*) }
  .rdata : { *(.rdata*) *(.rodata*) }
  .data : { *(.data*) }
  .bss 0x100000 : {
    ___bss_start = .;
    *(.bss*) *(COMMON)
    ___bss_end = .;
  }
  /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.drectve*) *(.idata*) *(.reloc*) }
}
