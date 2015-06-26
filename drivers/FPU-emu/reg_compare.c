/*---------------------------------------------------------------------------+
 |  reg_compare.c                                                            |
 |                                                                           |
 | Compare two floating point registers                                      |
 |                                                                           |
 | Copyright (C) 1992,1993,1994                                              |
 |                       W. Metzenthen, 22 Parker St, Ormond, Vic 3163,      |
 |                       Australia.  E-mail   billm@vaxc.cc.monash.edu.au    |
 |                                                                           |
 |                                                                           |
 +---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------+
 | compare() is the core FPU_REG comparison function                         |
 +---------------------------------------------------------------------------*/

#include "fpu_system.h"
#include "exception.h"
#include "fpu_emu.h"
#include "control_w.h"
#include "status_w.h"


int compare(FPU_REG const *b)
{
  int diff;

  if ( FPU_st0_ptr->tag | b->tag )
    {
      if ( FPU_st0_ptr->tag == TW_Zero )
	{
	  if ( b->tag == TW_Zero ) return COMP_A_eq_B;
	  if ( b->tag == TW_Valid )
	    {
	      return ((b->sign == SIGN_POS) ? COMP_A_lt_B : COMP_A_gt_B)
#ifdef DENORM_OPERAND
		| ((b->exp <= EXP_UNDER) ?
		   COMP_Denormal : 0)
#endif DENORM_OPERAND
		  ;
	    }
	}
      else if ( b->tag == TW_Zero )
	{
	  if ( FPU_st0_ptr->tag == TW_Valid )
	    {
	      return ((FPU_st0_ptr->sign == SIGN_POS) ? COMP_A_gt_B
		      : COMP_A_lt_B)
#ifdef DENORM_OPERAND
		| ((FPU_st0_ptr->exp <= EXP_UNDER )
		   ? COMP_Denormal : 0 )
#endif DENORM_OPERAND
		  ;
	    }
	}

      if ( FPU_st0_ptr->tag == TW_Infinity )
	{
	  if ( (b->tag == TW_Valid) || (b->tag == TW_Zero) )
	    {
	      return ((FPU_st0_ptr->sign == SIGN_POS) ? COMP_A_gt_B
		      : COMP_A_lt_B)
#ifdef DENORM_OPERAND
	      | (((b->tag == TW_Valid) && (b->exp <= EXP_UNDER)) ?
		COMP_Denormal : 0 )
#endif DENORM_OPERAND
;
	    }
	  else if ( b->tag == TW_Infinity )
	    {
	      /* The 80486 book says that infinities can be equal! */
	      return (FPU_st0_ptr->sign == b->sign) ? COMP_A_eq_B :
		((FPU_st0_ptr->sign == SIGN_POS) ? COMP_A_gt_B : COMP_A_lt_B);
	    }
	  /* Fall through to the NaN code */
	}
      else if ( b->tag == TW_Infinity )
	{
	  if ( (FPU_st0_ptr->tag == TW_Valid) || (FPU_st0_ptr->tag == TW_Zero) )
	    {
	      return ((b->sign == SIGN_POS) ? COMP_A_lt_B : COMP_A_gt_B)
#ifdef DENORM_OPERAND
		| (((FPU_st0_ptr->tag == TW_Valid)
		    && (FPU_st0_ptr->exp <= EXP_UNDER)) ?
		   COMP_Denormal : 0)
#endif DENORM_OPERAND
		  ;
	    }
	  /* Fall through to the NaN code */
	}

      /* The only possibility now should be that one of the arguments
	 is a NaN */
      if ( (FPU_st0_ptr->tag == TW_NaN) || (b->tag == TW_NaN) )
	{
	  if ( ((FPU_st0_ptr->tag == TW_NaN) && !(FPU_st0_ptr->sigh & 0x40000000))
	      || ((b->tag == TW_NaN) && !(b->sigh & 0x40000000)) )
	    /* At least one arg is a signaling NaN */
	    return COMP_No_Comp | COMP_SNaN | COMP_NaN;
	  else
	    /* Neither is a signaling NaN */
	    return COMP_No_Comp | COMP_NaN;
	}
      
      EXCEPTION(EX_Invalid);
    }
  
#ifdef PARANOID
  if (!(FPU_st0_ptr->sigh & 0x80000000)) EXCEPTION(EX_Invalid);
  if (!(b->sigh & 0x80000000)) EXCEPTION(EX_Invalid);
#endif PARANOID

  
  if (FPU_st0_ptr->sign != b->sign)
    {
      return ((FPU_st0_ptr->sign == SIGN_POS) ? COMP_A_gt_B : COMP_A_lt_B)
#ifdef DENORM_OPERAND
	|
	  ( ((FPU_st0_ptr->exp <= EXP_UNDER) || (b->exp <= EXP_UNDER)) ?
	   COMP_Denormal : 0)
#endif DENORM_OPERAND
	    ;
    }

  diff = FPU_st0_ptr->exp - b->exp;
  if ( diff == 0 )
    {
      diff = FPU_st0_ptr->sigh - b->sigh;  /* Works only if ms bits are
					      identical */
      if ( diff == 0 )
	{
	diff = FPU_st0_ptr->sigl > b->sigl;
	if ( diff == 0 )
	  diff = -(FPU_st0_ptr->sigl < b->sigl);
	}
    }

  if ( diff > 0 )
    {
      return ((FPU_st0_ptr->sign == SIGN_POS) ? COMP_A_gt_B : COMP_A_lt_B)
#ifdef DENORM_OPERAND
	|
	  ( ((FPU_st0_ptr->exp <= EXP_UNDER) || (b->exp <= EXP_UNDER)) ?
	   COMP_Denormal : 0)
#endif DENORM_OPERAND
	    ;
    }
  if ( diff < 0 )
    {
      return ((FPU_st0_ptr->sign == SIGN_POS) ? COMP_A_lt_B : COMP_A_gt_B)
#ifdef DENORM_OPERAND
	|
	  ( ((FPU_st0_ptr->exp <= EXP_UNDER) || (b->exp <= EXP_UNDER)) ?
	   COMP_Denormal : 0)
#endif DENORM_OPERAND
	    ;
    }

  return COMP_A_eq_B
#ifdef DENORM_OPERAND
    |
      ( ((FPU_st0_ptr->exp <= EXP_UNDER) || (b->exp <= EXP_UNDER)) ?
       COMP_Denormal : 0)
#endif DENORM_OPERAND
	;

}


/* This function requires that st(0) is not empty */
int compare_st_data(void)
{
  int f, c;

  c = compare(&FPU_loaded_data);

  if (c & COMP_NaN)
    {
      EXCEPTION(EX_Invalid);
      f = SW_C3 | SW_C2 | SW_C0;
    }
  else
    switch (c & 7)
      {
      case COMP_A_lt_B:
	f = SW_C0;
	break;
      case COMP_A_eq_B:
	f = SW_C3;
	break;
      case COMP_A_gt_B:
	f = 0;
	break;
      case COMP_No_Comp:
	f = SW_C3 | SW_C2 | SW_C0;
	break;
#ifdef PARANOID
      default:
	EXCEPTION(EX_INTERNAL|0x121);
	f = SW_C3 | SW_C2 | SW_C0;
	break;
#endif PARANOID
      }
  setcc(f);
  if (c & COMP_Denormal)
    {
      return denormal_operand();
    }
  return 0;
}


static int compare_st_st(int nr)
{
  int f, c;

  if ( !NOT_EMPTY_0 || !NOT_EMPTY(nr) )
    {
      setcc(SW_C3 | SW_C2 | SW_C0);
      /* Stack fault */
      EXCEPTION(EX_StackUnder);
      return !(control_word & CW_Invalid);
    }

  c = compare(&st(nr));
  if (c & COMP_NaN)
    {
      setcc(SW_C3 | SW_C2 | SW_C0);
      EXCEPTION(EX_Invalid);
      return !(control_word & CW_Invalid);
    }
  else
    switch (c & 7)
      {
      case COMP_A_lt_B:
	f = SW_C0;
	break;
      case COMP_A_eq_B:
	f = SW_C3;
	break;
      case COMP_A_gt_B:
	f = 0;
	break;
      case COMP_No_Comp:
	f = SW_C3 | SW_C2 | SW_C0;
	break;
#ifdef PARANOID
      default:
	EXCEPTION(EX_INTERNAL|0x122);
	f = SW_C3 | SW_C2 | SW_C0;
	break;
#endif PARANOID
      }
  setcc(f);
  if (c & COMP_Denormal)
    {
      return denormal_operand();
    }
  return 0;
}


static int compare_u_st_st(int nr)
{
  int f, c;

  if ( !NOT_EMPTY_0 || !NOT_EMPTY(nr) )
    {
      setcc(SW_C3 | SW_C2 | SW_C0);
      /* Stack fault */
      EXCEPTION(EX_StackUnder);
      return !(control_word & CW_Invalid);
    }

  c = compare(&st(nr));
  if (c & COMP_NaN)
    {
      setcc(SW_C3 | SW_C2 | SW_C0);
      if (c & COMP_SNaN)       /* This is the only difference between
				  un-ordered and ordinary comparisons */
	{
	  EXCEPTION(EX_Invalid);
	  return !(control_word & CW_Invalid);
	}
      return 0;
    }
  else
    switch (c & 7)
      {
      case COMP_A_lt_B:
	f = SW_C0;
	break;
      case COMP_A_eq_B:
	f = SW_C3;
	break;
      case COMP_A_gt_B:
	f = 0;
	break;
      case COMP_No_Comp:
	f = SW_C3 | SW_C2 | SW_C0;
	break;
#ifdef PARANOID
      default:
	EXCEPTION(EX_INTERNAL|0x123);
	f = SW_C3 | SW_C2 | SW_C0;
	break;
#endif PARANOID
      }
  setcc(f);
  if (c & COMP_Denormal)
    {
      return denormal_operand();
    }
  return 0;
}

/*---------------------------------------------------------------------------*/

void fcom_st()
{
  /* fcom st(i) */
  compare_st_st(FPU_rm);
}


void fcompst()
{
  /* fcomp st(i) */
  if ( !compare_st_st(FPU_rm) )
    pop();
}


void fcompp()
{
  /* fcompp */
  if (FPU_rm != 1)
    {
      FPU_illegal();
      return;
    }
  if ( !compare_st_st(1) )
    {
      pop(); FPU_st0_ptr = &st(0);
      pop();
    }
}


void fucom_()
{
  /* fucom st(i) */
  compare_u_st_st(FPU_rm);

}


void fucomp()
{
  /* fucomp st(i) */
  if ( !compare_u_st_st(FPU_rm) )
    pop();
}


void fucompp()
{
  /* fucompp */
  if (FPU_rm == 1)
    {
      if ( !compare_u_st_st(1) )
	{
	  pop(); FPU_st0_ptr = &st(0);
	  pop();
	}
    }
  else
    FPU_illegal();
}
