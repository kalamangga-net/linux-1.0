/*---------------------------------------------------------------------------+
 |  polynomial.S                                                             |
 |                                                                           |
 | Fixed point arithmetic polynomial evaluation.                             |
 |                                                                           |
 | Copyright (C) 1992,1993                                                   |
 |                       W. Metzenthen, 22 Parker St, Ormond, Vic 3163,      |
 |                       Australia.  E-mail   billm@vaxc.cc.monash.edu.au    |
 |                                                                           |
 | Call from C as:                                                           |
 |   void polynomial(unsigned accum[], unsigned x[], unsigned terms[][2],    |
 |                   int n)                                                  |
 |                                                                           |
 | Computes:                                                                 |
 | terms[0] + (terms[1] + (terms[2] + ... + (terms[n-1]*x)*x)*x)*x) ... )*x  |
 | The result is returned in accum.                                          |
 |                                                                           |
 +---------------------------------------------------------------------------*/

	.file	"fpolynom.s"

#include "fpu_asm.h"


/*	#define	EXTRA_PRECISE	// Do not use: not complete */

#define	TERM_SIZE	$8
#define	SUM_MS		-20(%ebp)	/* sum ms long */
#define SUM_MIDDLE	-24(%ebp)	/* sum middle long */
#define	SUM_LS		-28(%ebp)	/* sum ls long */
#define	SUM_LS_HI	-25(%ebp)	/* high byte of sum ls */
#define	ACCUM_MS	-4(%ebp)	/* accum ms long */
#define	ACCUM_MIDDLE	-8(%ebp)	/* accum middle long */
#define	ACCUM_LS	-12(%ebp)	/* accum ls long */
#define ACCUM_LS_HI	-9(%ebp)	/* high byte of accum ls */

.text
	.align 2,144
.globl _polynomial
_polynomial:
	pushl	%ebp
	movl	%esp,%ebp
	subl	$32,%esp
	pushl	%esi
	pushl	%edi
	pushl	%ebx

	movl	PARAM2,%esi		/* x */
	movl	PARAM3,%edi		/* terms */

	movl	TERM_SIZE,%eax
	mull	PARAM4			/* n */
	addl	%eax,%edi

	movl	4(%edi),%edx		/* terms[n] */
	movl	%edx,SUM_MS
	movl	(%edi),%edx		/* terms[n] */
	movl	%edx,SUM_MIDDLE
	xor	%eax,%eax
	movl	%eax,SUM_LS

	subl	TERM_SIZE,%edi
	decl	PARAM4
	js	L_accum_done

L_accum_loop:
	xor	%eax,%eax
	movl	%eax,ACCUM_MS
	movl	%eax,ACCUM_MIDDLE

	movl	SUM_MIDDLE,%eax
	mull	(%esi)			/* x ls long */
/*	movl	%eax,-16(%ebp)		// Not needed */
	movl	%edx,ACCUM_LS

	movl	SUM_MIDDLE,%eax
	mull	4(%esi)			/* x ms long */
	addl	%eax,ACCUM_LS
	adcl	%edx,ACCUM_MIDDLE
	adcl	$0,ACCUM_MS

	movl	SUM_MS,%eax
	mull	(%esi)			/* x ls long */
	addl	%eax,ACCUM_LS
	adcl	%edx,ACCUM_MIDDLE
	adcl	$0,ACCUM_MS

	movl	SUM_MS,%eax
	mull	4(%esi)			/* x ms long */
	addl	%eax,ACCUM_MIDDLE
	adcl	%edx,ACCUM_MS

/*
 * Now put the sum of next term and the accumulator
 * into the sum register
 */
	movl	ACCUM_MIDDLE,%eax
	addl	(%edi),%eax		/* term ls long */
	movl	%eax,SUM_MIDDLE
	movl	ACCUM_MS,%eax
	adcl	4(%edi),%eax		/* term ms long */
	movl	%eax,SUM_MS

#ifdef EXTRA_PRECISE
	movl	ACCUM_LS,%eax
	movl	%eax,SUM_LS
#else
	testb	$0x80,ACCUM_LS_HI	/* ms bit of ACCUM_LS */
	je	L_no_poly_round

	addl	$1,SUM_MIDDLE
	adcl	$0,SUM_MS
L_no_poly_round:
#endif EXTRA_PRECISE

	subl	TERM_SIZE,%edi
	decl	PARAM4
	jns	L_accum_loop

L_accum_done:
#ifdef EXTRA_PRECISE
/* Round the result */
	testb	$128,SUM_LS_HI
	je	L_poly_done

	addl	$1,SUM_MIDDLE
	adcl	$0,SUM_MS
#endif EXTRA_PRECISE

L_poly_done:
	movl	PARAM1,%edi		/* accum */
	movl	SUM_MIDDLE,%eax
	movl	%eax,(%edi)
	movl	SUM_MS,%eax
	movl	%eax,4(%edi)

	popl	%ebx
	popl	%edi
	popl	%esi
	leave
	ret
