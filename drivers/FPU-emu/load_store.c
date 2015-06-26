/*---------------------------------------------------------------------------+
 |  load_store.c                                                             |
 |                                                                           |
 | This file contains most of the code to interpret the FPU instructions     |
 | which load and store from user memory.                                    |
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

#include <asm/segment.h>

#include "fpu_system.h"
#include "exception.h"
#include "fpu_emu.h"
#include "status_w.h"
#include "control_w.h"


#define _NONE_ 0   /* FPU_st0_ptr etc not needed */
#define _REG0_ 1   /* Will be storing st(0) */
#define _PUSH_ 3   /* Need to check for space to push onto stack */
#define _null_ 4   /* Function illegal or not implemented */

#define pop_0()	{ pop_ptr->tag = TW_Empty; top++; }


static unsigned char const type_table[32] = {
  _PUSH_, _PUSH_, _PUSH_, _PUSH_,
  _null_, _null_, _null_, _null_,
  _REG0_, _REG0_, _REG0_, _REG0_,
  _REG0_, _REG0_, _REG0_, _REG0_,
  _NONE_, _null_, _NONE_, _PUSH_,
  _NONE_, _PUSH_, _null_, _PUSH_,
  _NONE_, _null_, _NONE_, _REG0_,
  _NONE_, _REG0_, _NONE_, _REG0_
  };

void load_store_instr(char type, fpu_addr_modes addr_modes)
{
  FPU_REG *pop_ptr;  /* We need a version of FPU_st0_ptr which won't
			change if the emulator is re-entered. */

  pop_ptr = NULL;    /* Initialized just to stop compiler warnings. */
  switch ( type_table[(int) (unsigned) type] )
    {
    case _NONE_:
      break;
    case _REG0_:
      pop_ptr = &st(0);       /* Some of these instructions pop after
				 storing */

      FPU_st0_ptr = pop_ptr;      /* Set the global variables. */
      FPU_st0_tag = FPU_st0_ptr->tag;
      break;
    case _PUSH_:
      {
	pop_ptr = &st(-1);
	if ( pop_ptr->tag != TW_Empty )
	  { stack_overflow(); return; }
	top--;
      }
      break;
    case _null_:
      FPU_illegal();
      return;
#ifdef PARANOID
    default:
      EXCEPTION(EX_INTERNAL);
      return;
#endif PARANOID
    }

switch ( type )
  {
  case 000:       /* fld m32real */
    clear_C1();
    reg_load_single();
    if ( (FPU_loaded_data.tag == TW_NaN) &&
	real_2op_NaN(&FPU_loaded_data, &FPU_loaded_data, &FPU_loaded_data) )
      {
	top++;
	break;
      }
    reg_move(&FPU_loaded_data, pop_ptr);
    break;
  case 001:      /* fild m32int */
    clear_C1();
    reg_load_int32();
    reg_move(&FPU_loaded_data, pop_ptr);
    break;
  case 002:      /* fld m64real */
    clear_C1();
    reg_load_double();
    if ( (FPU_loaded_data.tag == TW_NaN) &&
	real_2op_NaN(&FPU_loaded_data, &FPU_loaded_data, &FPU_loaded_data) )
      {
	top++;
	break;
      }
    reg_move(&FPU_loaded_data, pop_ptr);
    break;
  case 003:      /* fild m16int */
    clear_C1();
    reg_load_int16();
    reg_move(&FPU_loaded_data, pop_ptr);
    break;
  case 010:      /* fst m32real */
    clear_C1();
    reg_store_single();
    break;
  case 011:      /* fist m32int */
    clear_C1();
    reg_store_int32();
    break;
  case 012:     /* fst m64real */
    clear_C1();
    reg_store_double();
    break;
  case 013:     /* fist m16int */
    clear_C1();
    reg_store_int16();
    break;
  case 014:     /* fstp m32real */
    clear_C1();
    if ( reg_store_single() )
      pop_0();  /* pop only if the number was actually stored
		 (see the 80486 manual p16-28) */
    break;
  case 015:     /* fistp m32int */
    clear_C1();
    if ( reg_store_int32() )
      pop_0();  /* pop only if the number was actually stored
		 (see the 80486 manual p16-28) */
    break;
  case 016:     /* fstp m64real */
    clear_C1();
    if ( reg_store_double() )
      pop_0();  /* pop only if the number was actually stored
		 (see the 80486 manual p16-28) */
    break;
  case 017:     /* fistp m16int */
    clear_C1();
    if ( reg_store_int16() )
      pop_0();  /* pop only if the number was actually stored
		 (see the 80486 manual p16-28) */
    break;
  case 020:     /* fldenv  m14/28byte */
    fldenv(addr_modes);
    break;
  case 022:     /* frstor m94/108byte */
    frstor(addr_modes);
    break;
  case 023:     /* fbld m80dec */
    clear_C1();
    reg_load_bcd();
    reg_move(&FPU_loaded_data, pop_ptr);
    break;
  case 024:     /* fldcw */
    RE_ENTRANT_CHECK_OFF;
    FPU_verify_area(VERIFY_READ, FPU_data_address, 2);
    control_word = get_fs_word((unsigned short *) FPU_data_address);
    RE_ENTRANT_CHECK_ON;
    if ( partial_status & ~control_word & CW_Exceptions )
      partial_status |= (SW_Summary | SW_Backward);
    else
      partial_status &= ~(SW_Summary | SW_Backward);
#ifdef PECULIAR_486
    control_word |= 0x40;  /* An 80486 appears to always set this bit */
#endif PECULIAR_486
    NO_NET_DATA_EFFECT;
    NO_NET_INSTR_EFFECT;
    break;
  case 025:      /* fld m80real */
    clear_C1();
    reg_load_extended();
    reg_move(&FPU_loaded_data, pop_ptr);
    break;
  case 027:      /* fild m64int */
    clear_C1();
    reg_load_int64();
    reg_move(&FPU_loaded_data, pop_ptr);
    break;
  case 030:     /* fstenv  m14/28byte */
    fstenv(addr_modes);
    NO_NET_DATA_EFFECT;
    break;
  case 032:      /* fsave */
    fsave(addr_modes);
    NO_NET_DATA_EFFECT;
    break;
  case 033:      /* fbstp m80dec */
    clear_C1();
    if ( reg_store_bcd() )
      pop_0();  /* pop only if the number was actually stored
		 (see the 80486 manual p16-28) */
    break;
  case 034:      /* fstcw m16int */
    RE_ENTRANT_CHECK_OFF;
    FPU_verify_area(VERIFY_WRITE,FPU_data_address,2);
    put_fs_word(control_word, (short *) FPU_data_address);
    RE_ENTRANT_CHECK_ON;
    NO_NET_DATA_EFFECT;
    NO_NET_INSTR_EFFECT;
    break;
  case 035:      /* fstp m80real */
    clear_C1();
    if ( reg_store_extended() )
      pop_0();  /* pop only if the number was actually stored
		 (see the 80486 manual p16-28) */
    break;
  case 036:      /* fstsw m2byte */
    RE_ENTRANT_CHECK_OFF;
    FPU_verify_area(VERIFY_WRITE,FPU_data_address,2);
    put_fs_word(status_word(),(short *) FPU_data_address);
    RE_ENTRANT_CHECK_ON;
    NO_NET_DATA_EFFECT;
    NO_NET_INSTR_EFFECT;
    break;
  case 037:      /* fistp m64int */
    clear_C1();
    if ( reg_store_int64() )
      pop_0();  /* pop only if the number was actually stored
		 (see the 80486 manual p16-28) */
    break;
  }
}
