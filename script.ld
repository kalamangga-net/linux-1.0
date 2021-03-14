SECTIONS
{
  . = 0x100000;
  .text :
  {
    *(.text)
    _etext = .;
    etext = .;
    *(.rodata*)
    *(.data)
    _edata = .;
    edata = .;
    *(.bss COMMON)
    _end = .;
    end = .;
  }

}

