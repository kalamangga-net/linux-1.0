#ifndef _LINUX_DELAY_H
#define _LINUX_DELAY_H

/*
 * Copyright (C) 1993 Linus Torvalds
 *
 * Delay routines, using a pre-computed "loops_per_second" value.
 */

extern unsigned long loops_per_sec;

extern __inline__ void __delay(int loops)
{
	__asm__(".align 2,0x90\n1:\tdecl %0\n\tjns 1b": :"a" (loops):"ax");
}

/*
 * division by multiplication: you don't have to worry about
 * loss of precision.
 *
 * Use only for very small delays ( < 1 msec).  Should probably use a
 * lookup table, really, as the multiplications take much too long with
 * short delays.  This is a "reasonable" implementation, though (and the
 * first constant multiplications gets optimized away if the delay is
 * a constant)
 */
extern __inline__ void udelay(unsigned long usecs)
{
	usecs *= 0x000010c6;		/* 2**32 / 1000000 */
	__asm__("mull %0"
		:"=d" (usecs)
		:"a" (usecs),"0" (loops_per_sec)
		:"ax");
	__delay(usecs);
}

#endif
