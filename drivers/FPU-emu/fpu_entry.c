/*---------------------------------------------------------------------------+
 |  fpu_entry.c                                                              |
 |                                                                           |
 | The entry function for wm-FPU-emu                                         |
 |                                                                           |
 | Copyright (C) 1992,1993,1994                                              |
 |                       W. Metzenthen, 22 Parker St, Ormond, Vic 3163,      |
 |                       Australia.  E-mail   billm@vaxc.cc.monash.edu.au    |
 |                                                                           |
 | See the files "README" and "COPYING" for further copyright and warranty   |
 | information.                                                              |
 |                                                                           |
 +---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------+
 | Note:                                                                     |
 |    The file contains code which accesses user memory.                     |
 |    Emulator static data may change when user memory is accessed, due to   |
 |    other processes using the emulator while swapping is in progress.      |
 +---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------+
 | math_emulate() is the sole entry point for wm-FPU-emu                     |
 +---------------------------------------------------------------------------*/

#include <linux/signal.h>
#include <linux/segment.h>

#include "fpu_system.h"
#include "fpu_emu.h"
#include "exception.h"
#include "control_w.h"
#include "status_w.h"

#include <asm/segment.h>

#define __BAD__ FPU_illegal   /* Illegal on an 80486, causes SIGILL */

#ifndef NO_UNDOC_CODE    /* Un-documented FPU op-codes supported by default. */

/* WARNING: These codes are not documented by Intel in their 80486 manual
   and may not work on FPU clones or later Intel FPUs. */

/* Changes to support the un-doc codes provided by Linus Torvalds. */

#define _d9_d8_ fstp_i    /* unofficial code (19) */
#define _dc_d0_ fcom_st   /* unofficial code (14) */
#define _dc_d8_ fcompst   /* unofficial code (1c) */
#define _dd_c8_ fxch_i    /* unofficial code (0d) */
#define _de_d0_ fcompst   /* unofficial code (16) */
#define _df_c0_ ffreep    /* unofficial code (07) ffree + pop */
#define _df_c8_ fxch_i    /* unofficial code (0f) */
#define _df_d0_ fstp_i    /* unofficial code (17) */
#define _df_d8_ fstp_i    /* unofficial code (1f) */

static FUNC const st_instr_table[64] = {
  fadd__,   fld_i_,  __BAD__, __BAD__, fadd_i,  ffree_,  faddp_,  _df_c0_,
  fmul__,   fxch_i,  __BAD__, __BAD__, fmul_i,  _dd_c8_, fmulp_,  _df_c8_,
  fcom_st,  fp_nop,  __BAD__, __BAD__, _dc_d0_, fst_i_,  _de_d0_, _df_d0_,
  fcompst,  _d9_d8_, __BAD__, __BAD__, _dc_d8_, fstp_i,  fcompp,  _df_d8_,
  fsub__,   fp_etc,  __BAD__, finit_,  fsubri,  fucom_,  fsubrp,  fstsw_,
  fsubr_,   fconst,  fucompp, __BAD__, fsub_i,  fucomp,  fsubp_,  __BAD__,
  fdiv__,   trig_a,  __BAD__, __BAD__, fdivri,  __BAD__, fdivrp,  __BAD__,
  fdivr_,   trig_b,  __BAD__, __BAD__, fdiv_i,  __BAD__, fdivp_,  __BAD__,
};

#else     /* Support only documented FPU op-codes */

static FUNC const st_instr_table[64] = {
  fadd__,   fld_i_,  __BAD__, __BAD__, fadd_i,  ffree_,  faddp_,  __BAD__,
  fmul__,   fxch_i,  __BAD__, __BAD__, fmul_i,  __BAD__, fmulp_,  __BAD__,
  fcom_st,  fp_nop,  __BAD__, __BAD__, __BAD__, fst_i_,  __BAD__, __BAD__,
  fcompst,  __BAD__, __BAD__, __BAD__, __BAD__, fstp_i,  fcompp,  __BAD__,
  fsub__,   fp_etc,  __BAD__, finit_,  fsubri,  fucom_,  fsubrp,  fstsw_,
  fsubr_,   fconst,  fucompp, __BAD__, fsub_i,  fucomp,  fsubp_,  __BAD__,
  fdiv__,   trig_a,  __BAD__, __BAD__, fdivri,  __BAD__, fdivrp,  __BAD__,
  fdivr_,   trig_b,  __BAD__, __BAD__, fdiv_i,  __BAD__, fdivp_,  __BAD__,
};

#endif NO_UNDOC_CODE


#define _NONE_ 0   /* Take no special action */
#define _REG0_ 1   /* Need to check for not empty st(0) */
#define _REGI_ 2   /* Need to check for not empty st(0) and st(rm) */
#define _REGi_ 0   /* Uses st(rm) */
#define _PUSH_ 3   /* Need to check for space to push onto stack */
#define _null_ 4   /* Function illegal or not implemented */
#define _REGIi 5   /* Uses st(0) and st(rm), result to st(rm) */
#define _REGIp 6   /* Uses st(0) and st(rm), result to st(rm) then pop */
#define _REGIc 0   /* Compare st(0) and st(rm) */
#define _REGIn 0   /* Uses st(0) and st(rm), but handle checks later */

#ifndef NO_UNDOC_CODE

/* Un-documented FPU op-codes supported by default. (see above) */

static unsigned char const type_table[64] = {
  _REGI_, _NONE_, _null_, _null_, _REGIi, _REGi_, _REGIp, _REGi_,
  _REGI_, _REGIn, _null_, _null_, _REGIi, _REGI_, _REGIp, _REGI_,
  _REGIc, _NONE_, _null_, _null_, _REGIc, _REG0_, _REGIc, _REG0_,
  _REGIc, _REG0_, _null_, _null_, _REGIc, _REG0_, _REGIc, _REG0_,
  _REGI_, _NONE_, _null_, _NONE_, _REGIi, _REGIc, _REGIp, _NONE_,
  _REGI_, _NONE_, _REGIc, _null_, _REGIi, _REGIc, _REGIp, _null_,
  _REGI_, _NONE_, _null_, _null_, _REGIi, _null_, _REGIp, _null_,
  _REGI_, _NONE_, _null_, _null_, _REGIi, _null_, _REGIp, _null_
};

#else     /* Support only documented FPU op-codes */

static unsigned char const type_table[64] = {
  _REGI_, _NONE_, _null_, _null_, _REGIi, _REGi_, _REGIp, _null_,
  _REGI_, _REGIn, _null_, _null_, _REGIi, _null_, _REGIp, _null_,
  _REGIc, _NONE_, _null_, _null_, _null_, _REG0_, _null_, _null_,
  _REGIc, _null_, _null_, _null_, _null_, _REG0_, _REGIc, _null_,
  _REGI_, _NONE_, _null_, _NONE_, _REGIi, _REGIc, _REGIp, _NONE_,
  _REGI_, _NONE_, _REGIc, _null_, _REGIi, _REGIc, _REGIp, _null_,
  _REGI_, _NONE_, _null_, _null_, _REGIi, _null_, _REGIp, _null_,
  _REGI_, _NONE_, _null_, _null_, _REGIi, _null_, _REGIp, _null_
};

#endif NO_UNDOC_CODE


/* Be careful when using any of these global variables...
   they might change if swapping is triggered */
unsigned char  FPU_rm;
char	       FPU_st0_tag;
FPU_REG       *FPU_st0_ptr;

/* ######## To be shifted */
unsigned long FPU_entry_op_cs;
unsigned short FPU_data_selector;


#ifdef PARANOID
char emulating=0;
#endif PARANOID

static int valid_prefix(unsigned char *Byte, unsigned char **fpu_eip,
			overrides *override);


asmlinkage void math_emulate(long arg)
{
  unsigned char  FPU_modrm, byte1;
  unsigned short code;
  fpu_addr_modes addr_modes;
  int unmasked;

#ifdef PARANOID
  if ( emulating )
    {
      printk("ERROR: wm-FPU-emu is not RE-ENTRANT!\n");
    }
  RE_ENTRANT_CHECK_ON;
#endif PARANOID

  if (!current->used_math)
    {
      int i;
      for ( i = 0; i < 8; i++ )
	{
	  /* Make sure that the registers are compatible
	     with the assumptions of the emulator. */
	  regs[i].exp = 0;
	  regs[i].sigh = 0x80000000;
	}
      finit();
      current->used_math = 1;
    }

  SETUP_DATA_AREA(arg);

  addr_modes.vm86 = (FPU_EFLAGS & 0x00020000) != 0;

  if ( addr_modes.vm86 )
    FPU_EIP += FPU_CS << 4;

  FPU_ORIG_EIP = FPU_EIP;

  if ( !addr_modes.vm86 )
    {
      /* user code space? */
      if (FPU_CS == KERNEL_CS)
	{
	  printk("math_emulate: %04x:%08lx\n",FPU_CS,FPU_EIP);
	  panic("Math emulation needed in kernel");
	}

      /* We cannot handle multiple segments yet */
      if (FPU_CS != USER_CS || FPU_DS != USER_DS)
	{
	  math_abort(FPU_info,SIGILL);
	}
    }

  FPU_lookahead = 1;
  if (current->flags & PF_PTRACED)
    FPU_lookahead = 0;

  if ( !valid_prefix(&byte1, (unsigned char **)&FPU_EIP,
		     &addr_modes.override) )
    {
      RE_ENTRANT_CHECK_OFF;
      printk("FPU emulator: Unknown prefix byte 0x%02x, probably due to\n"
	     "FPU emulator: self-modifying code! (emulation impossible)\n",
	     byte1);
      RE_ENTRANT_CHECK_ON;
      EXCEPTION(EX_INTERNAL|0x126);
      math_abort(FPU_info,SIGILL);
    }

do_another_FPU_instruction:

  FPU_EIP++;  /* We have fetched the prefix and first code bytes. */

#ifdef PECULIAR_486
  /* It would be more logical to do this only in get_address(),
     but although it is supposed to be undefined for many fpu
     instructions, an 80486 behaves as if this were done here: */
  FPU_data_selector = FPU_DS;
#endif PECULIAR_486

  if ( (byte1 & 0xf8) != 0xd8 )
    {
      if ( byte1 == FWAIT_OPCODE )
	{
	  if (partial_status & SW_Summary)
	    goto do_the_FPU_interrupt;
	  else
	    goto FPU_fwait_done;
	}
#ifdef PARANOID
      EXCEPTION(EX_INTERNAL|0x128);
      math_abort(FPU_info,SIGILL);
#endif PARANOID
    }

  RE_ENTRANT_CHECK_OFF;
  FPU_code_verify_area(1);
  FPU_modrm = get_fs_byte((unsigned short *) FPU_EIP);
  RE_ENTRANT_CHECK_ON;
  FPU_EIP++;

  if (partial_status & SW_Summary)
    {
      /* Ignore the error for now if the current instruction is a no-wait
	 control instruction */
      /* The 80486 manual contradicts itself on this topic,
	 but a real 80486 uses the following instructions:
	 fninit, fnstenv, fnsave, fnstsw, fnstenv, fnclex.
       */
      code = (FPU_modrm << 8) | byte1;
      if ( ! ( (((code & 0xf803) == 0xe003) ||    /* fnclex, fninit, fnstsw */
		(((code & 0x3003) == 0x3001) &&   /* fnsave, fnstcw, fnstenv,
						     fnstsw */
		 ((code & 0xc000) != 0xc000))) ) )
	{
	  /*
	   *  We need to simulate the action of the kernel to FPU
	   *  interrupts here.
	   *  Currently, the "real FPU" part of the kernel (0.99.10)
	   *  clears the exception flags, sets the registers to empty,
	   *  and passes information back to the interrupted process
	   *  via the cs selector and operand selector, so we do the same.
	   */
	do_the_FPU_interrupt:
	  cs_selector &= 0xffff0000;
	  cs_selector |= status_word();
      	  operand_selector = tag_word();
	  partial_status = 0;
	  top = 0;
	  {
	    int r;
	    for (r = 0; r < 8; r++)
	      {
		regs[r].tag = TW_Empty;
	      }
	  }

	  RE_ENTRANT_CHECK_OFF;
	  current->tss.trap_no = 16;
	  current->tss.error_code = 0;
	  send_sig(SIGFPE, current, 1);
	  return;
	}
    }

  FPU_entry_eip = FPU_ORIG_EIP;

  FPU_entry_op_cs = (byte1 << 24) | (FPU_modrm << 16) | (FPU_CS & 0xffff) ;

  FPU_rm = FPU_modrm & 7;

  if ( FPU_modrm < 0300 )
    {
      /* All of these instructions use the mod/rm byte to get a data address */
      if ( addr_modes.vm86
	  ^ (addr_modes.override.address_size == ADDR_SIZE_PREFIX) )
	get_address_16(FPU_modrm, &FPU_EIP, addr_modes);
      else
	get_address(FPU_modrm, &FPU_EIP, addr_modes);
      if ( !(byte1 & 1) )
	{
	  unsigned short status1 = partial_status;
	  FPU_st0_ptr = &st(0);
	  FPU_st0_tag = FPU_st0_ptr->tag;

	  /* Stack underflow has priority */
	  if ( NOT_EMPTY_0 )
	    {
	      unmasked = 0;  /* Do this here to stop compiler warnings. */
	      switch ( (byte1 >> 1) & 3 )
		{
		case 0:
		  unmasked = reg_load_single();
		  break;
		case 1:
		  reg_load_int32();
		  break;
		case 2:
		  unmasked = reg_load_double();
		  break;
		case 3:
		  reg_load_int16();
		  break;
		}
	      
	      /* No more access to user memory, it is safe
		 to use static data now */
	      FPU_st0_ptr = &st(0);
	      FPU_st0_tag = FPU_st0_ptr->tag;

	      /* NaN operands have the next priority. */
	      /* We have to delay looking at st(0) until after
		 loading the data, because that data might contain an SNaN */
	      if ( (FPU_st0_tag == TW_NaN) ||
		  (FPU_loaded_data.tag == TW_NaN) )
		{
		  /* Restore the status word; we might have loaded a
		     denormal. */
		  partial_status = status1;
		  if ( (FPU_modrm & 0x30) == 0x10 )
		    {
		      /* fcom or fcomp */
		      EXCEPTION(EX_Invalid);
		      setcc(SW_C3 | SW_C2 | SW_C0);
		      if ( (FPU_modrm & 0x08) && (control_word & CW_Invalid) )
			pop();             /* fcomp, masked, so we pop. */
		    }
		  else
		    {
#ifdef PECULIAR_486
		      /* This is not really needed, but gives behaviour
			 identical to an 80486 */
		      if ( (FPU_modrm & 0x28) == 0x20 )
			/* fdiv or fsub */
			real_2op_NaN(&FPU_loaded_data, FPU_st0_ptr,
				     FPU_st0_ptr);
		      else
#endif PECULIAR_486
			/* fadd, fdivr, fmul, or fsubr */
			real_2op_NaN(FPU_st0_ptr, &FPU_loaded_data,
				     FPU_st0_ptr);
		    }
		  goto reg_mem_instr_done;
		}

	      if ( unmasked && !((FPU_modrm & 0x30) == 0x10) )
		{
		  /* Is not a comparison instruction. */
		  if ( (FPU_modrm & 0x38) == 0x38 )
		    {
		      /* fdivr */
		      if ( (FPU_st0_tag == TW_Zero) &&
			  (FPU_loaded_data.tag == TW_Valid) )
			{
			  if ( divide_by_zero(FPU_loaded_data.sign,
					      FPU_st0_ptr) )
			    {
			      /* We use the fact here that the unmasked
				 exception in the loaded data was for a
				 denormal operand */
			      /* Restore the state of the denormal op bit */
			      partial_status &= ~SW_Denorm_Op;
			      partial_status |= status1 & SW_Denorm_Op;
			    }
			}
		    }
		  goto reg_mem_instr_done;
		}

	      switch ( (FPU_modrm >> 3) & 7 )
		{
		case 0:         /* fadd */
		  clear_C1();
		  reg_add(FPU_st0_ptr, &FPU_loaded_data, FPU_st0_ptr,
			  control_word);
		  break;
		case 1:         /* fmul */
		  clear_C1();
		  reg_mul(FPU_st0_ptr, &FPU_loaded_data, FPU_st0_ptr,
			  control_word);
		  break;
		case 2:         /* fcom */
		  compare_st_data();
		  break;
		case 3:         /* fcomp */
		  if ( !compare_st_data() && !unmasked )
		    pop();
		  break;
		case 4:         /* fsub */
		  clear_C1();
		  reg_sub(FPU_st0_ptr, &FPU_loaded_data, FPU_st0_ptr,
			  control_word);
		  break;
		case 5:         /* fsubr */
		  clear_C1();
		  reg_sub(&FPU_loaded_data, FPU_st0_ptr, FPU_st0_ptr,
			  control_word);
		  break;
		case 6:         /* fdiv */
		  clear_C1();
		  reg_div(FPU_st0_ptr, &FPU_loaded_data, FPU_st0_ptr,
			  control_word);
		  break;
		case 7:         /* fdivr */
		  clear_C1();
		  if ( FPU_st0_tag == TW_Zero )
		    partial_status = status1;  /* Undo any denorm tag,
					       zero-divide has priority. */
		  reg_div(&FPU_loaded_data, FPU_st0_ptr, FPU_st0_ptr,
			  control_word);
		  break;
		}
	    }
	  else
	    {
	      if ( (FPU_modrm & 0x30) == 0x10 )
		{
		  /* The instruction is fcom or fcomp */
		  EXCEPTION(EX_StackUnder);
		  setcc(SW_C3 | SW_C2 | SW_C0);
		  if ( (FPU_modrm & 0x08) && (control_word & CW_Invalid) )
		    pop();             /* fcomp */
		}
	      else
		stack_underflow();
	    }
	}
      else
	{
	  load_store_instr(((FPU_modrm & 0x38) | (byte1 & 6)) >> 1,
			   addr_modes);
	}

    reg_mem_instr_done:

#ifndef PECULIAR_486
      *(unsigned short *)&operand_selector = FPU_data_selector;
#endif PECULIAR_486
      ;
    }
  else
    {
      /* None of these instructions access user memory */
      unsigned char instr_index = (FPU_modrm & 0x38) | (byte1 & 7);

#ifdef PECULIAR_486
      /* This is supposed to be undefined, but a real 80486 seems
	 to do this: */
      FPU_data_address = 0;
#endif PECULIAR_486

      FPU_st0_ptr = &st(0);
      FPU_st0_tag = FPU_st0_ptr->tag;
      switch ( type_table[(int) instr_index] )
	{
	case _NONE_:   /* also _REGIc: _REGIn */
	  break;
	case _REG0_:
	  if ( !NOT_EMPTY_0 )
	    {
	      stack_underflow();
	      goto FPU_instruction_done;
	    }
	  break;
	case _REGIi:
	  if ( !NOT_EMPTY_0 || !NOT_EMPTY(FPU_rm) )
	    {
	      stack_underflow_i(FPU_rm);
	      goto FPU_instruction_done;
	    }
	  break;
	case _REGIp:
	  if ( !NOT_EMPTY_0 || !NOT_EMPTY(FPU_rm) )
	    {
	      stack_underflow_pop(FPU_rm);
	      goto FPU_instruction_done;
	    }
	  break;
	case _REGI_:
	  if ( !NOT_EMPTY_0 || !NOT_EMPTY(FPU_rm) )
	    {
	      stack_underflow();
	      goto FPU_instruction_done;
	    }
	  break;
	case _PUSH_:     /* Only used by the fld st(i) instruction */
	  break;
	case _null_:
	  FPU_illegal();
	  goto FPU_instruction_done;
	default:
	  EXCEPTION(EX_INTERNAL|0x111);
	  goto FPU_instruction_done;
	}
      (*st_instr_table[(int) instr_index])();
    }

FPU_instruction_done:

  ip_offset = FPU_entry_eip;
  cs_selector = FPU_entry_op_cs;
  data_operand_offset = (unsigned long)FPU_data_address;
#ifdef PECULIAR_486
  *(unsigned short *)&operand_selector = FPU_data_selector;
#endif PECULIAR_486
  
FPU_fwait_done:

#ifdef DEBUG
  RE_ENTRANT_CHECK_OFF;
  emu_printall();
  RE_ENTRANT_CHECK_ON;
#endif DEBUG

  if (FPU_lookahead && !need_resched)
    {
      FPU_ORIG_EIP = FPU_EIP;
      if ( valid_prefix(&byte1, (unsigned char **)&FPU_EIP,
			&addr_modes.override) )
	goto do_another_FPU_instruction;
    }

  if ( addr_modes.vm86 )
    FPU_EIP -= FPU_CS << 4;

  RE_ENTRANT_CHECK_OFF;
}


/* Support for prefix bytes is not yet complete. To properly handle
   all prefix bytes, further changes are needed in the emulator code
   which accesses user address space. Access to separate segments is
   important for msdos emulation. */
static int valid_prefix(unsigned char *Byte, unsigned char **fpu_eip,
			overrides *override)
{
  unsigned char byte;
  unsigned char *ip = *fpu_eip;

  *override = (overrides) { 0, 0, PREFIX_DS_ };       /* defaults */

  RE_ENTRANT_CHECK_OFF;
  FPU_code_verify_area(1);
  byte = get_fs_byte(ip);
  RE_ENTRANT_CHECK_ON;

  while ( 1 )
    {
      switch ( byte )
	{
	case ADDR_SIZE_PREFIX:
	  override->address_size = ADDR_SIZE_PREFIX;
	  goto do_next_byte;

	case OP_SIZE_PREFIX:
	  override->operand_size = OP_SIZE_PREFIX;
	  goto do_next_byte;

	case PREFIX_CS:
	  override->segment = PREFIX_CS_;
	  goto do_next_byte;
	case PREFIX_ES:
	  override->segment = PREFIX_ES_;
	  goto do_next_byte;
	case PREFIX_SS:
	  override->segment = PREFIX_SS_;
	  goto do_next_byte;
	case PREFIX_FS:
	  override->segment = PREFIX_FS_;
	  goto do_next_byte;
	case PREFIX_GS:
	  override->segment = PREFIX_GS_;
	  goto do_next_byte;

	case PREFIX_DS:   /* Redundant unless preceded by another override. */
	  override->segment = PREFIX_DS_;

/* lock is not a valid prefix for FPU instructions,
   let the cpu handle it to generate a SIGILL. */
/*	case PREFIX_LOCK: */

	  /* rep.. prefixes have no meaning for FPU instructions */
	case PREFIX_REPE:
	case PREFIX_REPNE:

	do_next_byte:
	  ip++;
	  RE_ENTRANT_CHECK_OFF;
	  FPU_code_verify_area(1);
	  byte = get_fs_byte(ip);
	  RE_ENTRANT_CHECK_ON;
	  break;
	case FWAIT_OPCODE:
	  *Byte = byte;
	  return 1;
	default:
	  if ( (byte & 0xf8) == 0xd8 )
	    {
	      *Byte = byte;
	      *fpu_eip = ip;
	      return 1;
	    }
	  else
	    {
	      /* Not a valid sequence of prefix bytes followed by
		 an FPU instruction. */
	      *Byte = byte;  /* Needed for error message. */
	      return 0;
	    }
	}
    }
}


void math_abort(struct info * info, unsigned int signal)
{
	FPU_EIP = FPU_ORIG_EIP;
	current->tss.trap_no = 16;
	current->tss.error_code = 0;
	send_sig(signal,current,1);
	RE_ENTRANT_CHECK_OFF;
	__asm__("movl %0,%%esp ; ret": :"g" (((long) info)-4));
#ifdef PARANOID
      printk("ERROR: wm-FPU-emu math_abort failed!\n");
#endif PARANOID
}
