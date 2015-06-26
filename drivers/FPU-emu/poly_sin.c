/*---------------------------------------------------------------------------+
 |  poly_sin.c                                                               |
 |                                                                           |
 |  Computation of an approximation of the sin function by a polynomial      |
 |                                                                           |
 | Copyright (C) 1992,1993,1994                                              |
 |                       W. Metzenthen, 22 Parker St, Ormond, Vic 3163,      |
 |                       Australia.  E-mail   billm@vaxc.cc.monash.edu.au    |
 |                                                                           |
 |                                                                           |
 +---------------------------------------------------------------------------*/


#include "exception.h"
#include "reg_constant.h"
#include "fpu_emu.h"
#include "control_w.h"


#define	HIPOWER	5
static unsigned short const	lterms[HIPOWER][4] =
	{
	{ 0x846a, 0x42d1, 0xb544, 0x921f},
	{ 0xe110, 0x75aa, 0xbc67, 0x1466},
	{ 0x503d, 0xa43f, 0x83c1, 0x000a},
	{ 0x8f9d, 0x7a19, 0x00f4, 0x0000},
	{ 0xda03, 0x06aa, 0x0000, 0x0000},
	};

static unsigned short const	negterms[HIPOWER][4] =
	{
	{ 0x95ed, 0x2df2, 0xe731, 0xa55d},
	{ 0xd159, 0xe62b, 0xd2cc, 0x0132},
	{ 0x6342, 0xe9fb, 0x3c60, 0x0000},
	{ 0x6256, 0xdf5a, 0x0002, 0x0000},
	{ 0xf279, 0x000b, 0x0000, 0x0000},
	};


/*--- poly_sine() -----------------------------------------------------------+
 |                                                                           |
 +---------------------------------------------------------------------------*/
void	poly_sine(FPU_REG const *arg, FPU_REG *result)
{
  short	exponent;
  FPU_REG	fixed_arg, arg_sqrd, arg_to_4, accum, negaccum;
  
  
  exponent = arg->exp - EXP_BIAS;
  
  if ( arg->tag == TW_Zero )
    {
      /* Return 0.0 */
      reg_move(&CONST_Z, result);
      return;
    }
  
#ifdef PARANOID
  if ( arg->sign != 0 )	/* Can't hack a number < 0.0 */
    {
      EXCEPTION(EX_Invalid);
      reg_move(&CONST_QNaN, result);
      return;
    }
  
  if ( exponent >= 0 )	/* Can't hack a number > 1.0 */
    {
      if ( (exponent == 0) && (arg->sigl == 0) && (arg->sigh == 0x80000000) )
	{
	  reg_move(&CONST_1, result);
	  return;
	}
      EXCEPTION(EX_Invalid);
      reg_move(&CONST_QNaN, result);
      return;
    }
#endif PARANOID
  
  fixed_arg.sigl = arg->sigl;
  fixed_arg.sigh = arg->sigh;
  if ( exponent < -1 )
    {
      /* shift the argument right by the required places */
      if ( shrx(&(fixed_arg.sigl), -1-exponent) >= 0x80000000U )
	significand(&fixed_arg)++;	/* round up */
    }
  
  mul64(&significand(&fixed_arg), &significand(&fixed_arg),
	&significand(&arg_sqrd));
  mul64(&significand(&arg_sqrd), &significand(&arg_sqrd),
	&significand(&arg_to_4));
  
  /* will be a valid positive nr with expon = 0 */
  *(short *)&(accum.sign) = 0;
  accum.exp = 0;

  /* Do the basic fixed point polynomial evaluation */
  polynomial(&(accum.sigl), &(arg_to_4.sigl), lterms, HIPOWER-1);
  
  /* will be a valid positive nr with expon = 0 */
  *(short *)&(negaccum.sign) = 0;
  negaccum.exp = 0;
  
  /* Do the basic fixed point polynomial evaluation */
  polynomial(&(negaccum.sigl), &(arg_to_4.sigl), negterms, HIPOWER-1);
  mul64(&significand(&arg_sqrd), &significand(&negaccum),
	&significand(&negaccum));

  /* Subtract the mantissas */
  significand(&accum) -= significand(&negaccum);
  
  /* Convert to 64 bit signed-compatible */
  accum.exp = EXP_BIAS - 1 + accum.exp;

  reg_move(&accum, result);

  normalize(result);

  reg_mul(result, arg, result, FULL_PRECISION);
  reg_u_add(result, arg, result, FULL_PRECISION);
  
  if ( result->exp >= EXP_BIAS )
    {
      /* A small overflow may be possible... but an illegal result. */
      if (    (result->exp > EXP_BIAS) /* Larger or equal 2.0 */
	  || (result->sigl > 1)	  /* Larger than 1.0+msb */
	  ||	(result->sigh != 0x80000000) /* Much > 1.0 */
	  )
	{
#ifdef DEBUGGING
	  RE_ENTRANT_CHECK_OFF;
	  printk("\nEXP=%d, MS=%08x, LS=%08x\n", result->exp,
		 result->sigh, result->sigl);
	  RE_ENTRANT_CHECK_ON;
#endif DEBUGGING
	  EXCEPTION(EX_INTERNAL|0x103);
	}
      
#ifdef DEBUGGING
      RE_ENTRANT_CHECK_OFF;
      printk("\n***CORRECTING ILLEGAL RESULT*** in poly_sin() computation\n");
      printk("EXP=%d, MS=%08x, LS=%08x\n", result->exp,
	     result->sigh, result->sigl);
      RE_ENTRANT_CHECK_ON;
#endif DEBUGGING

      result->sigl = 0;	/* Truncate the result to 1.00 */
    }

}
