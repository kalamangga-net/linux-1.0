SECTIONS
{

  . = 0x1000;

  .text :
  {
    *(.te*)
    etext = .;
    *(.rodata*)
    *(.data)
    _edata = .;
    *(.bss COMMON)
    _end = .;
    end = .;
    *(piggy)
  }
}

