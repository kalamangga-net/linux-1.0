/*---------------------------------------------------------------------------+
 |  get_address.c                                                            |
 |                                                                           |
 | Get the effective address from an FPU instruction.                        |
 |                                                                           |
 | Copyright (C) 1992,1993,1994                                              |
 |                       W. Metzenthen, 22 Parker St, Ormond, Vic 3163,      |
 |                       Australia.  E-mail   billm@vaxc.cc.monash.edu.au    |
 |                                                                           |
 |                                                                           |
 +---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------+
 | Note:                                                                     |
 |    The file contains code which accesses user memory.                     |
 |    Emulator static data may change when user memory is accessed, due to   |
 |    other processes using the emulator while swapping is in progress.      |
 +---------------------------------------------------------------------------*/


#include <linux/stddef.h>

#include <asm/segment.h>

#include "fpu_system.h"
#include "exception.h"
#include "fpu_emu.h"

static int reg_offset[] = {
	offsetof(struct info,___eax),
	offsetof(struct info,___ecx),
	offsetof(struct info,___edx),
	offsetof(struct info,___ebx),
	offsetof(struct info,___esp),
	offsetof(struct info,___ebp),
	offsetof(struct info,___esi),
	offsetof(struct info,___edi)
};

#define REG_(x) (*(long *)(reg_offset[(x)]+(char *) FPU_info))

static int reg_offset_vm86[] = {
	offsetof(struct info,___cs),
	offsetof(struct info,___vm86_ds),
	offsetof(struct info,___vm86_es),
	offsetof(struct info,___vm86_fs),
	offsetof(struct info,___vm86_gs),
	offsetof(struct info,___ss)
      };

#define VM86_REG_(x) (*(unsigned short *) \
		      (reg_offset_vm86[((unsigned)x)]+(char *) FPU_info))


/* Decode the SIB byte. This function assumes mod != 0 */
static void *sib(int mod, unsigned long *fpu_eip)
{
  unsigned char ss,index,base;
  long offset;

  RE_ENTRANT_CHECK_OFF;
  FPU_code_verify_area(1);
  base = get_fs_byte((char *) (*fpu_eip));   /* The SIB byte */
  RE_ENTRANT_CHECK_ON;
  (*fpu_eip)++;
  ss = base >> 6;
  index = (base >> 3) & 7;
  base &= 7;

  if ((mod == 0) && (base == 5))
    offset = 0;              /* No base register */
  else
    offset = REG_(base);

  if (index == 4)
    {
      /* No index register */
      /* A non-zero ss is illegal */
      if ( ss )
	EXCEPTION(EX_Invalid);
    }
  else
    {
      offset += (REG_(index)) << ss;
    }

  if (mod == 1)
    {
      /* 8 bit signed displacement */
      RE_ENTRANT_CHECK_OFF;
      FPU_code_verify_area(1);
      offset += (signed char) get_fs_byte((char *) (*fpu_eip));
      RE_ENTRANT_CHECK_ON;
      (*fpu_eip)++;
    }
  else if (mod == 2 || base == 5) /* The second condition also has mod==0 */
    {
      /* 32 bit displacment */
      RE_ENTRANT_CHECK_OFF;
      FPU_code_verify_area(4);
      offset += (signed) get_fs_long((unsigned long *) (*fpu_eip));
      RE_ENTRANT_CHECK_ON;
      (*fpu_eip) += 4;
    }

  return (void *) offset;
}


static unsigned long vm86_segment(unsigned char segment)
{ 
  segment--;
#ifdef PARANOID
  if ( segment > PREFIX_SS_ )
    {
      EXCEPTION(EX_INTERNAL|0x130);
      math_abort(FPU_info,SIGSEGV);
    }
#endif PARANOID
  return (unsigned long)VM86_REG_(segment) << 4;
}


/*
       MOD R/M byte:  MOD == 3 has a special use for the FPU
                      SIB byte used iff R/M = 100b

       7   6   5   4   3   2   1   0
       .....   .........   .........
        MOD    OPCODE(2)     R/M


       SIB byte

       7   6   5   4   3   2   1   0
       .....   .........   .........
        SS      INDEX        BASE

*/

void get_address(unsigned char FPU_modrm, unsigned long *fpu_eip,
		 fpu_addr_modes addr_modes)
{
  unsigned char mod;
  long *cpu_reg_ptr;
  int offset = 0;     /* Initialized just to stop compiler warnings. */

#ifndef PECULIAR_486
  /* This is a reasonable place to do this */
  FPU_data_selector = FPU_DS;
#endif PECULIAR_486

  /* Memory accessed via the cs selector is write protected
     in 32 bit protected mode. */
#define FPU_WRITE_BIT 0x10
  if ( !addr_modes.vm86 && (FPU_modrm & FPU_WRITE_BIT)
      && (addr_modes.override.segment == PREFIX_CS_) )
    {
      math_abort(FPU_info,SIGSEGV);
    }

  mod = (FPU_modrm >> 6) & 3;

  if (FPU_rm == 4 && mod != 3)
    {
      FPU_data_address = sib(mod, fpu_eip);
      return;
    }

  cpu_reg_ptr = & REG_(FPU_rm);
  switch (mod)
    {
    case 0:
      if (FPU_rm == 5)
	{
	  /* Special case: disp32 */
	  RE_ENTRANT_CHECK_OFF;
	  FPU_code_verify_area(4);
	  offset = get_fs_long((unsigned long *) (*fpu_eip));
	  (*fpu_eip) += 4;
	  RE_ENTRANT_CHECK_ON;
	  FPU_data_address = (void *) offset;
	  return;
	}
      else
	{
	  FPU_data_address = (void *)*cpu_reg_ptr;  /* Just return the contents
						   of the cpu register */
	  return;
	}
    case 1:
      /* 8 bit signed displacement */
      RE_ENTRANT_CHECK_OFF;
      FPU_code_verify_area(1);
      offset = (signed char) get_fs_byte((char *) (*fpu_eip));
      RE_ENTRANT_CHECK_ON;
      (*fpu_eip)++;
      break;
    case 2:
      /* 32 bit displacement */
      RE_ENTRANT_CHECK_OFF;
      FPU_code_verify_area(4);
      offset = (signed) get_fs_long((unsigned long *) (*fpu_eip));
      (*fpu_eip) += 4;
      RE_ENTRANT_CHECK_ON;
      break;
    case 3:
      /* Not legal for the FPU */
      EXCEPTION(EX_Invalid);
    }

  if ( addr_modes.vm86 )
    {
      offset += vm86_segment(addr_modes.override.segment);
    }

  FPU_data_address = offset + (char *)*cpu_reg_ptr;
}


void get_address_16(unsigned char FPU_modrm, unsigned long *fpu_eip,
		      fpu_addr_modes addr_modes)
{
  unsigned char mod;
  int offset = 0;     /* Default used for mod == 0 */

#ifndef PECULIAR_486
  /* This is a reasonable place to do this */
  FPU_data_selector = FPU_DS;
#endif PECULIAR_486

  /* Memory accessed via the cs selector is write protected
     in 32 bit protected mode. */
#define FPU_WRITE_BIT 0x10
  if ( !addr_modes.vm86 && (FPU_modrm & FPU_WRITE_BIT)
      && (addr_modes.override.segment == PREFIX_CS_) )
    {
      math_abort(FPU_info,SIGSEGV);
    }

  mod = (FPU_modrm >> 6) & 3;

  switch (mod)
    {
    case 0:
      if (FPU_rm == 6)
	{
	  /* Special case: disp16 */
	  RE_ENTRANT_CHECK_OFF;
	  FPU_code_verify_area(2);
	  offset = (unsigned short)get_fs_word((unsigned short *) (*fpu_eip));
	  (*fpu_eip) += 2;
	  RE_ENTRANT_CHECK_ON;
	  goto add_segment;
	}
      break;
    case 1:
      /* 8 bit signed displacement */
      RE_ENTRANT_CHECK_OFF;
      FPU_code_verify_area(1);
      offset = (signed char) get_fs_byte((signed char *) (*fpu_eip));
      RE_ENTRANT_CHECK_ON;
      (*fpu_eip)++;
      break;
    case 2:
      /* 16 bit displacement */
      RE_ENTRANT_CHECK_OFF;
      FPU_code_verify_area(2);
      offset = (unsigned) get_fs_word((unsigned short *) (*fpu_eip));
      (*fpu_eip) += 2;
      RE_ENTRANT_CHECK_ON;
      break;
    case 3:
      /* Not legal for the FPU */
      EXCEPTION(EX_Invalid);
      break;
    }
  switch ( FPU_rm )
    {
    case 0:
      offset += FPU_info->___ebx + FPU_info->___esi;
      break;
    case 1:
      offset += FPU_info->___ebx + FPU_info->___edi;
      break;
    case 2:
      offset += FPU_info->___ebp + FPU_info->___esi;
      break;
    case 3:
      offset += FPU_info->___ebp + FPU_info->___edi;
      break;
    case 4:
      offset += FPU_info->___esi;
      break;
    case 5:
      offset += FPU_info->___edi;
      break;
    case 6:
      offset += FPU_info->___ebp;
      break;
    case 7:
      offset += FPU_info->___ebx;
      break;
    }

 add_segment:
  offset &= 0xffff;

  if ( addr_modes.vm86 )
    {
      offset += vm86_segment(addr_modes.override.segment);
    }

  FPU_data_address = (void *)offset ;
}

