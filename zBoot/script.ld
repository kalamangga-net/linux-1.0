SECTIONS
{

  dummy :
  {
    . = 0x1000;
  }

  .text :
  {
    *(.te*)
    etext = .;
    *(.rodata*)
    *(.data)
    _edata = .;
    *(.bss)
    _end = .;
    end = .;
    *(piggy)
  }
}

