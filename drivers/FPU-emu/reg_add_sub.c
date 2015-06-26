/*---------------------------------------------------------------------------+
 |  reg_add_sub.c                                                            |
 |                                                                           |
 | Functions to add or subtract two registers and put the result in a third. |
 |                                                                           |
 | Copyright (C) 1992,1993                                                   |
 |                       W. Metzenthen, 22 Parker St, Ormond, Vic 3163,      |
 |                       Australia.  E-mail   billm@vaxc.cc.monash.edu.au    |
 |                                                                           |
 |                                                                           |
 +---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------+
 | For each function, the destination may be any FPU_REG, including one of   |
 | the source FPU_REGs.                                                      |
 +---------------------------------------------------------------------------*/

#include "exception.h"
#include "reg_constant.h"
#include "fpu_emu.h"
#include "control_w.h"
#include "fpu_system.h"


int reg_add(FPU_REG const *a, FPU_REG const *b, FPU_REG *dest, int control_w)
{
  char saved_sign = dest->sign;
  int diff;
  
  if ( !(a->tag | b->tag) )
    {
      /* Both registers are valid */
      if (!(a->sign ^ b->sign))
	{
	  /* signs are the same */
	  dest->sign = a->sign;
	  if ( reg_u_add(a, b, dest, control_w) )
	    {
	      dest->sign = saved_sign;
	      return 1;
	    }
	  return 0;
	}
      
      /* The signs are different, so do a subtraction */
      diff = a->exp - b->exp;
      if (!diff)
	{
	  diff = a->sigh - b->sigh;  /* Works only if ms bits are identical */
	  if (!diff)
	    {
	      diff = a->sigl > b->sigl;
	      if (!diff)
		diff = -(a->sigl < b->sigl);
	    }
	}
      
      if (diff > 0)
	{
	  dest->sign = a->sign;
	  if ( reg_u_sub(a, b, dest, control_w) )
	    {
	      dest->sign = saved_sign;
	      return 1;
	    }
	}
      else if ( diff == 0 )
	{
#ifdef DENORM_OPERAND
	  if ( (b->tag == TW_Valid) && (b->exp <= EXP_UNDER) &&
	      denormal_operand() )
	    return 1;
#endif DENORM_OPERAND
	  reg_move(&CONST_Z, dest);
	  /* sign depends upon rounding mode */
	  dest->sign = ((control_w & CW_RC) != RC_DOWN)
	    ? SIGN_POS : SIGN_NEG;
	}
      else
	{
	  dest->sign = b->sign;
	  if ( reg_u_sub(b, a, dest, control_w) )
	    {
	      dest->sign = saved_sign;
	      return 1;
	    }
	}
      return 0;
    }
  else
    {
      if ( (a->tag == TW_NaN) || (b->tag == TW_NaN) )
	{ return real_2op_NaN(a, b, dest); }
      else if (a->tag == TW_Zero)
	{
	  if (b->tag == TW_Zero)
	    {
	      char different_signs = a->sign ^ b->sign;
	      /* Both are zero, result will be zero. */
	      reg_move(a, dest);
	      if (different_signs)
		{
		  /* Signs are different. */
		  /* Sign of answer depends upon rounding mode. */
		  dest->sign = ((control_w & CW_RC) != RC_DOWN)
		    ? SIGN_POS : SIGN_NEG;
		}
	    }
	  else
	    {
#ifdef DENORM_OPERAND
	      if ( (b->tag == TW_Valid) && (b->exp <= EXP_UNDER) &&
		  denormal_operand() )
		return 1;
#endif DENORM_OPERAND
	      reg_move(b, dest);
	    }
	  return 0;
	}
      else if (b->tag == TW_Zero)
	{
#ifdef DENORM_OPERAND
	  if ( (a->tag == TW_Valid) && (a->exp <= EXP_UNDER) &&
	      denormal_operand() )
	    return 1;
#endif DENORM_OPERAND
	  reg_move(a, dest); return 0;
	}
      else if (a->tag == TW_Infinity)
	{
	  if (b->tag != TW_Infinity)
	    {
#ifdef DENORM_OPERAND
	      if ( (b->tag == TW_Valid) && (b->exp <= EXP_UNDER) &&
		  denormal_operand() )
		return 1;
#endif DENORM_OPERAND
	      reg_move(a, dest); return 0;
	    }
	  if (a->sign == b->sign)
	    {
	      /* They are both + or - infinity */
	      reg_move(a, dest); return 0;
	    }
	  return arith_invalid(dest);	/* Infinity-Infinity is undefined. */
	}
      else if (b->tag == TW_Infinity)
	{
#ifdef DENORM_OPERAND
	  if ( (a->tag == TW_Valid) && (a->exp <= EXP_UNDER) &&
	      denormal_operand() )
	    return 1;
#endif DENORM_OPERAND
	  reg_move(b, dest); return 0;
	}
    }
#ifdef PARANOID
  EXCEPTION(EX_INTERNAL|0x101);
#endif
  return 1;
}


/* Subtract b from a.  (a-b) -> dest */
int reg_sub(FPU_REG const *a, FPU_REG const *b, FPU_REG *dest, int control_w)
{
  char saved_sign = dest->sign;
  int diff;

  if ( !(a->tag | b->tag) )
    {
      /* Both registers are valid */
      diff = a->exp - b->exp;
      if (!diff)
	{
	  diff = a->sigh - b->sigh;  /* Works only if ms bits are identical */
	  if (!diff)
	    {
	      diff = a->sigl > b->sigl;
	      if (!diff)
		diff = -(a->sigl < b->sigl);
	    }
	}

      switch (a->sign*2 + b->sign)
	{
	case 0: /* P - P */
	case 3: /* N - N */
	  if (diff > 0)
	    {
	      /* |a| > |b| */
	      dest->sign = a->sign;
	      if ( reg_u_sub(a, b, dest, control_w) )
		{
		  dest->sign = saved_sign;
		  return 1;
		}
	      return 0;
	    }
	  else if ( diff == 0 )
	    {
#ifdef DENORM_OPERAND
	      if ( (b->tag == TW_Valid) && (b->exp <= EXP_UNDER) &&
		  denormal_operand() )
		return 1;
#endif DENORM_OPERAND
	      reg_move(&CONST_Z, dest);
	      /* sign depends upon rounding mode */
	      dest->sign = ((control_w & CW_RC) != RC_DOWN)
		? SIGN_POS : SIGN_NEG;
	    }
	  else
	    {
	      dest->sign = a->sign ^ SIGN_POS^SIGN_NEG;
	      if ( reg_u_sub(b, a, dest, control_w) )
		{
		  dest->sign = saved_sign;
		  return 1;
		}
	    }
	  break;
	case 1: /* P - N */
	  dest->sign = SIGN_POS;
	  if ( reg_u_add(a, b, dest, control_w) )
	    {
	      dest->sign = saved_sign;
	      return 1;
	    }
	  break;
	case 2: /* N - P */
	  dest->sign = SIGN_NEG;
	  if ( reg_u_add(a, b, dest, control_w) )
	    {
	      dest->sign = saved_sign;
	      return 1;
	    }
	  break;
	}
      return 0;
    }
  else
    {
      if ( (a->tag == TW_NaN) || (b->tag == TW_NaN) )
	{ return real_2op_NaN(b, a, dest); }
      else if (b->tag == TW_Zero)
	{ 
	  if (a->tag == TW_Zero)
	    {
	      char same_signs = !(a->sign ^ b->sign);
	      /* Both are zero, result will be zero. */
	      reg_move(a, dest); /* Answer for different signs. */
	      if (same_signs)
		{
		  /* Sign depends upon rounding mode */
		  dest->sign = ((control_w & CW_RC) != RC_DOWN)
		    ? SIGN_POS : SIGN_NEG;
		}
	    }
	  else
	    {
#ifdef DENORM_OPERAND
	      if ( (a->tag == TW_Valid) && (a->exp <= EXP_UNDER) &&
		  denormal_operand() )
		return 1;
#endif DENORM_OPERAND
	      reg_move(a, dest);
	    }
	  return 0;
	}
      else if (a->tag == TW_Zero)
	{
#ifdef DENORM_OPERAND
	  if ( (b->tag == TW_Valid) && (b->exp <= EXP_UNDER) &&
	      denormal_operand() )
	    return 1;
#endif DENORM_OPERAND
	  reg_move(b, dest);
	  dest->sign ^= SIGN_POS^SIGN_NEG;
	  return 0;
	}
      else if (a->tag == TW_Infinity)
	{
	  if (b->tag != TW_Infinity)
	    {
#ifdef DENORM_OPERAND
	      if ( (b->tag == TW_Valid) && (b->exp <= EXP_UNDER) &&
		  denormal_operand() )
		return 1;
#endif DENORM_OPERAND
	      reg_move(a, dest); return 0;
	    }
	  /* Both args are Infinity */
	  if (a->sign == b->sign)
	    {
	      /* Infinity-Infinity is undefined. */
	      return arith_invalid(dest);
	    }
	  reg_move(a, dest);
	  return 0;
	}
      else if (b->tag == TW_Infinity)
	{
#ifdef DENORM_OPERAND
	  if ( (a->tag == TW_Valid) && (a->exp <= EXP_UNDER) &&
	      denormal_operand() )
	    return 1;
#endif DENORM_OPERAND
	  reg_move(b, dest);
	  dest->sign ^= SIGN_POS^SIGN_NEG;
	  return 0;
	}
    }
#ifdef PARANOID
  EXCEPTION(EX_INTERNAL|0x110);
#endif
  return 1;
}

