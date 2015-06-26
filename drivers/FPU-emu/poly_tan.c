/*---------------------------------------------------------------------------+
 |  poly_tan.c                                                               |
 |                                                                           |
 | Compute the tan of a FPU_REG, using a polynomial approximation.           |
 |                                                                           |
 | Copyright (C) 1992,1993                                                   |
 |                       W. Metzenthen, 22 Parker St, Ormond, Vic 3163,      |
 |                       Australia.  E-mail   billm@vaxc.cc.monash.edu.au    |
 |                                                                           |
 |                                                                           |
 +---------------------------------------------------------------------------*/

#include "exception.h"
#include "reg_constant.h"
#include "fpu_emu.h"
#include "control_w.h"


#define	HIPOWERop	3	/* odd poly, positive terms */
static unsigned short const	oddplterms[HIPOWERop][4] =
	{
	{ 0x846a, 0x42d1, 0xb544, 0x921f},
	{ 0x6fb2, 0x0215, 0x95c0, 0x099c},
	{ 0xfce6, 0x0cc8, 0x1c9a, 0x0000}
	};

#define	HIPOWERon	2	/* odd poly, negative terms */
static unsigned short const	oddnegterms[HIPOWERon][4] =
	{
	{ 0x6906, 0xe205, 0x25c8, 0x8838},
	{ 0x1dd7, 0x3fe3, 0x944e, 0x002c}
	};

#define	HIPOWERep	2	/* even poly, positive terms */
static unsigned short const	evenplterms[HIPOWERep][4] =
	{
	{ 0xdb8f, 0x3761, 0x1432, 0x2acf},
	{ 0x16eb, 0x13c1, 0x3099, 0x0003}
	};

#define	HIPOWERen	2	/* even poly, negative terms */
static unsigned short const	evennegterms[HIPOWERen][4] =
	{
	{ 0x3a7c, 0xe4c5, 0x7f87, 0x2945},
	{ 0x572b, 0x664c, 0xc543, 0x018c}
	};


/*--- poly_tan() ------------------------------------------------------------+
 |                                                                           |
 +---------------------------------------------------------------------------*/
void	poly_tan(FPU_REG const *arg, FPU_REG *result, int invert)
{
  short		exponent;
  FPU_REG       odd_poly, even_poly, pos_poly, neg_poly;
  FPU_REG       argSq;
  unsigned long long     arg_signif, argSqSq;
  

  exponent = arg->exp - EXP_BIAS;

#ifdef PARANOID
  if ( arg->sign != 0 )	/* Can't hack a number < 0.0 */
    { arith_invalid(result); return; }  /* Need a positive number */
#endif PARANOID

  arg_signif = significand(arg);
  if ( exponent < -1 )
    {
      /* shift the argument right by the required places */
      if ( shrx(&arg_signif, -1-exponent) >= 0x80000000U )
	arg_signif++;	/* round up */
    }

  mul64(&arg_signif, &arg_signif, &significand(&argSq));
  mul64(&significand(&argSq), &significand(&argSq), &argSqSq);

  /* will be a valid positive nr with expon = 0 */
  *(short *)&(pos_poly.sign) = 0;
  pos_poly.exp = EXP_BIAS;

  /* Do the basic fixed point polynomial evaluation */
  polynomial(&pos_poly.sigl, (unsigned *)&argSqSq, oddplterms, HIPOWERop-1);

  /* will be a valid positive nr with expon = 0 */
  *(short *)&(neg_poly.sign) = 0;
  neg_poly.exp = EXP_BIAS;

  /* Do the basic fixed point polynomial evaluation */
  polynomial(&neg_poly.sigl, (unsigned *)&argSqSq, oddnegterms, HIPOWERon-1);
  mul64(&significand(&argSq), &significand(&neg_poly),
	&significand(&neg_poly));

  /* Subtract the mantissas */
  significand(&pos_poly) -= significand(&neg_poly);

  /* Convert to 64 bit signed-compatible */
  pos_poly.exp -= 1;

  reg_move(&pos_poly, &odd_poly);
  normalize(&odd_poly);
  
  reg_mul(&odd_poly, arg, &odd_poly, FULL_PRECISION);
  /* Complete the odd polynomial. */
  reg_u_add(&odd_poly, arg, &odd_poly, FULL_PRECISION);

  /* will be a valid positive nr with expon = 0 */
  *(short *)&(pos_poly.sign) = 0;
  pos_poly.exp = EXP_BIAS;
  
  /* Do the basic fixed point polynomial evaluation */
  polynomial(&pos_poly.sigl, (unsigned *)&argSqSq, evenplterms, HIPOWERep-1);
  mul64(&significand(&argSq),
	&significand(&pos_poly), &significand(&pos_poly));
  
  /* will be a valid positive nr with expon = 0 */
  *(short *)&(neg_poly.sign) = 0;
  neg_poly.exp = EXP_BIAS;

  /* Do the basic fixed point polynomial evaluation */
  polynomial(&neg_poly.sigl, (unsigned *)&argSqSq, evennegterms, HIPOWERen-1);

  /* Subtract the mantissas */
  significand(&neg_poly) -= significand(&pos_poly);
  /* and multiply by argSq */

  /* Convert argSq to a valid reg number */
  *(short *)&(argSq.sign) = 0;
  argSq.exp = EXP_BIAS - 1;
  normalize(&argSq);

  /* Convert to 64 bit signed-compatible */
  neg_poly.exp -= 1;

  reg_move(&neg_poly, &even_poly);
  normalize(&even_poly);

  reg_mul(&even_poly, &argSq, &even_poly, FULL_PRECISION);
  reg_add(&even_poly, &argSq, &even_poly, FULL_PRECISION);
  /* Complete the even polynomial */
  reg_sub(&CONST_1, &even_poly, &even_poly, FULL_PRECISION);

  /* Now ready to copy the results */
  if ( invert )
    { reg_div(&even_poly, &odd_poly, result, FULL_PRECISION); }
  else
    { reg_div(&odd_poly, &even_poly, result, FULL_PRECISION); }

}
