/*---------------------------------------------------------------------------+
 |  reg_mul.c                                                                |
 |                                                                           |
 | Multiply one FPU_REG by another, put the result in a destination FPU_REG. |
 |                                                                           |
 | Copyright (C) 1992,1993                                                   |
 |                       W. Metzenthen, 22 Parker St, Ormond, Vic 3163,      |
 |                       Australia.  E-mail   billm@vaxc.cc.monash.edu.au    |
 |                                                                           |
 |                                                                           |
 +---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------+
 | The destination may be any FPU_REG, including one of the source FPU_REGs. |
 +---------------------------------------------------------------------------*/

#include "exception.h"
#include "reg_constant.h"
#include "fpu_emu.h"
#include "fpu_system.h"


/* This routine must be called with non-empty source registers */
int reg_mul(FPU_REG const *a, FPU_REG const *b,
	    FPU_REG *dest, unsigned int control_w)
{
  char saved_sign = dest->sign;
  char sign = (a->sign ^ b->sign);

  if (!(a->tag | b->tag))
    {
      /* Both regs Valid, this should be the most common case. */
      dest->sign = sign;
      if ( reg_u_mul(a, b, dest, control_w) )
	{
	  dest->sign = saved_sign;
	  return 1;
	}
      return 0;
    }
  else if ((a->tag <= TW_Zero) && (b->tag <= TW_Zero))
    {
#ifdef DENORM_OPERAND
      if ( ((b->tag == TW_Valid) && (b->exp <= EXP_UNDER)) ||
	  ((a->tag == TW_Valid) && (a->exp <= EXP_UNDER)) )
	{
	  if ( denormal_operand() ) return 1;
	}
#endif DENORM_OPERAND
      /* Must have either both arguments == zero, or
	 one valid and the other zero.
	 The result is therefore zero. */
      reg_move(&CONST_Z, dest);
      /* The 80486 book says that the answer is +0, but a real
	 80486 behaves this way.
	 IEEE-754 apparently says it should be this way. */
      dest->sign = sign;
      return 0;
    }
  else
    {
      /* Must have infinities, NaNs, etc */
      if ( (a->tag == TW_NaN) || (b->tag == TW_NaN) )
	{ return real_2op_NaN(a, b, dest); }
      else if (a->tag == TW_Infinity)
	{
	  if (b->tag == TW_Zero)
	    { return arith_invalid(dest); }  /* Zero*Infinity is invalid */
	  else
	    {
#ifdef DENORM_OPERAND
	      if ( (b->tag == TW_Valid) && (b->exp <= EXP_UNDER) &&
		  denormal_operand() )
		return 1;
#endif DENORM_OPERAND
	      reg_move(a, dest);
	      dest->sign = sign;
	    }
	  return 0;
	}
      else if (b->tag == TW_Infinity)
	{
	  if (a->tag == TW_Zero)
	    { return arith_invalid(dest); }  /* Zero*Infinity is invalid */
	  else
	    {
#ifdef DENORM_OPERAND
	      if ( (a->tag == TW_Valid) && (a->exp <= EXP_UNDER) &&
		  denormal_operand() )
		return 1;
#endif DENORM_OPERAND
	      reg_move(b, dest);
	      dest->sign = sign;
	    }
	  return 0;
	}
#ifdef PARANOID
      else
	{
	  EXCEPTION(EX_INTERNAL|0x102);
	  return 1;
	}
#endif PARANOID
    }
}
