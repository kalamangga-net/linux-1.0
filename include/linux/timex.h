/*****************************************************************************
 *                                                                           *
 * Copyright (c) David L. Mills 1993                                         *
 *                                                                           *
 * Permission to use, copy, modify, and distribute this software and its     *
 * documentation for any purpose and without fee is hereby granted, provided *
 * that the above copyright notice appears in all copies and that both the   *
 * copyright notice and this permission notice appear in supporting          *
 * documentation, and that the name University of Delaware not be used in    *
 * advertising or publicity pertaining to distribution of the software       *
 * without specific, written prior permission.  The University of Delaware   *
 * makes no representations about the suitability this software for any      *
 * purpose.  It is provided "as is" without express or implied warranty.     *
 *                                                                           *
 *****************************************************************************/

/*
 * Modification history timex.h
 * 
 * 17 Sep 93    David L. Mills
 *      Created file $NTP/include/sys/timex.h
 * 07 Oct 93    Torsten Duwe
 *      Derived linux/timex.h
 */
#ifndef _LINUX_TIMEX_H
#define _LINUX_TIMEX_H

/*
 * The following defines establish the engineering parameters of the PLL
 * model. The HZ variable establishes the timer interrupt frequency, 100 Hz 
 * for the SunOS kernel, 256 Hz for the Ultrix kernel and 1024 Hz for the
 * OSF/1 kernel. The SHIFT_HZ define expresses the same value as the
 * nearest power of two in order to avoid hardware multiply operations.
 */
#define SHIFT_HZ 7		/* log2(HZ) */

/*
 * The SHIFT_KG and SHIFT_KF defines establish the damping of the PLL
 * and are chosen by analysis for a slightly underdamped convergence
 * characteristic. The MAXTC define establishes the maximum time constant
 * of the PLL. With the parameters given and the default time constant of
 * zero, the PLL will converge in about 15 minutes.
 */
#define SHIFT_KG 8		/* shift for phase increment */
#define SHIFT_KF 20		/* shift for frequency increment */
#define MAXTC 6			/* maximum time constant (shift) */

/*
 * The SHIFT_SCALE define establishes the decimal point of the time_phase
 * variable which serves as a an extension to the low-order bits of the
 * system clock variable. The SHIFT_UPDATE define establishes the decimal
 * point of the time_offset variable which represents the current offset
 * with respect to standard time. The FINEUSEC define represents 1 usec in
 * scaled units.
 */
#define SHIFT_SCALE 24		/* shift for phase scale factor */
#define SHIFT_UPDATE (SHIFT_KG + MAXTC) /* shift for offset scale factor */
#define FINEUSEC (1 << SHIFT_SCALE) /* 1 us in scaled units */

#define MAXPHASE 128000         /* max phase error (us) */
#define MAXFREQ 100             /* max frequency error (ppm) */
#define MINSEC 16               /* min interval between updates (s) */
#define MAXSEC 1200             /* max interval between updates (s) */

#define CLOCK_TICK_RATE	1193180 /* Underlying HZ */
#define CLOCK_TICK_FACTOR	20	/* Factor of both 1000000 and CLOCK_TICK_RATE */
#define LATCH  ((CLOCK_TICK_RATE + HZ/2) / HZ)	/* For divider */

#define FINETUNE (((((LATCH * HZ - CLOCK_TICK_RATE) << SHIFT_HZ) * \
	(1000000/CLOCK_TICK_FACTOR) / (CLOCK_TICK_RATE/CLOCK_TICK_FACTOR)) \
		<< (SHIFT_SCALE-SHIFT_HZ)) / HZ)

/*
 * syscall interface - used (mainly by NTP daemon)
 * to discipline kernel clock oscillator
 */
struct timex {
	int mode;		/* mode selector */
	long offset;		/* time offset (usec) */
	long frequency;		/* frequency offset (scaled ppm) */
	long maxerror;		/* maximum error (usec) */
	long esterror;		/* estimated error (usec) */
	int status;		/* clock command/status */
	long time_constant;	/* pll time constant */
	long precision;		/* clock precision (usec) (read only) */
	long tolerance;		/* clock frequency tolerance (ppm)
				 * (read only)
				 */
	struct timeval time;	/* (read only) */
	long tick;		/* (modified) usecs between clock ticks */
};

/*
 * Mode codes (timex.mode) 
 */
#define ADJ_OFFSET		0x0001	/* time offset */
#define ADJ_FREQUENCY		0x0002	/* frequency offset */
#define ADJ_MAXERROR		0x0004	/* maximum time error */
#define ADJ_ESTERROR		0x0008	/* estimated time error */
#define ADJ_STATUS		0x0010	/* clock status */
#define ADJ_TIMECONST		0x0020	/* pll time constant */
#define ADJ_TICK		0x4000	/* tick value */
#define ADJ_OFFSET_SINGLESHOT	0x8001	/* old-fashioned adjtime */

/*
 * Clock command/status codes (timex.status)
 */
#define TIME_OK		0	/* clock synchronized */
#define TIME_INS	1	/* insert leap second */
#define TIME_DEL	2	/* delete leap second */
#define TIME_OOP	3	/* leap second in progress */
#define TIME_BAD	4	/* clock not synchronized */

#ifdef __KERNEL__
/*
 * kernel variables
 */
extern long tick;                      /* timer interrupt period */
extern int tickadj;			/* amount of adjustment per tick */
extern volatile struct timeval xtime;		/* The current time */

/*
 * phase-lock loop variables
 */
extern int time_status;		/* clock synchronization status */
extern long time_offset;	/* time adjustment (us) */
extern long time_constant;	/* pll time constant */
extern long time_tolerance;	/* frequency tolerance (ppm) */
extern long time_precision;	/* clock precision (us) */
extern long time_maxerror;	/* maximum error */
extern long time_esterror;	/* estimated error */
extern long time_phase;		/* phase offset (scaled us) */
extern long time_freq;		/* frequency offset (scaled ppm) */
extern long time_adj;		/* tick adjust (scaled 1 / HZ) */
extern long time_reftime;	/* time at last adjustment (s) */

extern long time_adjust;	/* The amount of adjtime left */
#endif /* KERNEL */

#endif /* LINUX_TIMEX_H */
