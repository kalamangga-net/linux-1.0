	.file	"reg_u_add.S"
/*---------------------------------------------------------------------------+
 |  reg_u_add.S                                                              |
 |                                                                           |
 | Add two valid (TW_Valid) FPU_REG numbers, of the same sign, and put the   |
 |   result in a destination FPU_REG.                                        |
 |                                                                           |
 | Copyright (C) 1992,1993                                                   |
 |                       W. Metzenthen, 22 Parker St, Ormond, Vic 3163,      |
 |                       Australia.  E-mail   billm@vaxc.cc.monash.edu.au    |
 |                                                                           |
 | Call from C as:                                                           |
 |   void reg_u_add(FPU_REG *arg1, FPU_REG *arg2, FPU_REG *answ,             |
 |                                                int control_w)             |
 |                                                                           |
 +---------------------------------------------------------------------------*/

/*
 |    Kernel addition routine reg_u_add(reg *arg1, reg *arg2, reg *answ).
 |    Takes two valid reg f.p. numbers (TW_Valid), which are
 |    treated as unsigned numbers,
 |    and returns their sum as a TW_Valid or TW_S f.p. number.
 |    The returned number is normalized.
 |    Basic checks are performed if PARANOID is defined.
 */

#include "exception.h"
#include "fpu_asm.h"
#include "control_w.h"

.text
	.align 2,144
.globl _reg_u_add
_reg_u_add:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%esi
	pushl	%edi
	pushl	%ebx

	movl	PARAM1,%esi		/* source 1 */
	movl	PARAM2,%edi		/* source 2 */

#ifdef DENORM_OPERAND
	cmpl	EXP_UNDER,EXP(%esi)
	jg	xOp1_not_denorm

	call	_denormal_operand
	orl	%eax,%eax
	jnz	fpu_Arith_exit

xOp1_not_denorm:
	cmpl	EXP_UNDER,EXP(%edi)
	jg	xOp2_not_denorm

	call	_denormal_operand
	orl	%eax,%eax
	jnz	fpu_Arith_exit

xOp2_not_denorm:
#endif DENORM_OPERAND

	movl	EXP(%esi),%ecx
	subl	EXP(%edi),%ecx		/* exp1 - exp2 */
	jge	L_arg1_larger

	/* num1 is smaller */
	movl	SIGL(%esi),%ebx
	movl	SIGH(%esi),%eax

	movl	%edi,%esi
	negw	%cx
	jmp	L_accum_loaded

L_arg1_larger:
	/* num1 has larger or equal exponent */
	movl	SIGL(%edi),%ebx
	movl	SIGH(%edi),%eax

L_accum_loaded:
	movl	PARAM3,%edi		/* destination */
/*	movb	SIGN(%esi),%dl
	movb	%dl,SIGN(%edi) */	/* Copy the sign from the first arg */


	movl	EXP(%esi),%edx
	movl	%edx,EXP(%edi)		/* Copy exponent to destination */

	xorl	%edx,%edx		/* clear the extension */

#ifdef PARANOID
	testl	$0x80000000,%eax
	je	L_bugged

	testl	$0x80000000,SIGH(%esi)
	je	L_bugged
#endif PARANOID

/* The number to be shifted is in %eax:%ebx:%edx */
	cmpw	$32,%cx		/* shrd only works for 0..31 bits */
	jnc	L_more_than_31

/* less than 32 bits */
	shrd	%cl,%ebx,%edx
	shrd	%cl,%eax,%ebx
	shr	%cl,%eax
	jmp	L_shift_done

L_more_than_31:
	cmpw	$64,%cx
	jnc	L_more_than_63

	subb	$32,%cl
	jz	L_exactly_32

	shrd	%cl,%eax,%edx
	shr	%cl,%eax
	orl	%ebx,%ebx
	jz	L_more_31_no_low	/* none of the lowest bits is set */

	orl	$1,%edx			/* record the fact in the extension */

L_more_31_no_low:
	movl	%eax,%ebx
	xorl	%eax,%eax
	jmp	L_shift_done

L_exactly_32:
	movl	%ebx,%edx
	movl	%eax,%ebx
	xorl	%eax,%eax
	jmp	L_shift_done

L_more_than_63:
	cmpw	$65,%cx
	jnc	L_more_than_64

	movl	%eax,%edx
	orl	%ebx,%ebx
	jz	L_more_63_no_low

	orl	$1,%edx
	jmp	L_more_63_no_low

L_more_than_64:
	movl	$1,%edx		/* The shifted nr always at least one '1' */

L_more_63_no_low:
	xorl	%ebx,%ebx
	xorl	%eax,%eax

L_shift_done:
	/* Now do the addition */
	addl	SIGL(%esi),%ebx
	adcl	SIGH(%esi),%eax
	jnc	L_round_the_result

	/* Overflow, adjust the result */
	rcrl	$1,%eax
	rcrl	$1,%ebx
	rcrl	$1,%edx
	jnc	L_no_bit_lost

	orl	$1,%edx

L_no_bit_lost:
	incl	EXP(%edi)

L_round_the_result:
	jmp	fpu_reg_round	/* Round the result */



#ifdef PARANOID
/* If we ever get here then we have problems! */
L_bugged:
	pushl	EX_INTERNAL|0x201
	call	EXCEPTION
	pop	%ebx
	jmp	L_exit
#endif PARANOID


L_exit:
	popl	%ebx
	popl	%edi
	popl	%esi
	leave
	ret
