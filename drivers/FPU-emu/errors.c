/*---------------------------------------------------------------------------+
 |  errors.c                                                                 |
 |                                                                           |
 |  The error handling functions for wm-FPU-emu                              |
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

#include <linux/signal.h>

#include <asm/segment.h>

#include "fpu_system.h"
#include "exception.h"
#include "fpu_emu.h"
#include "status_w.h"
#include "control_w.h"
#include "reg_constant.h"
#include "version.h"

/* */
#undef PRINT_MESSAGES
/* */


void Un_impl(void)
{
  unsigned char byte1, FPU_modrm;
  unsigned long address = FPU_ORIG_EIP;

  RE_ENTRANT_CHECK_OFF;
  /* No need to verify_area(), we have previously fetched these bytes. */
  printk("Unimplemented FPU Opcode at eip=%p : ", (void *) address);
  while ( 1 )
    {
      byte1 = get_fs_byte((unsigned char *) address);
      if ( (byte1 & 0xf8) == 0xd8 ) break;
      printk("[%02x]", byte1);
      address++;
    }
  printk("%02x ", byte1);
  FPU_modrm = get_fs_byte(1 + (unsigned char *) address);

  if (FPU_modrm >= 0300)
    printk("%02x (%02x+%d)\n", FPU_modrm, FPU_modrm & 0xf8, FPU_modrm & 7);
  else
    printk("/%d\n", (FPU_modrm >> 3) & 7);
  RE_ENTRANT_CHECK_ON;

  EXCEPTION(EX_Invalid);

}


/*
   Called for opcodes which are illegal and which are known to result in a
   SIGILL with a real 80486.
   */
void FPU_illegal(void)
{
  math_abort(FPU_info,SIGILL);
}



void emu_printall()
{
  int i;
  static char *tag_desc[] = { "Valid", "Zero", "ERROR", "ERROR",
                              "DeNorm", "Inf", "NaN", "Empty" };
  unsigned char byte1, FPU_modrm;
  unsigned long address = FPU_ORIG_EIP;

  RE_ENTRANT_CHECK_OFF;
  /* No need to verify_area(), we have previously fetched these bytes. */
  printk("At %p:", (void *) address);
#define MAX_PRINTED_BYTES 20
  for ( i = 0; i < MAX_PRINTED_BYTES; i++ )
    {
      byte1 = get_fs_byte((unsigned char *) address);
      if ( (byte1 & 0xf8) == 0xd8 )
	{
	  printk(" %02x", byte1);
	  break;
	}
      printk(" [%02x]", byte1);
      address++;
    }
  if ( i == MAX_PRINTED_BYTES )
    printk(" [more..]\n");
  else
    {
      FPU_modrm = get_fs_byte(1 + (unsigned char *) address);

      if (FPU_modrm >= 0300)
	printk(" %02x (%02x+%d)\n", FPU_modrm, FPU_modrm & 0xf8, FPU_modrm & 7);
      else
	printk(" /%d, mod=%d rm=%d\n",
	       (FPU_modrm >> 3) & 7, (FPU_modrm >> 6) & 3, FPU_modrm & 7);
    }

  partial_status = status_word();

#ifdef DEBUGGING
if ( partial_status & SW_Backward )    printk("SW: backward compatibility\n");
if ( partial_status & SW_C3 )          printk("SW: condition bit 3\n");
if ( partial_status & SW_C2 )          printk("SW: condition bit 2\n");
if ( partial_status & SW_C1 )          printk("SW: condition bit 1\n");
if ( partial_status & SW_C0 )          printk("SW: condition bit 0\n");
if ( partial_status & SW_Summary )     printk("SW: exception summary\n");
if ( partial_status & SW_Stack_Fault ) printk("SW: stack fault\n");
if ( partial_status & SW_Precision )   printk("SW: loss of precision\n");
if ( partial_status & SW_Underflow )   printk("SW: underflow\n");
if ( partial_status & SW_Overflow )    printk("SW: overflow\n");
if ( partial_status & SW_Zero_Div )    printk("SW: divide by zero\n");
if ( partial_status & SW_Denorm_Op )   printk("SW: denormalized operand\n");
if ( partial_status & SW_Invalid )     printk("SW: invalid operation\n");
#endif DEBUGGING

  printk(" SW: b=%d st=%ld es=%d sf=%d cc=%d%d%d%d ef=%d%d%d%d%d%d\n",
	 partial_status & 0x8000 ? 1 : 0,   /* busy */
	 (partial_status & 0x3800) >> 11,   /* stack top pointer */
	 partial_status & 0x80 ? 1 : 0,     /* Error summary status */
	 partial_status & 0x40 ? 1 : 0,     /* Stack flag */
	 partial_status & SW_C3?1:0, partial_status & SW_C2?1:0, /* cc */
	 partial_status & SW_C1?1:0, partial_status & SW_C0?1:0, /* cc */
	 partial_status & SW_Precision?1:0, partial_status & SW_Underflow?1:0,
	 partial_status & SW_Overflow?1:0, partial_status & SW_Zero_Div?1:0,
	 partial_status & SW_Denorm_Op?1:0, partial_status & SW_Invalid?1:0);
  
printk(" CW: ic=%d rc=%ld%ld pc=%ld%ld iem=%d     ef=%d%d%d%d%d%d\n",
	 control_word & 0x1000 ? 1 : 0,
	 (control_word & 0x800) >> 11, (control_word & 0x400) >> 10,
	 (control_word & 0x200) >> 9, (control_word & 0x100) >> 8,
	 control_word & 0x80 ? 1 : 0,
	 control_word & SW_Precision?1:0, control_word & SW_Underflow?1:0,
	 control_word & SW_Overflow?1:0, control_word & SW_Zero_Div?1:0,
	 control_word & SW_Denorm_Op?1:0, control_word & SW_Invalid?1:0);

  for ( i = 0; i < 8; i++ )
    {
      FPU_REG *r = &st(i);
      switch (r->tag)
	{
	case TW_Empty:
	  continue;
	  break;
	case TW_Zero:
#if 0
	  printk("st(%d)  %c .0000 0000 0000 0000         ",
		 i, r->sign ? '-' : '+');
	  break;
#endif
	case TW_Valid:
	case TW_NaN:
/*	case TW_Denormal: */
	case TW_Infinity:
	  printk("st(%d)  %c .%04lx %04lx %04lx %04lx e%+-6ld ", i,
		 r->sign ? '-' : '+',
		 (long)(r->sigh >> 16),
		 (long)(r->sigh & 0xFFFF),
		 (long)(r->sigl >> 16),
		 (long)(r->sigl & 0xFFFF),
		 r->exp - EXP_BIAS + 1);
	  break;
	default:
	  printk("Whoops! Error in errors.c      ");
	  break;
	}
      printk("%s\n", tag_desc[(int) (unsigned) r->tag]);
    }

  printk("[data] %c .%04lx %04lx %04lx %04lx e%+-6ld ",
	 FPU_loaded_data.sign ? '-' : '+',
	 (long)(FPU_loaded_data.sigh >> 16),
	 (long)(FPU_loaded_data.sigh & 0xFFFF),
	 (long)(FPU_loaded_data.sigl >> 16),
	 (long)(FPU_loaded_data.sigl & 0xFFFF),
	 FPU_loaded_data.exp - EXP_BIAS + 1);
  printk("%s\n", tag_desc[(int) (unsigned) FPU_loaded_data.tag]);
  RE_ENTRANT_CHECK_ON;

}

static struct {
  int type;
  char *name;
} exception_names[] = {
  { EX_StackOver, "stack overflow" },
  { EX_StackUnder, "stack underflow" },
  { EX_Precision, "loss of precision" },
  { EX_Underflow, "underflow" },
  { EX_Overflow, "overflow" },
  { EX_ZeroDiv, "divide by zero" },
  { EX_Denormal, "denormalized operand" },
  { EX_Invalid, "invalid operation" },
  { EX_INTERNAL, "INTERNAL BUG in "FPU_VERSION },
  { 0, NULL }
};

/*
 EX_INTERNAL is always given with a code which indicates where the
 error was detected.

 Internal error types:
       0      in load_store.c
       0x14   in fpu_etc.c
       0x1nn  in a *.c file:
              0x101  in reg_add_sub.c
              0x102  in reg_mul.c
              0x103  in poly_sin.c
              0x104  in poly_atan.c
              0x105  in reg_mul.c
	      0x106  in reg_ld_str.c
              0x107  in fpu_trig.c
	      0x108  in reg_compare.c
	      0x109  in reg_compare.c
	      0x110  in reg_add_sub.c
	      0x111  in fpe_entry.c
	      0x112  in fpu_trig.c
	      0x113  in errors.c
	      0x114  in reg_ld_str.c
	      0x115  in fpu_trig.c
	      0x116  in fpu_trig.c
	      0x117  in fpu_trig.c
	      0x118  in fpu_trig.c
	      0x119  in fpu_trig.c
	      0x120  in poly_atan.c
	      0x121  in reg_compare.c
	      0x122  in reg_compare.c
	      0x123  in reg_compare.c
	      0x125  in fpu_trig.c
	      0x126  in fpu_entry.c
	      0x127  in poly_2xm1.c
	      0x128  in fpu_entry.c
	      0x130  in get_address.c
       0x2nn  in an *.S file:
              0x201  in reg_u_add.S
              0x202  in reg_u_div.S
              0x203  in reg_u_div.S
              0x204  in reg_u_div.S
              0x205  in reg_u_mul.S
              0x206  in reg_u_sub.S
              0x207  in wm_sqrt.S
	      0x208  in reg_div.S
              0x209  in reg_u_sub.S
              0x210  in reg_u_sub.S
              0x211  in reg_u_sub.S
              0x212  in reg_u_sub.S
	      0x213  in wm_sqrt.S
	      0x214  in wm_sqrt.S
	      0x215  in wm_sqrt.S
	      0x220  in reg_norm.S
	      0x221  in reg_norm.S
	      0x230  in reg_round.S
	      0x231  in reg_round.S
	      0x232  in reg_round.S
	      0x233  in reg_round.S
	      0x234  in reg_round.S
	      0x235  in reg_round.S
	      0x236  in reg_round.S
 */

void exception(int n)
{
  int i, int_type;

  int_type = 0;         /* Needed only to stop compiler warnings */
  if ( n & EX_INTERNAL )
    {
      int_type = n - EX_INTERNAL;
      n = EX_INTERNAL;
      /* Set lots of exception bits! */
      partial_status |= (SW_Exc_Mask | SW_Summary | SW_Backward);
    }
  else
    {
      /* Extract only the bits which we use to set the status word */
      n &= (SW_Exc_Mask);
      /* Set the corresponding exception bit */
      partial_status |= n;
      /* Set summary bits iff exception isn't masked */
      if ( partial_status & ~control_word & CW_Exceptions )
	partial_status |= (SW_Summary | SW_Backward);
      if ( n & (SW_Stack_Fault | EX_Precision) )
	{
	  if ( !(n & SW_C1) )
	    /* This bit distinguishes over- from underflow for a stack fault,
	       and roundup from round-down for precision loss. */
	    partial_status &= ~SW_C1;
	}
    }

  RE_ENTRANT_CHECK_OFF;
  if ( (~control_word & n & CW_Exceptions) || (n == EX_INTERNAL) )
    {
#ifdef PRINT_MESSAGES
      /* My message from the sponsor */
      printk(FPU_VERSION" "__DATE__" (C) W. Metzenthen.\n");
#endif PRINT_MESSAGES
      
      /* Get a name string for error reporting */
      for (i=0; exception_names[i].type; i++)
	if ( (exception_names[i].type & n) == exception_names[i].type )
	  break;
      
      if (exception_names[i].type)
	{
#ifdef PRINT_MESSAGES
	  printk("FP Exception: %s!\n", exception_names[i].name);
#endif PRINT_MESSAGES
	}
      else
	printk("FPU emulator: Unknown Exception: 0x%04x!\n", n);
      
      if ( n == EX_INTERNAL )
	{
	  printk("FPU emulator: Internal error type 0x%04x\n", int_type);
	  emu_printall();
	}
#ifdef PRINT_MESSAGES
      else
	emu_printall();
#endif PRINT_MESSAGES

      /*
       * The 80486 generates an interrupt on the next non-control FPU
       * instruction. So we need some means of flagging it.
       * We use the ES (Error Summary) bit for this, assuming that
       * this is the way a real FPU does it (until I can check it out),
       * if not, then some method such as the following kludge might
       * be needed.
       */
/*      regs[0].tag |= TW_FPU_Interrupt; */
    }
  RE_ENTRANT_CHECK_ON;

#ifdef __DEBUG__
  math_abort(FPU_info,SIGFPE);
#endif __DEBUG__

}


/* Real operation attempted on two operands, one a NaN. */
/* Returns nz if the exception is unmasked */
asmlinkage int real_2op_NaN(FPU_REG const *a, FPU_REG const *b, FPU_REG *dest)
{
  FPU_REG const *x;
  int signalling;

  /* The default result for the case of two "equal" NaNs (signs may
     differ) is chosen to reproduce 80486 behaviour */
  x = a;
  if (a->tag == TW_NaN)
    {
      if (b->tag == TW_NaN)
	{
	  signalling = !(a->sigh & b->sigh & 0x40000000);
	  /* find the "larger" */
	  if ( significand(a) < significand(b) )
	    x = b;
	}
      else
	{
	  /* return the quiet version of the NaN in a */
	  signalling = !(a->sigh & 0x40000000);
	}
    }
  else
#ifdef PARANOID
    if (b->tag == TW_NaN)
#endif PARANOID
    {
      signalling = !(b->sigh & 0x40000000);
      x = b;
    }
#ifdef PARANOID
  else
    {
      signalling = 0;
      EXCEPTION(EX_INTERNAL|0x113);
      x = &CONST_QNaN;
    }
#endif PARANOID

  if ( !signalling )
    {
      if ( !(x->sigh & 0x80000000) )  /* pseudo-NaN ? */
	x = &CONST_QNaN;
      reg_move(x, dest);
      return 0;
    }

  if ( control_word & CW_Invalid )
    {
      /* The masked response */
      if ( !(x->sigh & 0x80000000) )  /* pseudo-NaN ? */
	x = &CONST_QNaN;
      reg_move(x, dest);
      /* ensure a Quiet NaN */
      dest->sigh |= 0x40000000;
    }

  EXCEPTION(EX_Invalid);
  
  return !(control_word & CW_Invalid);
}


/* Invalid arith operation on Valid registers */
/* Returns nz if the exception is unmasked */
asmlinkage int arith_invalid(FPU_REG *dest)
{

  EXCEPTION(EX_Invalid);
  
  if ( control_word & CW_Invalid )
    {
      /* The masked response */
      reg_move(&CONST_QNaN, dest);
    }
  
  return !(control_word & CW_Invalid);

}


/* Divide a finite number by zero */
asmlinkage int divide_by_zero(int sign, FPU_REG *dest)
{

  if ( control_word & CW_ZeroDiv )
    {
      /* The masked response */
      reg_move(&CONST_INF, dest);
      dest->sign = (unsigned char)sign;
    }
 
  EXCEPTION(EX_ZeroDiv);

  return !(control_word & CW_ZeroDiv);

}


/* This may be called often, so keep it lean */
int set_precision_flag(int flags)
{
  if ( control_word & CW_Precision )
    {
      partial_status &= ~(SW_C1 & flags);
      partial_status |= flags;   /* The masked response */
      return 0;
    }
  else
    {
      exception(flags);
      return 1;
    }
}


/* This may be called often, so keep it lean */
asmlinkage void set_precision_flag_up(void)
{
  if ( control_word & CW_Precision )
    partial_status |= (SW_Precision | SW_C1);   /* The masked response */
  else
    exception(EX_Precision | SW_C1);

}


/* This may be called often, so keep it lean */
asmlinkage void set_precision_flag_down(void)
{
  if ( control_word & CW_Precision )
    {   /* The masked response */
      partial_status &= ~SW_C1;
      partial_status |= SW_Precision;
    }
  else
    exception(EX_Precision);
}


asmlinkage int denormal_operand(void)
{
  if ( control_word & CW_Denormal )
    {   /* The masked response */
      partial_status |= SW_Denorm_Op;
      return 0;
    }
  else
    {
      exception(EX_Denormal);
      return 1;
    }
}


asmlinkage int arith_overflow(FPU_REG *dest)
{

  if ( control_word & CW_Overflow )
    {
      char sign;
      /* The masked response */
/* ###### The response here depends upon the rounding mode */
      sign = dest->sign;
      reg_move(&CONST_INF, dest);
      dest->sign = sign;
    }
  else
    {
      /* Subtract the magic number from the exponent */
      dest->exp -= (3 * (1 << 13));
    }

  EXCEPTION(EX_Overflow);
  if ( control_word & CW_Overflow )
    {
      /* The overflow exception is masked. */
      /* By definition, precision is lost.
	 The roundup bit (C1) is also set because we have
	 "rounded" upwards to Infinity. */
      EXCEPTION(EX_Precision | SW_C1);
      return !(control_word & CW_Precision);
    }

  return !(control_word & CW_Overflow);

}


asmlinkage int arith_underflow(FPU_REG *dest)
{

  if ( control_word & CW_Underflow )
    {
      /* The masked response */
      if ( dest->exp <= EXP_UNDER - 63 )
	{
	  reg_move(&CONST_Z, dest);
	  partial_status &= ~SW_C1;       /* Round down. */
	}
    }
  else
    {
      /* Add the magic number to the exponent. */
      dest->exp += (3 * (1 << 13));
    }

  EXCEPTION(EX_Underflow);
  if ( control_word & CW_Underflow )
    {
      /* The underflow exception is masked. */
      EXCEPTION(EX_Precision);
      return !(control_word & CW_Precision);
    }

  return !(control_word & CW_Underflow);

}


void stack_overflow(void)
{

 if ( control_word & CW_Invalid )
    {
      /* The masked response */
      top--;
      reg_move(&CONST_QNaN, FPU_st0_ptr = &st(0));
    }

  EXCEPTION(EX_StackOver);

  return;

}


void stack_underflow(void)
{

 if ( control_word & CW_Invalid )
    {
      /* The masked response */
      reg_move(&CONST_QNaN, FPU_st0_ptr);
    }

  EXCEPTION(EX_StackUnder);

  return;

}


void stack_underflow_i(int i)
{

 if ( control_word & CW_Invalid )
    {
      /* The masked response */
      reg_move(&CONST_QNaN, &(st(i)));
    }

  EXCEPTION(EX_StackUnder);

  return;

}


void stack_underflow_pop(int i)
{

 if ( control_word & CW_Invalid )
    {
      /* The masked response */
      reg_move(&CONST_QNaN, &(st(i)));
      pop();
    }

  EXCEPTION(EX_StackUnder);

  return;

}

