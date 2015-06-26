/*---------------------------------------------------------------------------+
 |  fpu_system.h                                                             |
 |                                                                           |
 | Copyright (C) 1992,1994                                                   |
 |                       W. Metzenthen, 22 Parker St, Ormond, Vic 3163,      |
 |                       Australia.  E-mail   billm@vaxc.cc.monash.edu.au    |
 |                                                                           |
 +---------------------------------------------------------------------------*/

#ifndef _FPU_SYSTEM_H
#define _FPU_SYSTEM_H

/* system dependent definitions */

#include <linux/sched.h>
#include <linux/kernel.h>

/* This sets the pointer FPU_info to point to the argument part
   of the stack frame of math_emulate() */
#define SETUP_DATA_AREA(arg)    FPU_info = (struct info *) &arg

#define I387			(current->tss.i387)
#define FPU_info		(I387.soft.info)

#define FPU_CS			(*(unsigned short *) &(FPU_info->___cs))
#define FPU_SS			(*(unsigned short *) &(FPU_info->___ss))
#define FPU_DS			(*(unsigned short *) &(FPU_info->___ds))
#define FPU_EAX			(FPU_info->___eax)
#define FPU_EFLAGS		(FPU_info->___eflags)
#define FPU_EIP			(FPU_info->___eip)
#define FPU_ORIG_EIP		(FPU_info->___orig_eip)

#define FPU_lookahead           (I387.soft.lookahead)
#define FPU_entry_eip           (I387.soft.entry_eip)

#define partial_status       	(I387.soft.swd)
#define control_word		(I387.soft.cwd)
#define regs			(I387.soft.regs)
#define top			(I387.soft.top)

#define ip_offset		(I387.soft.fip)
#define cs_selector		(I387.soft.fcs)
#define data_operand_offset	(I387.soft.foo)
#define operand_selector	(I387.soft.fos)

#define FPU_verify_area(x,y,z)  if ( verify_area(x,y,z) ) \
                                math_abort(FPU_info,SIGSEGV)

#undef FPU_IGNORE_CODE_SEGV
#ifdef FPU_IGNORE_CODE_SEGV
/* verify_area() is very expensive, and causes the emulator to run
   about 20% slower if applied to the code. Anyway, errors due to bad
   code addresses should be much rarer than errors due to bad data
   addresses. */
#define	FPU_code_verify_area(z)
#else
/* A simpler test than verify_area() can probably be done for
   FPU_code_verify_area() because the only possible error is to step
   past the upper boundary of a legal code area. */
#define	FPU_code_verify_area(z) FPU_verify_area(VERIFY_READ,(void *)FPU_EIP,z)
#endif

/* ######## temporary and ugly ;-) */
#define FPU_data_address        ((void *)(I387.soft.twd))

#endif
