/*---------------------------------------------------------------------------+
 |  fpu_arith.c                                                              |
 |                                                                           |
 | Code to implement the FPU register/register arithmetic instructions       |
 |                                                                           |
 | Copyright (C) 1992,1993                                                   |
 |                       W. Metzenthen, 22 Parker St, Ormond, Vic 3163,      |
 |                       Australia.  E-mail   billm@vaxc.cc.monash.edu.au    |
 |                                                                           |
 |                                                                           |
 +---------------------------------------------------------------------------*/

#include "fpu_system.h"
#include "fpu_emu.h"
#include "control_w.h"
#include "status_w.h"


void fadd__()
{
  /* fadd st,st(i) */
  clear_C1();
  reg_add(FPU_st0_ptr, &st(FPU_rm), FPU_st0_ptr, control_word);
}


void fmul__()
{
  /* fmul st,st(i) */
  clear_C1();
  reg_mul(FPU_st0_ptr, &st(FPU_rm), FPU_st0_ptr, control_word);
}



void fsub__()
{
  /* fsub st,st(i) */
  clear_C1();
  reg_sub(FPU_st0_ptr, &st(FPU_rm), FPU_st0_ptr, control_word);
}


void fsubr_()
{
  /* fsubr st,st(i) */
  clear_C1();
  reg_sub(&st(FPU_rm), FPU_st0_ptr, FPU_st0_ptr, control_word);
}


void fdiv__()
{
  /* fdiv st,st(i) */
  clear_C1();
  reg_div(FPU_st0_ptr, &st(FPU_rm), FPU_st0_ptr, control_word);
}


void fdivr_()
{
  /* fdivr st,st(i) */
  clear_C1();
  reg_div(&st(FPU_rm), FPU_st0_ptr, FPU_st0_ptr, control_word);
}



void fadd_i()
{
  /* fadd st(i),st */
  clear_C1();
  reg_add(FPU_st0_ptr, &st(FPU_rm), &st(FPU_rm), control_word);
}


void fmul_i()
{
  /* fmul st(i),st */
  clear_C1();
  reg_mul(FPU_st0_ptr, &st(FPU_rm), &st(FPU_rm), control_word);
}


void fsubri()
{
  /* fsubr st(i),st */
  /* This is the sense of the 80486 manual
     reg_sub(&st(FPU_rm), FPU_st0_ptr, &st(FPU_rm), control_word); */
  clear_C1();
  reg_sub(FPU_st0_ptr, &st(FPU_rm), &st(FPU_rm), control_word);
}


void fsub_i()
{
  /* fsub st(i),st */
  /* This is the sense of the 80486 manual
     reg_sub(FPU_st0_ptr, &st(FPU_rm), &st(FPU_rm), control_word); */
  clear_C1();
  reg_sub(&st(FPU_rm), FPU_st0_ptr, &st(FPU_rm), control_word);
}


void fdivri()
{
  /* fdivr st(i),st */
  clear_C1();
  reg_div(FPU_st0_ptr, &st(FPU_rm), &st(FPU_rm), control_word);
}


void fdiv_i()
{
  /* fdiv st(i),st */
  clear_C1();
  reg_div(&st(FPU_rm), FPU_st0_ptr, &st(FPU_rm), control_word);
}



void faddp_()
{
  /* faddp st(i),st */
  clear_C1();
  if ( !reg_add(FPU_st0_ptr, &st(FPU_rm), &st(FPU_rm), control_word) )
    pop();
}


void fmulp_()
{
  /* fmulp st(i),st */
  clear_C1();
  if ( !reg_mul(FPU_st0_ptr, &st(FPU_rm), &st(FPU_rm), control_word) )
    pop();
}



void fsubrp()
{
  /* fsubrp st(i),st */
  /* This is the sense of the 80486 manual
     reg_sub(&st(FPU_rm), FPU_st0_ptr, &st(FPU_rm), control_word); */
  clear_C1();
  if ( !reg_sub(FPU_st0_ptr, &st(FPU_rm), &st(FPU_rm), control_word) )
    pop();
}


void fsubp_()
{
  /* fsubp st(i),st */
  /* This is the sense of the 80486 manual
     reg_sub(FPU_st0_ptr, &st(FPU_rm), &st(FPU_rm), control_word); */
  clear_C1();
  if ( !reg_sub(&st(FPU_rm), FPU_st0_ptr, &st(FPU_rm), control_word) )
    pop();
}


void fdivrp()
{
  /* fdivrp st(i),st */
  clear_C1();
  if ( !reg_div(FPU_st0_ptr, &st(FPU_rm), &st(FPU_rm), control_word) )
    pop();
}


void fdivp_()
{
  /* fdivp st(i),st */
  clear_C1();
  if ( !reg_div(&st(FPU_rm), FPU_st0_ptr, &st(FPU_rm), control_word) )
    pop();
}

