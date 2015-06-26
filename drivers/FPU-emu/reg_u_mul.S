	.file	"reg_u_mul.S"
/*---------------------------------------------------------------------------+
 |  reg_u_mul.S                                                              |
 |                                                                           |
 | Core multiplication routine                                               |
 |                                                                           |
 | Copyright (C) 1992,1993                                                   |
 |                       W. Metzenthen, 22 Parker St, Ormond, Vic 3163,      |
 |                       Australia.  E-mail   billm@vaxc.cc.monash.edu.au    |
 |                                                                           |
 |                                                                           |
 +---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------+
 |   Basic multiplication routine.                                           |
 |   Does not check the resulting exponent for overflow/underflow            |
 |                                                                           |
 |   reg_u_mul(FPU_REG *a, FPU_REG *b, FPU_REG *c, unsigned int cw);         |
 |                                                                           |
 |   Internal working is at approx 128 bits.                                 |
 |   Result is rounded to nearest 53 or 64 bits, using "nearest or even".    |
 +---------------------------------------------------------------------------*/

#include "exception.h"
#include "fpu_asm.h"
#include "control_w.h"



#ifdef REENTRANT_FPU
/*  Local storage on the stack: */
#define FPU_accum_0	-4(%ebp)	/* ms word */
#define FPU_accum_1	-8(%ebp)

#else
/*  Local storage in a static area: */
.data
	.align 4,0
FPU_accum_0:
	.long	0
FPU_accum_1:
	.long	0
#endif REENTRANT_FPU


.text
	.align 2,144

.globl _reg_u_mul
_reg_u_mul:
	pushl	%ebp
	movl	%esp,%ebp
#ifdef REENTRANT_FPU
	subl	$8,%esp
#endif REENTRANT_FPU

	pushl	%esi
	pushl	%edi
	pushl	%ebx

	movl	PARAM1,%esi
	movl	PARAM2,%edi

#ifdef PARANOID
	testl	$0x80000000,SIGH(%esi)
	jz	L_bugged
	testl	$0x80000000,SIGH(%edi)
	jz	L_bugged
#endif PARANOID

#ifdef DENORM_OPERAND
	movl	EXP(%esi),%eax
	cmpl	EXP_UNDER,%eax
	jg	xOp1_not_denorm

	call	_denormal_operand
	orl	%eax,%eax
	jnz	fpu_Arith_exit

xOp1_not_denorm:
	movl	EXP(%edi),%eax
	cmpl	EXP_UNDER,%eax
	jg	xOp2_not_denorm

	call	_denormal_operand
	orl	%eax,%eax
	jnz	fpu_Arith_exit

xOp2_not_denorm:
#endif DENORM_OPERAND

	xorl	%ecx,%ecx
	xorl	%ebx,%ebx

	movl	SIGL(%esi),%eax
	mull	SIGL(%edi)
	movl	%eax,FPU_accum_0
	movl	%edx,FPU_accum_1

	movl	SIGL(%esi),%eax
	mull	SIGH(%edi)
	addl	%eax,FPU_accum_1
	adcl	%edx,%ebx
/*	adcl	$0,%ecx		// overflow here is not possible */

	movl	SIGH(%esi),%eax
	mull	SIGL(%edi)
	addl	%eax,FPU_accum_1
	adcl	%edx,%ebx
	adcl	$0,%ecx

	movl	SIGH(%esi),%eax
	mull	SIGH(%edi)
	addl	%eax,%ebx
	adcl	%edx,%ecx

	movl	EXP(%esi),%eax	/* Compute the exponent */
	addl	EXP(%edi),%eax
	subl	EXP_BIAS-1,%eax

/*  Have now finished with the sources */
	movl	PARAM3,%edi	/* Point to the destination */
	movl	%eax,EXP(%edi)

/*  Now make sure that the result is normalized */
	testl	$0x80000000,%ecx
	jnz	LResult_Normalised

	/* Normalize by shifting left one bit */
	shll	$1,FPU_accum_0
	rcll	$1,FPU_accum_1
	rcll	$1,%ebx
	rcll	$1,%ecx
	decl	EXP(%edi)

LResult_Normalised:
	movl	FPU_accum_0,%eax
	movl	FPU_accum_1,%edx
	orl	%eax,%eax
	jz	L_extent_zero

	orl	$1,%edx

L_extent_zero:
	movl	%ecx,%eax
	jmp	fpu_reg_round


#ifdef PARANOID
L_bugged:
	pushl	EX_INTERNAL|0x205
	call	EXCEPTION
	pop	%ebx
	jmp	L_exit

L_exit:
	popl	%ebx
	popl	%edi
	popl	%esi
	leave
	ret
#endif PARANOID

