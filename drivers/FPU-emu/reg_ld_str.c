/*---------------------------------------------------------------------------+
 |  reg_ld_str.c                                                             |
 |                                                                           |
 | All of the functions which transfer data between user memory and FPU_REGs.|
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
#include "reg_constant.h"
#include "fpu_emu.h"
#include "control_w.h"
#include "status_w.h"


#define EXTENDED_Ebias 0x3fff
#define EXTENDED_Emin (-0x3ffe)  /* smallest valid exponent */

#define DOUBLE_Emax 1023         /* largest valid exponent */
#define DOUBLE_Ebias 1023
#define DOUBLE_Emin (-1022)      /* smallest valid exponent */

#define SINGLE_Emax 127          /* largest valid exponent */
#define SINGLE_Ebias 127
#define SINGLE_Emin (-126)       /* smallest valid exponent */

static void write_to_extended(FPU_REG *rp, char *d);

FPU_REG FPU_loaded_data;


/* Get a long double from user memory */
int reg_load_extended(void)
{
  long double *s = (long double *)FPU_data_address;
  unsigned long sigl, sigh, exp;

  RE_ENTRANT_CHECK_OFF;
  FPU_verify_area(VERIFY_READ, s, 10);
  /* Use temporary variables here because FPU_loaded data is
     static and hence re-entrancy problems can arise */
  sigl = get_fs_long((unsigned long *) s);
  sigh = get_fs_long(1 + (unsigned long *) s);
  exp = get_fs_word(4 + (unsigned short *) s);
  RE_ENTRANT_CHECK_ON;

  FPU_loaded_data.tag = TW_Valid;   /* Default */
  FPU_loaded_data.sigl = sigl;
  FPU_loaded_data.sigh = sigh;
  if (exp & 0x8000)
    FPU_loaded_data.sign = SIGN_NEG;
  else
    FPU_loaded_data.sign = SIGN_POS;
  exp &= 0x7fff;
  FPU_loaded_data.exp = exp - EXTENDED_Ebias + EXP_BIAS;

  /* Assume that optimisation can keep sigl, sigh, and exp in
     registers, otherwise it would be more efficient to work
     with FPU_loaded_data (which is static) here. */
  if ( exp == 0 )
    {
      if ( !(sigh | sigl) )
	{
	  FPU_loaded_data.tag = TW_Zero;
	  return 0;
	}
      /* The number is a de-normal or pseudodenormal. */
      if (sigh & 0x80000000)
	{
	  /* Is a pseudodenormal. */
	  /* Convert it for internal use. */
	  /* This is non-80486 behaviour because the number
	     loses its 'denormal' identity. */
	  FPU_loaded_data.exp++;
	  return 1;
	}
      else
	{
	  /* Is a denormal. */
	  /* Convert it for internal use. */
	  FPU_loaded_data.exp++;
	  normalize_nuo(&FPU_loaded_data);
	  return 0;
	}
    }
  else if ( exp == 0x7fff )
    {
      if ( !((sigh ^ 0x80000000) | sigl) )
	{
	  /* Matches the bit pattern for Infinity. */
	  FPU_loaded_data.exp = EXP_Infinity;
	  FPU_loaded_data.tag = TW_Infinity;
	  return 0;
	}

      FPU_loaded_data.exp = EXP_NaN;
      FPU_loaded_data.tag = TW_NaN;
      if ( !(sigh & 0x80000000) )
	{
	  /* NaNs have the ms bit set to 1. */
	  /* This is therefore an Unsupported NaN data type. */
	  /* This is non 80486 behaviour */
	  /* This should generate an Invalid Operand exception
	     later, so we convert it to a SNaN */
	  FPU_loaded_data.sigh = 0x80000000;
	  FPU_loaded_data.sigl = 0x00000001;
	  FPU_loaded_data.sign = SIGN_NEG;
	  return 1;
	}
      return 0;
    }

  if ( !(sigh & 0x80000000) )
    {
      /* Unsupported data type. */
      /* Valid numbers have the ms bit set to 1. */
      /* Unnormal. */
      /* Convert it for internal use. */
      /* This is non-80486 behaviour */
      /* This should generate an Invalid Operand exception
	 later, so we convert it to a SNaN */
      FPU_loaded_data.sigh = 0x80000000;
      FPU_loaded_data.sigl = 0x00000001;
      FPU_loaded_data.sign = SIGN_NEG;
      FPU_loaded_data.exp = EXP_NaN;
      FPU_loaded_data.tag = TW_NaN;
      return 1;
    }
  return 0;
}


/* Get a double from user memory */
int reg_load_double(void)
{
  double *dfloat = (double *)FPU_data_address;
  int exp;
  unsigned m64, l64;

  RE_ENTRANT_CHECK_OFF;
  FPU_verify_area(VERIFY_READ, dfloat, 8);
  m64 = get_fs_long(1 + (unsigned long *) dfloat);
  l64 = get_fs_long((unsigned long *) dfloat);
  RE_ENTRANT_CHECK_ON;

  if (m64 & 0x80000000)
    FPU_loaded_data.sign = SIGN_NEG;
  else
    FPU_loaded_data.sign = SIGN_POS;
  exp = ((m64 & 0x7ff00000) >> 20) - DOUBLE_Ebias;
  m64 &= 0xfffff;
  if (exp > DOUBLE_Emax)
    {
      /* Infinity or NaN */
      if ((m64 == 0) && (l64 == 0))
	{
	  /* +- infinity */
	  FPU_loaded_data.sigh = 0x80000000;
	  FPU_loaded_data.sigl = 0x00000000;
	  FPU_loaded_data.exp = EXP_Infinity;
	  FPU_loaded_data.tag = TW_Infinity;
	  return 0;
	}
      else
	{
	  /* Must be a signaling or quiet NaN */
	  FPU_loaded_data.exp = EXP_NaN;
	  FPU_loaded_data.tag = TW_NaN;
	  FPU_loaded_data.sigh = (m64 << 11) | 0x80000000;
	  FPU_loaded_data.sigh |= l64 >> 21;
	  FPU_loaded_data.sigl = l64 << 11;
	  return 0; /* The calling function must look for NaNs */
	}
    }
  else if ( exp < DOUBLE_Emin )
    {
      /* Zero or de-normal */
      if ((m64 == 0) && (l64 == 0))
	{
	  /* Zero */
	  int c = FPU_loaded_data.sign;
	  reg_move(&CONST_Z, &FPU_loaded_data);
	  FPU_loaded_data.sign = c;
	  return 0;
	}
      else
	{
	  /* De-normal */
	  FPU_loaded_data.exp = DOUBLE_Emin + EXP_BIAS;
	  FPU_loaded_data.tag = TW_Valid;
	  FPU_loaded_data.sigh = m64 << 11;
	  FPU_loaded_data.sigh |= l64 >> 21;
	  FPU_loaded_data.sigl = l64 << 11;
	  normalize_nuo(&FPU_loaded_data);
	  return denormal_operand();
	}
    }
  else
    {
      FPU_loaded_data.exp = exp + EXP_BIAS;
      FPU_loaded_data.tag = TW_Valid;
      FPU_loaded_data.sigh = (m64 << 11) | 0x80000000;
      FPU_loaded_data.sigh |= l64 >> 21;
      FPU_loaded_data.sigl = l64 << 11;

      return 0;
    }
}


/* Get a float from user memory */
int reg_load_single(void)
{
  float *single = (float *)FPU_data_address;
  unsigned m32;
  int exp;

  RE_ENTRANT_CHECK_OFF;
  FPU_verify_area(VERIFY_READ, single, 4);
  m32 = get_fs_long((unsigned long *) single);
  RE_ENTRANT_CHECK_ON;

  if (m32 & 0x80000000)
    FPU_loaded_data.sign = SIGN_NEG;
  else
    FPU_loaded_data.sign = SIGN_POS;
  if (!(m32 & 0x7fffffff))
    {
      /* Zero */
      int c = FPU_loaded_data.sign;
      reg_move(&CONST_Z, &FPU_loaded_data);
      FPU_loaded_data.sign = c;
      return 0;
    }
  exp = ((m32 & 0x7f800000) >> 23) - SINGLE_Ebias;
  m32 = (m32 & 0x7fffff) << 8;
  if ( exp < SINGLE_Emin )
    {
      /* De-normals */
      FPU_loaded_data.exp = SINGLE_Emin + EXP_BIAS;
      FPU_loaded_data.tag = TW_Valid;
      FPU_loaded_data.sigh = m32;
      FPU_loaded_data.sigl = 0;
      normalize_nuo(&FPU_loaded_data);
      return denormal_operand();
    }
  else if ( exp > SINGLE_Emax )
    {
    /* Infinity or NaN */
      if ( m32 == 0 )
	{
	  /* +- infinity */
	  FPU_loaded_data.sigh = 0x80000000;
	  FPU_loaded_data.sigl = 0x00000000;
	  FPU_loaded_data.exp = EXP_Infinity;
	  FPU_loaded_data.tag = TW_Infinity;
	  return 0;
	}
      else
	{
	  /* Must be a signaling or quiet NaN */
	  FPU_loaded_data.exp = EXP_NaN;
	  FPU_loaded_data.tag = TW_NaN;
	  FPU_loaded_data.sigh = m32 | 0x80000000;
	  FPU_loaded_data.sigl = 0;
	  return 0; /* The calling function must look for NaNs */
	}
    }
  else
    {
      FPU_loaded_data.exp = exp + EXP_BIAS;
      FPU_loaded_data.sigh = m32 | 0x80000000;
      FPU_loaded_data.sigl = 0;
      FPU_loaded_data.tag = TW_Valid;
      return 0;
    }
}


/* Get a long long from user memory */
void reg_load_int64(void)
{
  long long *_s = (long long *)FPU_data_address;
  int e;
  long long s;

  RE_ENTRANT_CHECK_OFF;
  FPU_verify_area(VERIFY_READ, _s, 8);
  ((unsigned long *)&s)[0] = get_fs_long((unsigned long *) _s);
  ((unsigned long *)&s)[1] = get_fs_long(1 + (unsigned long *) _s);
  RE_ENTRANT_CHECK_ON;

  if (s == 0)
    { reg_move(&CONST_Z, &FPU_loaded_data); return; }

  if (s > 0)
    FPU_loaded_data.sign = SIGN_POS;
  else
  {
    s = -s;
    FPU_loaded_data.sign = SIGN_NEG;
  }

  e = EXP_BIAS + 63;
  significand(&FPU_loaded_data) = s;
  FPU_loaded_data.exp = e;
  FPU_loaded_data.tag = TW_Valid;
  normalize_nuo(&FPU_loaded_data);
}


/* Get a long from user memory */
void reg_load_int32(void)
{
  long *_s = (long *)FPU_data_address;
  long s;
  int e;

  RE_ENTRANT_CHECK_OFF;
  FPU_verify_area(VERIFY_READ, _s, 4);
  s = (long)get_fs_long((unsigned long *) _s);
  RE_ENTRANT_CHECK_ON;

  if (s == 0)
    { reg_move(&CONST_Z, &FPU_loaded_data); return; }

  if (s > 0)
    FPU_loaded_data.sign = SIGN_POS;
  else
  {
    s = -s;
    FPU_loaded_data.sign = SIGN_NEG;
  }

  e = EXP_BIAS + 31;
  FPU_loaded_data.sigh = s;
  FPU_loaded_data.sigl = 0;
  FPU_loaded_data.exp = e;
  FPU_loaded_data.tag = TW_Valid;
  normalize_nuo(&FPU_loaded_data);
}


/* Get a short from user memory */
void reg_load_int16(void)
{
  short *_s = (short *)FPU_data_address;
  int s, e;

  RE_ENTRANT_CHECK_OFF;
  FPU_verify_area(VERIFY_READ, _s, 2);
  /* Cast as short to get the sign extended. */
  s = (short)get_fs_word((unsigned short *) _s);
  RE_ENTRANT_CHECK_ON;

  if (s == 0)
    { reg_move(&CONST_Z, &FPU_loaded_data); return; }

  if (s > 0)
    FPU_loaded_data.sign = SIGN_POS;
  else
  {
    s = -s;
    FPU_loaded_data.sign = SIGN_NEG;
  }

  e = EXP_BIAS + 15;
  FPU_loaded_data.sigh = s << 16;

  FPU_loaded_data.sigl = 0;
  FPU_loaded_data.exp = e;
  FPU_loaded_data.tag = TW_Valid;
  normalize_nuo(&FPU_loaded_data);
}


/* Get a packed bcd array from user memory */
void reg_load_bcd(void)
{
  char *s = (char *)FPU_data_address;
  int pos;
  unsigned char bcd;
  long long l=0;

  RE_ENTRANT_CHECK_OFF;
  FPU_verify_area(VERIFY_READ, s, 10);
  RE_ENTRANT_CHECK_ON;
  for ( pos = 8; pos >= 0; pos--)
    {
      l *= 10;
      RE_ENTRANT_CHECK_OFF;
      bcd = (unsigned char)get_fs_byte((unsigned char *) s+pos);
      RE_ENTRANT_CHECK_ON;
      l += bcd >> 4;
      l *= 10;
      l += bcd & 0x0f;
    }
  
  /* Finish all access to user memory before putting stuff into
     the static FPU_loaded_data */
  RE_ENTRANT_CHECK_OFF;
  FPU_loaded_data.sign =
    ((unsigned char)get_fs_byte((unsigned char *) s+9)) & 0x80 ?
      SIGN_NEG : SIGN_POS;
  RE_ENTRANT_CHECK_ON;

  if (l == 0)
    {
      char sign = FPU_loaded_data.sign;
      reg_move(&CONST_Z, &FPU_loaded_data);
      FPU_loaded_data.sign = sign;
    }
  else
    {
      significand(&FPU_loaded_data) = l;
      FPU_loaded_data.exp = EXP_BIAS + 63;
      FPU_loaded_data.tag = TW_Valid;
      normalize_nuo(&FPU_loaded_data);
    }
}

/*===========================================================================*/

/* Put a long double into user memory */
int reg_store_extended(void)
{
  /*
    The only exception raised by an attempt to store to an
    extended format is the Invalid Stack exception, i.e.
    attempting to store from an empty register.
   */
  long double *d = (long double *)FPU_data_address;

  if ( FPU_st0_tag != TW_Empty )
    {
      RE_ENTRANT_CHECK_OFF;
      FPU_verify_area(VERIFY_WRITE, d, 10);
      RE_ENTRANT_CHECK_ON;
      write_to_extended(FPU_st0_ptr, (char *) FPU_data_address);
      return 1;
    }

  /* Empty register (stack underflow) */
  EXCEPTION(EX_StackUnder);
  if ( control_word & CW_Invalid )
    {
      /* The masked response */
      /* Put out the QNaN indefinite */
      RE_ENTRANT_CHECK_OFF;
      FPU_verify_area(VERIFY_WRITE,d,10);
      put_fs_long(0, (unsigned long *) d);
      put_fs_long(0xc0000000, 1 + (unsigned long *) d);
      put_fs_word(0xffff, 4 + (short *) d);
      RE_ENTRANT_CHECK_ON;
      return 1;
    }
  else
    return 0;

}


/* Put a double into user memory */
int reg_store_double(void)
{
  double *dfloat = (double *)FPU_data_address;
  unsigned long l[2];
  unsigned long increment = 0;	/* avoid gcc warnings */

  if (FPU_st0_tag == TW_Valid)
    {
      int exp;
      FPU_REG tmp;

      reg_move(FPU_st0_ptr, &tmp);
      exp = tmp.exp - EXP_BIAS;

      if ( exp < DOUBLE_Emin )     /* It may be a denormal */
	{
	  int precision_loss;

	  /* A denormal will always underflow. */
#ifndef PECULIAR_486
	  /* An 80486 is supposed to be able to generate
	     a denormal exception here, but... */
	  if ( FPU_st0_ptr->exp <= EXP_UNDER )
	    {
	      /* Underflow has priority. */
	      if ( control_word & CW_Underflow )
		denormal_operand();
	    }
#endif PECULIAR_486

	  tmp.exp += -DOUBLE_Emin + 52;  /* largest exp to be 51 */

	  if ( (precision_loss = round_to_int(&tmp)) )
	    {
#ifdef PECULIAR_486
	      /* Did it round to a non-denormal ? */
	      /* This behaviour might be regarded as peculiar, it appears
		 that the 80486 rounds to the dest precision, then
		 converts to decide underflow. */
	      if ( !((tmp.sigh == 0x00100000) && (tmp.sigl == 0) &&
		  (FPU_st0_ptr->sigl & 0x000007ff)) )
#endif PECULIAR_486
		{
		  EXCEPTION(EX_Underflow);
		  /* This is a special case: see sec 16.2.5.1 of
		     the 80486 book */
		  if ( !(control_word & CW_Underflow) )
		    return 0;
		}
	      EXCEPTION(precision_loss);
	      if ( !(control_word & CW_Precision) )
		return 0;
	    }
	  l[0] = tmp.sigl;
	  l[1] = tmp.sigh;
	}
      else
	{
	  if ( tmp.sigl & 0x000007ff )
	    {
	      switch (control_word & CW_RC)
		{
		case RC_RND:
		  /* Rounding can get a little messy.. */
		  increment = ((tmp.sigl & 0x7ff) > 0x400) |  /* nearest */
		    ((tmp.sigl & 0xc00) == 0xc00);            /* odd -> even */
		  break;
		case RC_DOWN:   /* towards -infinity */
		  increment = (tmp.sign == SIGN_POS) ? 0 : tmp.sigl & 0x7ff;
		  break;
		case RC_UP:     /* towards +infinity */
		  increment = (tmp.sign == SIGN_POS) ? tmp.sigl & 0x7ff : 0;
		  break;
		case RC_CHOP:
		  increment = 0;
		  break;
		}
	  
	      /* Truncate the mantissa */
	      tmp.sigl &= 0xfffff800;
	  
	      if ( increment )
		{
		  set_precision_flag_up();

		  if ( tmp.sigl >= 0xfffff800 )
		    {
		      /* the sigl part overflows */
		      if ( tmp.sigh == 0xffffffff )
			{
			  /* The sigh part overflows */
			  tmp.sigh = 0x80000000;
			  exp++;
			  if (exp >= EXP_OVER)
			    goto overflow;
			}
		      else
			{
			  tmp.sigh ++;
			}
		      tmp.sigl = 0x00000000;
		    }
		  else
		    {
		      /* We only need to increment sigl */
		      tmp.sigl += 0x00000800;
		    }
		}
	      else
		set_precision_flag_down();
	    }
	  
	  l[0] = (tmp.sigl >> 11) | (tmp.sigh << 21);
	  l[1] = ((tmp.sigh >> 11) & 0xfffff);

	  if ( exp > DOUBLE_Emax )
	    {
	    overflow:
	      EXCEPTION(EX_Overflow);
	      if ( !(control_word & CW_Overflow) )
		return 0;
	      set_precision_flag_up();
	      if ( !(control_word & CW_Precision) )
		return 0;

	      /* This is a special case: see sec 16.2.5.1 of the 80486 book */
	      /* Overflow to infinity */
	      l[0] = 0x00000000;	/* Set to */
	      l[1] = 0x7ff00000;	/* + INF */
	    }
	  else
	    {
	      /* Add the exponent */
	      l[1] |= (((exp+DOUBLE_Ebias) & 0x7ff) << 20);
	    }
	}
    }
  else if (FPU_st0_tag == TW_Zero)
    {
      /* Number is zero */
      l[0] = 0;
      l[1] = 0;
    }
  else if (FPU_st0_tag == TW_Infinity)
    {
      l[0] = 0;
      l[1] = 0x7ff00000;
    }
  else if (FPU_st0_tag == TW_NaN)
    {
      /* See if we can get a valid NaN from the FPU_REG */
      l[0] = (FPU_st0_ptr->sigl >> 11) | (FPU_st0_ptr->sigh << 21);
      l[1] = ((FPU_st0_ptr->sigh >> 11) & 0xfffff);
      if ( !(FPU_st0_ptr->sigh & 0x40000000) )
	{
	  /* It is a signalling NaN */
	  EXCEPTION(EX_Invalid);
	  if ( !(control_word & CW_Invalid) )
	    return 0;
	  l[1] |= (0x40000000 >> 11);
	}
      l[1] |= 0x7ff00000;
    }
  else if ( FPU_st0_tag == TW_Empty )
    {
      /* Empty register (stack underflow) */
      EXCEPTION(EX_StackUnder);
      if ( control_word & CW_Invalid )
	{
	  /* The masked response */
	  /* Put out the QNaN indefinite */
	  RE_ENTRANT_CHECK_OFF;
	  FPU_verify_area(VERIFY_WRITE,(void *)dfloat,8);
	  put_fs_long(0, (unsigned long *) dfloat);
	  put_fs_long(0xfff80000, 1 + (unsigned long *) dfloat);
	  RE_ENTRANT_CHECK_ON;
	  return 1;
	}
      else
	return 0;
    }
  if (FPU_st0_ptr->sign)
    l[1] |= 0x80000000;

  RE_ENTRANT_CHECK_OFF;
  FPU_verify_area(VERIFY_WRITE,(void *)dfloat,8);
  put_fs_long(l[0], (unsigned long *)dfloat);
  put_fs_long(l[1], 1 + (unsigned long *)dfloat);
  RE_ENTRANT_CHECK_ON;

  return 1;
}


/* Put a float into user memory */
int reg_store_single(void)
{
  float *single = (float *)FPU_data_address;
  long templ;
  unsigned long increment = 0;     	/* avoid gcc warnings */

  if (FPU_st0_tag == TW_Valid)
    {
      int exp;
      FPU_REG tmp;

      reg_move(FPU_st0_ptr, &tmp);
      exp = tmp.exp - EXP_BIAS;

      if ( exp < SINGLE_Emin )
	{
	  int precision_loss;

	  /* A denormal will always underflow. */
#ifndef PECULIAR_486
	  /* An 80486 is supposed to be able to generate
	     a denormal exception here, but... */
	  if ( FPU_st0_ptr->exp <= EXP_UNDER )
	    {
	      /* Underflow has priority. */
	      if ( control_word & CW_Underflow )
		denormal_operand();
	    }
#endif PECULIAR_486

	  tmp.exp += -SINGLE_Emin + 23;  /* largest exp to be 22 */

	  if ( (precision_loss = round_to_int(&tmp)) )
	    {
#ifdef PECULIAR_486
	      /* Did it round to a non-denormal ? */
	      /* This behaviour might be regarded as peculiar, it appears
		 that the 80486 rounds to the dest precision, then
		 converts to decide underflow. */
	      if ( !((tmp.sigl == 0x00800000) &&
		  ((FPU_st0_ptr->sigh & 0x000000ff) || FPU_st0_ptr->sigl)) )
#endif PECULIAR_486
		{
		  EXCEPTION(EX_Underflow);
		  /* This is a special case: see sec 16.2.5.1 of
		     the 80486 book */
		  if ( !(control_word & EX_Underflow) )
		    return 0;
		}
	      EXCEPTION(precision_loss);
	      if ( !(control_word & EX_Precision) )
		return 0;
	    }
	  templ = tmp.sigl;
	}
      else
	{
	  if ( tmp.sigl | (tmp.sigh & 0x000000ff) )
	    {
	      unsigned long sigh = tmp.sigh;
	      unsigned long sigl = tmp.sigl;
	      
	      switch (control_word & CW_RC)
		{
		case RC_RND:
		  increment = ((sigh & 0xff) > 0x80)       /* more than half */
		    || (((sigh & 0xff) == 0x80) && sigl)   /* more than half */
		      || ((sigh & 0x180) == 0x180);        /* round to even */
		  break;
		case RC_DOWN:   /* towards -infinity */
		  increment = (tmp.sign == SIGN_POS)
		              ? 0 : (sigl | (sigh & 0xff));
		  break;
		case RC_UP:     /* towards +infinity */
		  increment = (tmp.sign == SIGN_POS)
		              ? (sigl | (sigh & 0xff)) : 0;
		  break;
		case RC_CHOP:
		  increment = 0;
		  break;
		}
	  
	      /* Truncate part of the mantissa */
	      tmp.sigl = 0;
	  
	      if (increment)
		{
		  set_precision_flag_up();

		  if ( sigh >= 0xffffff00 )
		    {
		      /* The sigh part overflows */
		      tmp.sigh = 0x80000000;
		      exp++;
		      if ( exp >= EXP_OVER )
			goto overflow;
		    }
		  else
		    {
		      tmp.sigh &= 0xffffff00;
		      tmp.sigh += 0x100;
		    }
		}
	      else
		{
		  set_precision_flag_down();
		  tmp.sigh &= 0xffffff00;  /* Finish the truncation */
		}
	    }

	  templ = (tmp.sigh >> 8) & 0x007fffff;

	  if ( exp > SINGLE_Emax )
	    {
	    overflow:
	      EXCEPTION(EX_Overflow);
	      if ( !(control_word & CW_Overflow) )
		return 0;
	      set_precision_flag_up();
	      if ( !(control_word & CW_Precision) )
		return 0;

	      /* This is a special case: see sec 16.2.5.1 of the 80486 book. */
	      /* Masked respose is overflow to infinity. */
	      templ = 0x7f800000;
	    }
	  else
	    templ |= ((exp+SINGLE_Ebias) & 0xff) << 23;
	}
    }
  else if (FPU_st0_tag == TW_Zero)
    {
      templ = 0;
    }
  else if (FPU_st0_tag == TW_Infinity)
    {
      templ = 0x7f800000;
    }
  else if (FPU_st0_tag == TW_NaN)
    {
      /* See if we can get a valid NaN from the FPU_REG */
      templ = FPU_st0_ptr->sigh >> 8;
      if ( !(FPU_st0_ptr->sigh & 0x40000000) )
	{
	  /* It is a signalling NaN */
	  EXCEPTION(EX_Invalid);
	  if ( !(control_word & CW_Invalid) )
	    return 0;
	  templ |= (0x40000000 >> 8);
	}
      templ |= 0x7f800000;
    }
  else if ( FPU_st0_tag == TW_Empty )
    {
      /* Empty register (stack underflow) */
      EXCEPTION(EX_StackUnder);
      if ( control_word & EX_Invalid )
	{
	  /* The masked response */
	  /* Put out the QNaN indefinite */
	  RE_ENTRANT_CHECK_OFF;
	  FPU_verify_area(VERIFY_WRITE,(void *)single,4);
	  put_fs_long(0xffc00000, (unsigned long *) single);
	  RE_ENTRANT_CHECK_ON;
	  return 1;
	}
      else
	return 0;
    }
#ifdef PARANOID
  else
    {
      EXCEPTION(EX_INTERNAL|0x106);
      return 0;
    }
#endif
  if (FPU_st0_ptr->sign)
    templ |= 0x80000000;

  RE_ENTRANT_CHECK_OFF;
  FPU_verify_area(VERIFY_WRITE,(void *)single,4);
  put_fs_long(templ,(unsigned long *) single);
  RE_ENTRANT_CHECK_ON;

  return 1;
}


/* Put a long long into user memory */
int reg_store_int64(void)
{
  long long *d = (long long *)FPU_data_address;
  FPU_REG t;
  long long tll;
  int precision_loss;

  if ( FPU_st0_tag == TW_Empty )
    {
      /* Empty register (stack underflow) */
      EXCEPTION(EX_StackUnder);
      goto invalid_operand;
    }
  else if ( (FPU_st0_tag == TW_Infinity) ||
	   (FPU_st0_tag == TW_NaN) )
    {
      EXCEPTION(EX_Invalid);
      goto invalid_operand;
    }

  reg_move(FPU_st0_ptr, &t);
  precision_loss = round_to_int(&t);
  ((long *)&tll)[0] = t.sigl;
  ((long *)&tll)[1] = t.sigh;
  if ( (precision_loss == 1) ||
      ((t.sigh & 0x80000000) &&
       !((t.sigh == 0x80000000) && (t.sigl == 0) &&
	 (t.sign == SIGN_NEG))) )
    {
      EXCEPTION(EX_Invalid);
      /* This is a special case: see sec 16.2.5.1 of the 80486 book */
    invalid_operand:
      if ( control_word & EX_Invalid )
	{
	  /* Produce something like QNaN "indefinite" */
	  tll = 0x8000000000000000LL;
	}
      else
	return 0;
    }
  else
    {
      if ( precision_loss )
	set_precision_flag(precision_loss);
      if ( t.sign )
	tll = - tll;
    }

  RE_ENTRANT_CHECK_OFF;
  FPU_verify_area(VERIFY_WRITE,(void *)d,8);
  put_fs_long(((long *)&tll)[0],(unsigned long *) d);
  put_fs_long(((long *)&tll)[1],1 + (unsigned long *) d);
  RE_ENTRANT_CHECK_ON;

  return 1;
}


/* Put a long into user memory */
int reg_store_int32(void)
{
  long *d = (long *)FPU_data_address;
  FPU_REG t;
  int precision_loss;

  if ( FPU_st0_tag == TW_Empty )
    {
      /* Empty register (stack underflow) */
      EXCEPTION(EX_StackUnder);
      goto invalid_operand;
    }
  else if ( (FPU_st0_tag == TW_Infinity) ||
	   (FPU_st0_tag == TW_NaN) )
    {
      EXCEPTION(EX_Invalid);
      goto invalid_operand;
    }

  reg_move(FPU_st0_ptr, &t);
  precision_loss = round_to_int(&t);
  if (t.sigh ||
      ((t.sigl & 0x80000000) &&
       !((t.sigl == 0x80000000) && (t.sign == SIGN_NEG))) )
    {
      EXCEPTION(EX_Invalid);
      /* This is a special case: see sec 16.2.5.1 of the 80486 book */
    invalid_operand:
      if ( control_word & EX_Invalid )
	{
	  /* Produce something like QNaN "indefinite" */
	  t.sigl = 0x80000000;
	}
      else
	return 0;
    }
  else
    {
      if ( precision_loss )
	set_precision_flag(precision_loss);
      if ( t.sign )
	t.sigl = -(long)t.sigl;
    }

  RE_ENTRANT_CHECK_OFF;
  FPU_verify_area(VERIFY_WRITE,d,4);
  put_fs_long(t.sigl, (unsigned long *) d);
  RE_ENTRANT_CHECK_ON;

  return 1;
}


/* Put a short into user memory */
int reg_store_int16(void)
{
  short *d = (short *)FPU_data_address;
  FPU_REG t;
  int precision_loss;

  if ( FPU_st0_tag == TW_Empty )
    {
      /* Empty register (stack underflow) */
      EXCEPTION(EX_StackUnder);
      goto invalid_operand;
    }
  else if ( (FPU_st0_tag == TW_Infinity) ||
	   (FPU_st0_tag == TW_NaN) )
    {
      EXCEPTION(EX_Invalid);
      goto invalid_operand;
    }

  reg_move(FPU_st0_ptr, &t);
  precision_loss = round_to_int(&t);
  if (t.sigh ||
      ((t.sigl & 0xffff8000) &&
       !((t.sigl == 0x8000) && (t.sign == SIGN_NEG))) )
    {
      EXCEPTION(EX_Invalid);
      /* This is a special case: see sec 16.2.5.1 of the 80486 book */
    invalid_operand:
      if ( control_word & EX_Invalid )
	{
	  /* Produce something like QNaN "indefinite" */
	  t.sigl = 0x8000;
	}
      else
	return 0;
    }
  else
    {
      if ( precision_loss )
	set_precision_flag(precision_loss);
      if ( t.sign )
	t.sigl = -t.sigl;
    }

  RE_ENTRANT_CHECK_OFF;
  FPU_verify_area(VERIFY_WRITE,d,2);
  put_fs_word((short)t.sigl,(short *) d);
  RE_ENTRANT_CHECK_ON;

  return 1;
}


/* Put a packed bcd array into user memory */
int reg_store_bcd(void)
{
  char *d = (char *)FPU_data_address;
  FPU_REG t;
  unsigned long long ll;
  unsigned char b;
  int i, precision_loss;
  unsigned char sign = (FPU_st0_ptr->sign == SIGN_NEG) ? 0x80 : 0;

  if ( FPU_st0_tag == TW_Empty )
    {
      /* Empty register (stack underflow) */
      EXCEPTION(EX_StackUnder);
      goto invalid_operand;
    }

  reg_move(FPU_st0_ptr, &t);
  precision_loss = round_to_int(&t);
  ll = significand(&t);

  /* Check for overflow, by comparing with 999999999999999999 decimal. */
  if ( (t.sigh > 0x0de0b6b3) ||
      ((t.sigh == 0x0de0b6b3) && (t.sigl > 0xa763ffff)) )
    {
      EXCEPTION(EX_Invalid);
      /* This is a special case: see sec 16.2.5.1 of the 80486 book */
    invalid_operand:
      if ( control_word & CW_Invalid )
	{
	  /* Produce the QNaN "indefinite" */
	  RE_ENTRANT_CHECK_OFF;
	  FPU_verify_area(VERIFY_WRITE,d,10);
	  for ( i = 0; i < 7; i++)
	    put_fs_byte(0, (unsigned char *) d+i); /* These bytes "undefined" */
	  put_fs_byte(0xc0, (unsigned char *) d+7); /* This byte "undefined" */
	  put_fs_byte(0xff, (unsigned char *) d+8);
	  put_fs_byte(0xff, (unsigned char *) d+9);
	  RE_ENTRANT_CHECK_ON;
	  return 1;
	}
      else
	return 0;
    }
  else if ( precision_loss )
    {
      /* Precision loss doesn't stop the data transfer */
      set_precision_flag(precision_loss);
    }

  RE_ENTRANT_CHECK_OFF;
  FPU_verify_area(VERIFY_WRITE,d,10);
  RE_ENTRANT_CHECK_ON;
  for ( i = 0; i < 9; i++)
    {
      b = div_small(&ll, 10);
      b |= (div_small(&ll, 10)) << 4;
      RE_ENTRANT_CHECK_OFF;
      put_fs_byte(b,(unsigned char *) d+i);
      RE_ENTRANT_CHECK_ON;
    }
  RE_ENTRANT_CHECK_OFF;
  put_fs_byte(sign,(unsigned char *) d+9);
  RE_ENTRANT_CHECK_ON;

  return 1;
}

/*===========================================================================*/

/* r gets mangled such that sig is int, sign: 
   it is NOT normalized */
/* The return value (in eax) is zero if the result is exact,
   if bits are changed due to rounding, truncation, etc, then
   a non-zero value is returned */
/* Overflow is signalled by a non-zero return value (in eax).
   In the case of overflow, the returned significand always has the
   the largest possible value */
int round_to_int(FPU_REG *r)
{
  char     very_big;
  unsigned eax;

  if (r->tag == TW_Zero)
    {
      /* Make sure that zero is returned */
      significand(r) = 0;
      return 0;        /* o.k. */
    }
  
  if (r->exp > EXP_BIAS + 63)
    {
      r->sigl = r->sigh = ~0;      /* The largest representable number */
      return 1;        /* overflow */
    }

  eax = shrxs(&r->sigl, EXP_BIAS + 63 - r->exp);
  very_big = !(~(r->sigh) | ~(r->sigl));  /* test for 0xfff...fff */
#define	half_or_more	(eax & 0x80000000)
#define	frac_part	(eax)
#define more_than_half  ((eax & 0x80000001) == 0x80000001)
  switch (control_word & CW_RC)
    {
    case RC_RND:
      if ( more_than_half               	/* nearest */
	  || (half_or_more && (r->sigl & 1)) )	/* odd -> even */
	{
	  if ( very_big ) return 1;        /* overflow */
	  significand(r) ++;
	  return PRECISION_LOST_UP;
	}
      break;
    case RC_DOWN:
      if (frac_part && r->sign)
	{
	  if ( very_big ) return 1;        /* overflow */
	  significand(r) ++;
	  return PRECISION_LOST_UP;
	}
      break;
    case RC_UP:
      if (frac_part && !r->sign)
	{
	  if ( very_big ) return 1;        /* overflow */
	  significand(r) ++;
	  return PRECISION_LOST_UP;
	}
      break;
    case RC_CHOP:
      break;
    }

  return eax ? PRECISION_LOST_DOWN : 0;

}

/*===========================================================================*/

char *fldenv(fpu_addr_modes addr_modes)
{
  char *s = (char *)FPU_data_address;
  unsigned short tag_word = 0;
  unsigned char tag;
  int i;

  if ( addr_modes.vm86
      || (addr_modes.override.operand_size == OP_SIZE_PREFIX) )
    {
      RE_ENTRANT_CHECK_OFF;
      FPU_verify_area(VERIFY_READ, s, 0x0e);
      control_word = get_fs_word((unsigned short *) s);
      partial_status = get_fs_word((unsigned short *) (s+2));
      tag_word = get_fs_word((unsigned short *) (s+4));
      ip_offset = get_fs_word((unsigned short *) (s+6));
      cs_selector = get_fs_word((unsigned short *) (s+8));
      data_operand_offset = get_fs_word((unsigned short *) (s+0x0a));
      operand_selector = get_fs_word((unsigned short *) (s+0x0c));
      RE_ENTRANT_CHECK_ON;
      s += 0x0e;
      if ( addr_modes.vm86 )
	{
	  ip_offset += (cs_selector & 0xf000) << 4;
	  data_operand_offset += (operand_selector & 0xf000) << 4;
	}
    }
  else
    {
      RE_ENTRANT_CHECK_OFF;
      FPU_verify_area(VERIFY_READ, s, 0x1c);
      control_word = get_fs_word((unsigned short *) s);
      partial_status = get_fs_word((unsigned short *) (s+4));
      tag_word = get_fs_word((unsigned short *) (s+8));
      ip_offset = get_fs_long((unsigned long *) (s+0x0c));
      cs_selector = get_fs_long((unsigned long *) (s+0x10));
      data_operand_offset = get_fs_long((unsigned long *) (s+0x14));
      operand_selector = get_fs_long((unsigned long *) (s+0x18));
      RE_ENTRANT_CHECK_ON;
      s += 0x1c;
    }

#ifdef PECULIAR_486
  control_word &= ~0xe080;
#endif PECULIAR_486

  top = (partial_status >> SW_Top_Shift) & 7;

  if ( partial_status & ~control_word & CW_Exceptions )
    partial_status |= (SW_Summary | SW_Backward);
  else
    partial_status &= ~(SW_Summary | SW_Backward);

  for ( i = 0; i < 8; i++ )
    {
      tag = tag_word & 3;
      tag_word >>= 2;

      if ( tag == 3 )
	/* New tag is empty.  Accept it */
	regs[i].tag = TW_Empty;
      else if ( regs[i].tag == TW_Empty )
	{
	  /* Old tag is empty and new tag is not empty.  New tag is determined
	     by old reg contents */
	  if ( regs[i].exp == EXP_BIAS - EXTENDED_Ebias )
	    {
	      if ( !(regs[i].sigl | regs[i].sigh) )
		regs[i].tag = TW_Zero;
	      else
		regs[i].tag = TW_Valid;
	    }
	  else if ( regs[i].exp == 0x7fff + EXP_BIAS - EXTENDED_Ebias )
	    {
	      if ( !((regs[i].sigh & ~0x80000000) | regs[i].sigl) )
		regs[i].tag = TW_Infinity;
	      else
		regs[i].tag = TW_NaN;
	    }
	  else
	    regs[i].tag = TW_Valid;
  	}
      /* Else old tag is not empty and new tag is not empty.  Old tag
	 remains correct */
    }

  /* Ensure that the values just loaded are not changed by
     fix-up operations. */
  NO_NET_DATA_EFFECT;
  NO_NET_INSTR_EFFECT;

  return s;
}


void frstor(fpu_addr_modes addr_modes)
{
  int i, stnr;
  unsigned char tag;
  char *s = fldenv(addr_modes);

  for ( i = 0; i < 8; i++ )
    {
      /* Load each register. */
      FPU_data_address = (void *)(s+i*10);
      reg_load_extended();
      stnr = (i+top) & 7;
      tag = regs[stnr].tag;   /* Derived from the loaded tag word. */
      reg_move(&FPU_loaded_data, &regs[stnr]);
      if ( tag == TW_Empty )  /* The loaded data over-rides all other cases. */
	regs[stnr].tag = tag;
    }

  /* Reverse the effect which loading the registers had on the
     data pointer */
  NO_NET_DATA_EFFECT;

}


unsigned short tag_word(void)
{
  unsigned short word = 0;
  unsigned char tag;
  int i;

  for ( i = 7; i >= 0; i-- )
    {
      switch ( tag = regs[i].tag )
	{
	case TW_Valid:
	  if ( regs[i].exp <= (EXP_BIAS - EXTENDED_Ebias) )
	    tag = 2;
	  break;
	case TW_Infinity:
	case TW_NaN:
	  tag = 2;
	  break;
	case TW_Empty:
	  tag = 3;
	  break;
	  /* TW_Zero already has the correct value */
	}
      word <<= 2;
      word |= tag;
    }
  return word;
}


char *fstenv(fpu_addr_modes addr_modes)
{
  char *d = (char *)FPU_data_address;

  if ( addr_modes.vm86
      || (addr_modes.override.operand_size == OP_SIZE_PREFIX) )
    {
      RE_ENTRANT_CHECK_OFF;
      FPU_verify_area(VERIFY_WRITE,d,14);
#ifdef PECULIAR_486
      put_fs_long(control_word & ~0xe080, (unsigned short *) d);
#else
      put_fs_word(control_word, (unsigned short *) d);
#endif PECULIAR_486
      put_fs_word(status_word(), (unsigned short *) (d+2));
      put_fs_word(tag_word(), (unsigned short *) (d+4));
      put_fs_word(ip_offset, (unsigned short *) (d+6));
      put_fs_word(data_operand_offset, (unsigned short *) (d+0x0a));
      if ( addr_modes.vm86 )
	{
	  put_fs_word((ip_offset & 0xf0000) >> 4,
		      (unsigned short *) (d+8));
	  put_fs_word((data_operand_offset & 0xf0000) >> 4,
		      (unsigned short *) (d+0x0c));
	}
      else
	{
	  put_fs_word(cs_selector, (unsigned short *) (d+8));
	  put_fs_word(operand_selector, (unsigned short *) (d+0x0c));
	}
      RE_ENTRANT_CHECK_ON;
      d += 0x0e;
    }
  else
    {
      RE_ENTRANT_CHECK_OFF;
      FPU_verify_area(VERIFY_WRITE,d,28);
#ifdef PECULIAR_486
      /* An 80486 sets all the reserved bits to 1. */
      put_fs_long(0xffff0040 | (control_word & ~0xe080), (unsigned long *) d);
      put_fs_long(0xffff0000 | status_word(), (unsigned long *) (d+4));
      put_fs_long(0xffff0000 | tag_word(), (unsigned long *) (d+8));
#else
      put_fs_word(control_word, (unsigned short *) d);
      put_fs_word(status_word(), (unsigned short *) (d+4));
      put_fs_word(tag_word(), (unsigned short *) (d+8));
#endif PECULIAR_486
      put_fs_long(ip_offset, (unsigned long *) (d+0x0c));
      put_fs_long(cs_selector & ~0xf8000000, (unsigned long *) (d+0x10));
      put_fs_long(data_operand_offset, (unsigned long *) (d+0x14));
#ifdef PECULIAR_486
      /* An 80486 sets all the reserved bits to 1. */
      put_fs_long(0xffff0000 | operand_selector, (unsigned long *) (d+0x18));
#else
      put_fs_long(operand_selector, (unsigned long *) (d+0x18));
#endif PECULIAR_486
      RE_ENTRANT_CHECK_ON;
      d += 0x1c;
    }
  
  control_word |= CW_Exceptions;
  partial_status &= ~(SW_Summary | SW_Backward);

  return d;
}


void fsave(fpu_addr_modes addr_modes)
{
  char *d;
  int i;

  d = fstenv(addr_modes);
  RE_ENTRANT_CHECK_OFF;
  FPU_verify_area(VERIFY_WRITE,d,80);
  RE_ENTRANT_CHECK_ON;
  for ( i = 0; i < 8; i++ )
    write_to_extended(&regs[(top + i) & 7], d + 10 * i);

  finit();

}

/*===========================================================================*/

/*
  A call to this function must be preceeded by a call to
  FPU_verify_area() to verify access to the 10 bytes at d
  */
static void write_to_extended(FPU_REG *rp, char *d)
{
  long e;
  FPU_REG tmp;
  
  e = rp->exp - EXP_BIAS + EXTENDED_Ebias;

#ifdef PARANOID
  switch ( rp->tag )
    {
    case TW_Zero:
      if ( rp->sigh | rp->sigl | e )
	EXCEPTION(EX_INTERNAL | 0x114);
      break;
    case TW_Infinity:
    case TW_NaN:
      if ( (e ^ 0x7fff) | !(rp->sigh & 0x80000000) )
	EXCEPTION(EX_INTERNAL | 0x114);
      break;
    default:
      if (e > 0x7fff || e < -63)
	EXCEPTION(EX_INTERNAL | 0x114);
    }
#endif PARANOID

  /*
    All numbers except denormals are stored internally in a
    format which is compatible with the extended real number
    format.
   */
  if ( e > 0 )
    {
      /* just copy the reg */
      RE_ENTRANT_CHECK_OFF;
      put_fs_long(rp->sigl, (unsigned long *) d);
      put_fs_long(rp->sigh, (unsigned long *) (d + 4));
      RE_ENTRANT_CHECK_ON;
    }
  else
    {
      /*
	The number is a de-normal stored as a normal using our
	extra exponent range, or is Zero.
	Convert it back to a de-normal, or leave it as Zero.
       */
      reg_move(rp, &tmp);
      tmp.exp += -EXTENDED_Emin + 63;  /* largest exp to be 63 */
      round_to_int(&tmp);
      e = 0;
      RE_ENTRANT_CHECK_OFF;
      put_fs_long(tmp.sigl, (unsigned long *) d);
      put_fs_long(tmp.sigh, (unsigned long *) (d + 4));
      RE_ENTRANT_CHECK_ON;
    }
  e |= rp->sign == SIGN_POS ? 0 : 0x8000;
  RE_ENTRANT_CHECK_OFF;
  put_fs_word(e, (unsigned short *) (d + 8));
  RE_ENTRANT_CHECK_ON;
}
