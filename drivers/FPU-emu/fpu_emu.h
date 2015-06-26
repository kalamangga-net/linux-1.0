/*---------------------------------------------------------------------------+
 |  fpu_emu.h                                                                |
 |                                                                           |
 | Copyright (C) 1992,1993,1994                                              |
 |                       W. Metzenthen, 22 Parker St, Ormond, Vic 3163,      |
 |                       Australia.  E-mail   billm@vaxc.cc.monash.edu.au    |
 |                                                                           |
 +---------------------------------------------------------------------------*/


#ifndef _FPU_EMU_H_
#define _FPU_EMU_H_

/*
 * Define DENORM_OPERAND to make the emulator detect denormals
 * and use the denormal flag of the status word. Note: this only
 * affects the flag and corresponding interrupt, the emulator
 * will always generate denormals and operate upon them as required.
 */
#define DENORM_OPERAND

/*
 * Define PECULIAR_486 to get a closer approximation to 80486 behaviour,
 * rather than behaviour which appears to be cleaner.
 * This is a matter of opinion: for all I know, the 80486 may simply
 * be complying with the IEEE spec. Maybe one day I'll get to see the
 * spec...
 */
#define PECULIAR_486

#ifdef __ASSEMBLER__
#include "fpu_asm.h"
#define	Const(x)	$##x
#else
#define	Const(x)	x
#endif

#define EXP_BIAS	Const(0)
#define EXP_OVER	Const(0x4000)    /* smallest invalid large exponent */
#define	EXP_UNDER	Const(-0x3fff)   /* largest invalid small exponent */
#define EXP_Infinity    EXP_OVER
#define EXP_NaN         EXP_OVER

#define SIGN_POS	Const(0)
#define SIGN_NEG	Const(1)

/* Keep the order TW_Valid, TW_Zero, TW_Denormal */
#define TW_Valid	Const(0)	/* valid */
#define TW_Zero		Const(1)	/* zero */
/* The following fold to 2 (Special) in the Tag Word */
/* #define TW_Denormal     Const(4) */       /* De-normal */
#define TW_Infinity	Const(5)	/* + or - infinity */
#define	TW_NaN		Const(6)	/* Not a Number */

#define TW_Empty	Const(7)	/* empty */


#ifndef __ASSEMBLER__

#include <linux/math_emu.h>
#include <linux/linkage.h>

#ifdef PARANOID
extern char emulating;
#  define RE_ENTRANT_CHECK_OFF emulating = 0
#  define RE_ENTRANT_CHECK_ON emulating = 1
#else
#  define RE_ENTRANT_CHECK_OFF
#  define RE_ENTRANT_CHECK_ON
#endif PARANOID

#define FWAIT_OPCODE 0x9b
#define OP_SIZE_PREFIX 0x66
#define ADDR_SIZE_PREFIX 0x67
#define PREFIX_CS 0x2e
#define PREFIX_DS 0x3e
#define PREFIX_ES 0x26
#define PREFIX_SS 0x36
#define PREFIX_FS 0x64
#define PREFIX_GS 0x65
#define PREFIX_REPE 0xf3
#define PREFIX_REPNE 0xf2
#define PREFIX_LOCK 0xf0
#define PREFIX_CS_ 1
#define PREFIX_DS_ 2
#define PREFIX_ES_ 3
#define PREFIX_FS_ 4
#define PREFIX_GS_ 5
#define PREFIX_SS_ 6

/* These are to defeat the default action, giving the instruction
   no net effect: */
#define NO_NET_DATA_EFFECT \
      { FPU_data_address = (void *)data_operand_offset; \
	FPU_data_selector = operand_selector; }
#define NO_NET_INSTR_EFFECT \
      { FPU_entry_eip = ip_offset; \
	FPU_entry_op_cs = cs_selector; }


typedef void (*FUNC)(void);
typedef struct fpu_reg FPU_REG;
typedef struct { unsigned char address_size, operand_size, segment; }
        overrides;
/* This structure is 32 bits: */
typedef struct { overrides override;
		 unsigned char vm86; } fpu_addr_modes;

#define	st(x)	( regs[((top+x) &7 )] )

#define	STACK_OVERFLOW	(st_new_ptr = &st(-1), st_new_ptr->tag != TW_Empty)
#define	NOT_EMPTY(i)	(st(i).tag != TW_Empty)
#define	NOT_EMPTY_0	(FPU_st0_tag ^ TW_Empty)

extern unsigned char FPU_rm;

extern	char	FPU_st0_tag;
extern	FPU_REG	*FPU_st0_ptr;

/* ###### These need to be shifted to somewhere safe. */
/* extern void  *FPU_data_address; has been shifted */
extern unsigned short FPU_data_selector;
extern unsigned long FPU_entry_op_cs;

extern  FPU_REG  FPU_loaded_data;

#define pop()	{ FPU_st0_ptr->tag = TW_Empty; top++; }

/* push() does not affect the tags */
#define push()	{ top--; FPU_st0_ptr = st_new_ptr; }


#define reg_move(x, y) { \
		 *(short *)&((y)->sign) = *(short *)&((x)->sign); \
		 *(long *)&((y)->exp) = *(long *)&((x)->exp); \
		 *(long long *)&((y)->sigl) = *(long long *)&((x)->sigl); }

#define significand(x) ( ((unsigned long long *)&((x)->sigl))[0] )


/*----- Prototypes for functions written in assembler -----*/
/* extern void reg_move(FPU_REG *a, FPU_REG *b); */

asmlinkage void mul64(unsigned long long const *a, unsigned long long const *b,
		      unsigned long long *result);
asmlinkage void poly_div2(unsigned long long *x);
asmlinkage void poly_div4(unsigned long long *x);
asmlinkage void poly_div16(unsigned long long *x);
asmlinkage void polynomial(unsigned accum[], unsigned const x[],
		       unsigned short const terms[][4], int const n);
asmlinkage void normalize(FPU_REG *x);
asmlinkage void normalize_nuo(FPU_REG *x);
asmlinkage int reg_div(FPU_REG const *arg1, FPU_REG const *arg2,
		       FPU_REG *answ, unsigned int control_w);
asmlinkage int reg_u_sub(FPU_REG const *arg1, FPU_REG const *arg2,
			 FPU_REG *answ, unsigned int control_w);
asmlinkage int reg_u_mul(FPU_REG const *arg1, FPU_REG const *arg2,
			 FPU_REG *answ, unsigned int control_w);
asmlinkage int reg_u_div(FPU_REG const *arg1, FPU_REG const *arg2,
			 FPU_REG *answ, unsigned int control_w);
asmlinkage int reg_u_add(FPU_REG const *arg1, FPU_REG const *arg2,
			 FPU_REG *answ, unsigned int control_w);
asmlinkage int wm_sqrt(FPU_REG *n, unsigned int control_w);
asmlinkage unsigned	shrx(void *l, unsigned x);
asmlinkage unsigned	shrxs(void *v, unsigned x);
asmlinkage unsigned long div_small(unsigned long long *x, unsigned long y);
asmlinkage void round_reg(FPU_REG *arg, unsigned int extent,
		      unsigned int control_w);

#ifndef MAKING_PROTO
#include "fpu_proto.h"
#endif

#endif __ASSEMBLER__

#endif _FPU_EMU_H_
