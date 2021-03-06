SECTIONS
{

  dummy :
  {
    . = 0x100000;
  }
  
  .text :
  {
    *(.text)
    etext = .;
    *(.rodata)
    *(.data)
    _edata = .;
    *(.bss)
    _end = .;
    end = .;
  }
}

