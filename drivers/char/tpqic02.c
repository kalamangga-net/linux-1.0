/* $Id: tpqic02.c,v 0.2.1.21 1993/06/18 19:04:33 root Exp root $
 *
 * Driver for tape drive support for Linux-i386 0.99.12.
 *
 * Copyright (c) 1993 by H. H. Bergman. All rights reserved.
 * Current e-mail address: csg279@wing.rug.nl
 * [If you are unable to reach me directly, try the TAPE mailing list
 * channel on linux-activists@niksula.hut.fi using "X-Mn-Key: TAPE" as
 * the first line in your message.]
 *
 * Distribution of this program in executable form is only allowed if
 * all of the corresponding source files are made available through the same
 * medium at no extra cost.
 *
 * I will not accept any responsibility for damage caused directly or
 * indirectly by this program, or code derived from this program.
 *
 * Use this code at your own risk. Don't blame me if it destroys your data!
 * Make sure you have a backup before you try this code.
 *
 * This driver was partially inspired by the 'wt' driver in the 386BSD
 * source distribution, which carries the following copyright notice:
 *
 *  Copyright (c) 1991 The Regents of the University of California.
 *  All rights reserved.
 *
 * You are not allowed to change this line nor the text above.
 *
 * $Log: tpqic02.c,v $
 * Revision 0.2.1.21  1993/06/18  19:04:33  root
 * minor fixes for 0.99.10.
 *
 * Revision 0.2.1.20  1993/06/11  21:38:51  root
 * Added exception code for status 0x8000 (Cypher weirdness).
 *
 * Revision 0.2.1.19  1993/04/19  23:13:59  root
 * Cleanups. Changed to 0.99.8.
 *
 * Revision 0.2.1.18  1993/03/22  17:39:47  root
 * Moved to 0.99.7. Added Archive MTSEEK and MTTELL support.
 *
 * Revision 0.2.1.17  1993/03/08  18:51:59  root
 * Tried to `fix' write-once bug in previous release.
 *
 * Revision 0.2.1.16  1993/03/01  00:06:16  root
 * Use register_chrdev() for 0.99.6.
 *
 * Revision 0.2.1.15  1993/02/25  00:14:25  root
 * minor cleanups.
 *
 * Revision 0.2.1.14  1993/01/25  00:06:14  root
 * Kernel udelay. Eof fixups.
 * Removed report_ read/write dummies; have strace(1) now.
 *
 * Revision 0.2.1.13  1993/01/10  02:24:43  root
 * Rewrote wait_for_ready() to use newer schedule() features.
 * This improves performance for rewinds etc.
 *
 * Revision 0.2.1.12  1993/01/05  18:44:09  root
 * Changes for 0.99.1. Fixed `restartable reads'.
 *
 * Revision 0.2.1.11  1992/11/28  01:19:10  root
 * Changes to exception handling (significant).
 * Changed returned error codes. Hopefully they're correct now.
 * Changed declarations to please gcc-2.3.1.
 * Patch to deal with bogus interrupts for Archive cards.
 *
 * Revision 0.2.1.10  1992/10/28  00:50:44  root
 * underrun/error counter needed byte swapping.
 *
 * Revision 0.2.1.9  1992/10/15  17:06:01  root
 * Removed online() stuff. Changed EOF handling.
 *
 * Revision 0.2.1.8  1992/10/02  22:25:48  root
 * Removed `no_sleep' parameters (got usleep() now),
 * cleaned up some comments.
 *
 * Revision 0.2.1.7  1992/09/27  01:41:55  root
 * Changed write() to do entire user buffer in one go, rather than just
 * a kernel-buffer sized portion each time.
 *
 * Revision 0.2.1.6  1992/09/21  02:15:30  root
 * Introduced udelay() function for microsecond-delays.
 * Trying to use get_dma_residue rather than TC flags.
 * Patch to fill entire user buffer on reads before
 * returning.
 *
 * Revision 0.2.1.5  1992/09/19  02:31:28  root
 * Some changes based on patches by Eddy Olk to
 * support Archive SC402/SC499R controller cards.
 *
 * Revision 0.2.1.4  1992/09/07  01:37:37  root
 * Minor changes
 *
 * Revision 0.2.1.3  1992/08/13  00:11:02  root
 * Added some support for Archive SC402 and SC499 cards.
 * (Untested.)
 *
 * Revision 0.2.1.2  1992/08/10  02:02:36  root
 * Changed from linux/system.h macros to asm/dma.h inline functions.
 *
 * Revision 0.2.1.1  1992/08/08  01:12:39  root
 * cleaned up a bit. added stuff for selftesting.
 * preparing for asm/dma.h instead of linux/system.h
 *
 * Revision 0.2  1992/08/03  20:11:30  root
 * Changed to use new IRQ allocation. Padding now done at runtime, pads to
 * 512 bytes. Because of this the page regs must be re-programmed every
 * block! Added hooks for selftest commands.
 * Moved to linux-0.97.
 *
 * Revision 0.1.0.5  1992/06/22  22:20:30  root
 * moved to Linux 0.96b
 *
 * Revision 0.1.0.4  1992/06/18  02:00:04  root
 * Use minor bit-7 to enable/disable printing of extra debugging info
 * when do tape access.
 * Added semop stuff for DMA/IRQ allocation checking. Don't think this
 * is the right way to do it though.
 *
 * Revision 0.1.0.3  1992/06/01  01:57:34  root
 * changed DRQ to DMA. added TDEBUG ifdefs to reduce output.
 *
 * Revision 0.1.0.2  1992/05/31  14:02:38  root
 * changed SET_DMA_PAGE handling slightly.
 *
 * Revision 0.1.0.1  1992/05/27  12:12:03  root
 * Can now use multiple files on tape (sort of).
 * First release.
 *
 * Revision 0.1  1992/05/26  01:16:31  root
 * Initial version. Copyright H. H. Bergman 1992
 *
 */

/* After the legalese, now the important bits:
 * 
 * This is a driver for the Wangtek 5150 tape drive with 
 * a QIC-02 controller for ISA-PC type computers.
 * Hopefully it will work with other QIC-02 tape drives as well.
 *
 * Make sure your setup matches the configuration parameters.
 * Also, be careful to avoid IO conflicts with other devices!
 */

#include <linux/config.h>

/* skip this driver if not required for this configuration */
#if CONFIG_TAPE_QIC02

/*
#define TDEBUG
*/

#define REALLY_SLOW_IO		/* it sure is ... */

#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/errno.h>
#include <linux/mtio.h>
#include <linux/fcntl.h>
#include <linux/delay.h>
#include <linux/tpqic02.h>

#include <asm/dma.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

/* check existence of required configuration parameters */
#if !defined(TAPE_QIC02_PORT) || \
    !defined(TAPE_QIC02_IRQ) || \
    !defined(TAPE_QIC02_DMA)
#error tape_qic02 configuration error
#endif


#define TPQIC_NAME	"tpqic02"

/* Linux outb() commands have (value,port) as parameters.
 * One might expect (port,value) instead, so beware!
 */

static volatile int ctlbits = 0;     /* control reg bits for tape interface */

static struct wait_queue *tape_qic02_transfer = NULL; /* sync rw with interrupts */

static volatile struct mtget ioctl_status;	/* current generic status */

static volatile struct tpstatus tperror;	/* last drive status */

static char rcs_revision[] = "$Revision: 0.2.1.21 $";
static char rcs_date[] = "$Date: 1993/06/18 19:04:33 $";

/* Flag bits for status and outstanding requests.
 * (Could all be put in one bit-field-struct.)
 * Some variables need `volatile' because they may be modified
 * by an interrupt.
 */
static volatile flag status_dead = YES;	/* device is legally dead until proven alive */
static 		flag status_open = NO;	/* in use or not */

static volatile flag status_bytes_wr = NO;	/* write FM at close or not */
static volatile flag status_bytes_rd = NO;	/* (rd|wr) used for rewinding */

static volatile unsigned long status_cmd_pending = 0; /* cmd in progress */
static volatile flag status_expect_int = NO;	/* ready for interrupts */
static volatile flag status_timer_on = NO; 	/* using time-out */
static volatile int  status_error = 0;		/* int handler may detect error */
static volatile flag status_eof_detected = NO;	/* end of file */
static volatile flag status_eom_detected = NO;	/* end of recorded media */
static volatile flag status_eot_detected = NO;	/* end of tape */
static volatile flag doing_read = NO;
static volatile flag doing_write = NO;

static volatile unsigned long dma_bytes_todo;
static volatile unsigned long dma_bytes_done;
static volatile unsigned dma_mode = 0;		/* !=0 also means DMA in use */
static 		flag need_rewind = YES;

static dev_t current_tape_dev = QIC02_TAPE_MAJOR << 8;
static int extra_blocks_left = BLOCKS_BEYOND_EW;


/* return_*_eof:
 *	NO:	not at EOF,
 *	YES:	tell app EOF was reached (return 0).
 *
 * return_*_eof==YES && reported_*_eof==NO  ==>
 *	return current buffer, next time(s) return EOF.
 *
 * return_*_eof==YES && reported_*_eof==YES  ==>
 *	at EOF and application knows it, so we can
 *	move on to the next file.
 *
 */
static flag return_read_eof = NO;	/* set to signal app EOF was reached */
static flag return_write_eof = NO;
static flag reported_read_eof = NO;	/* set when we've done that */
static flag reported_write_eof = NO;


#ifdef TP_HAVE_SEEK
/* This is for doing `mt seek <blocknr>' */
static char seek_addr_buf[SEEK_BUF_SIZE];
#endif


/* In write mode, we have to write a File Mark after the last block written, 
 * when the tape device is closed. Tape repositioning and reading in write
 * mode is allowed as long as no actual writing has been done. After writing
 * the File Mark, repositioning and reading are allowed again.
 */
static int  mode_access;	/* access mode: READ or WRITE */


/* This is the actual kernel buffer where the interrupt routines read
 * from/write to. It is needed because the DMA channels 1 and 3 cannot
 * access the user buffers. [The kernel buffer must reside in the lower
 * 1MBytes of system memory because of the DMA controller.]
 * The user must ensure that a large enough buffer is passed to the
 * kernel, in order to reduce tape repositioning.
 *
 * The buffer is 512 bytes larger than expected, because I want to align it
 * at 512 bytes, to prevent problems with 64k boundaries.
 */

static volatile char tape_qic02_buf[TPQBUF_SIZE+TAPE_BLKSIZE];
/* A really good compiler would be able to align this at 512 bytes... :-( */

static unsigned long buffaddr;	/* aligned physical address of buffer */


/* This translates minor numbers to the corresponding recording format: */
static char *format_names[] = {
	"not set",	/* for dumb drives unable to handle format selection */
	"11",		/* extinct */
	"24",
	"120",
	"150",
	"300",		/* untested. */
	"600"		/* untested. */
};


/* `exception_list' is needed for exception status reporting.
 * Exceptions 1..14 are defined by QIC-02 rev F.
 * The drive status is matched sequentially to each entry,
 * ignoring irrelevant bits, until a match is found. If no
 * match is found, exception number 0 is used. (That should of
 * course never happen...) The original table was based on the
 * "Exception Status Summary" in QIC-02 rev F, but some changes
 * were required to make it work with real-world drives.
 *
 * Exception 1 (CNI) is changed to also cover status 0x00e0 (mask USL),
 * Exception 4 (EOM) is changed to also cover status 0x8288 (mask EOR),
 * Exception 11 (FIL) is changed to also cover status 0x0089 (mask EOM).
 * Exception 15 (EOR) is added for seek-to-end-of-data (catch EOR),
 * Exception 16 (BOM) is added for beginning-of-media (catch BOM).
 */
static struct exception_list_type {
	short mask, code;
	char *msg;
} exception_list[] = {
	{0, 0,
		"Unknown exception status code",		/* extra: 0 */},
	{~(TP_WRP|TP_USL), TP_ST0|TP_CNI,
		/* My Wangtek 5150EQ sometimes reports a status code
		 * of 0x00e0, which is not a valid exception code, but
		 * I think it should be recognized as "NO CARTRIDGE".
		 */
		"Cartridge not in place"			/* 1 */},
	{-1, TP_ST0|TP_CNI|TP_USL|TP_WRP,
		"Drive not online"				/* 2 */},
	{~(TP_ST1|TP_BOM), TP_ST0|TP_WRP,
		"Write protected cartridge"			/* 3 */},
	{~(TP_ST1|TP_EOR), TP_ST0|TP_EOM,
		"End of media"					/* 4 */},
	{~TP_WRP, TP_ST0|TP_UDA| TP_ST1|TP_BOM,
		"Read or Write abort. Rewind tape."		/* 5 */},
	{~TP_WRP, TP_ST0|TP_UDA,
		"Read error. Bad block transferred."		/* 6 */},
	{~TP_WRP, TP_ST0|TP_UDA|TP_BNL,
		"Read error. Filler block transferred."		/* 7 */},
	{~TP_WRP, TP_ST0|TP_UDA|TP_BNL |TP_ST1|TP_NDT,
		"Read error. No data detected."			/* 8 */},
	{~TP_WRP, TP_ST0|TP_EOM|TP_UDA|TP_BNL |TP_ST1|TP_NDT,
		"Read error. No data detected. EOM."		/* 9 */},
	{~(TP_WRP|TP_MBD|TP_PAR|TP_EOR), TP_ST0|TP_UDA|TP_BNL |TP_ST1|TP_NDT|TP_BOM,
		"Read error. No data detected. BOM."		/* 10 */},
	{~(TP_WRP|TP_EOM), TP_ST0|TP_FIL,
		/* Status 0x0089 (EOM & FM) is viewed as an FM,
		 * because it can only happen during a read.
		 * EOM is checked separately for an FM condition.
		 */
		"File mark detected"				/* 11 */},
	{~(TP_ST0|TP_CNI|TP_USL|TP_WRP|TP_BOM), TP_ST1|TP_ILL,
		"Illegal command"				/* 12 */},
	{~(TP_ST0|TP_CNI|TP_USL|TP_WRP|TP_BOM), TP_ST1|TP_POR,
		"Reset occurred"					/* 13 */},
	{~TP_WRP, TP_ST0|TP_FIL|TP_MBD,		/* NOTE: ST1 not set! */
		"Marginal block detected"			/* 14 */},
	{~(TP_ST0|TP_WRP|TP_EOM|TP_UDA|TP_BNL|TP_FIL |TP_NDT), TP_ST1|TP_EOR,
		/********** Is the extra TP_NDT really needed Eddy? **********/
		"End of recorded media"				/* extra: 15 */},
		/* 15 is returned when SEEKEOD completes successfully */
	{~(TP_WRP|TP_ST0), TP_ST1|TP_BOM,
		"Beginning of media"				/* extra: 16 */}
#ifdef CYPHER_BUG
	/* Perhaps the Cypher driver clears the TP_BOM bit after the
	 * status has been read? The QIC-02 specs explicitly state that
	 * the BOM bit should remain set as long as the tape is logically
	 * at the beginning of the tape.
	 */
	,{-1, TP_ST1,
		"Hmm, this must be Cypher drive... Aaargh"	/* extra */}
#endif
};
#define NR_OF_EXC	(sizeof(exception_list)/sizeof(struct exception_list_type))



static void tpqputs(char *s)
{
	printk(TPQIC_NAME ": %s\n", s);
} /* tpqputs */


/* Perform byte order swapping for a 16-bit word.
 *
 * [FIXME] This should probably be in include/asm/
 * ([FIXME] i486 can do this faster)
 */
static inline void byte_swap_w(volatile unsigned short * w)
{
	int t = *w;

	*w = (t>>8) | ((t & 0xff)<<8);
}



/* Init control register bits on interface card.
 * For Archive, interrupts must be enabled explicitly.
 * Wangtek interface card requires ONLINE to be set, Archive SC402/SC499R
 * cards keep it active all the time.
 */
static void ifc_init(void)
{
#if TAPE_QIC02_IFC == WANGTEK
	ctlbits = WT_CTL_ONLINE;	/* online */
	outb_p(ctlbits, QIC_CTL_PORT);

#elif TAPE_QIC02_IFC == ARCHIVE
	ctlbits = 0;			/* no interrupts yet */
	outb_p(ctlbits, QIC_CTL_PORT);
	outb_p(0, AR_RESET_DMA_PORT);	/* dummy write to reset DMA */
#else
# error No valid interface card specified
#endif
} /* ifc_init */


static void report_exception(unsigned n)
{
	if (n >= NR_OF_EXC) { tpqputs("Oops -- report_exception"); n = 0; }
	printk(TPQIC_NAME ": sense: %s\n", exception_list[n].msg);
} /* report_exception */


/* Try to map the drive-exception bits `s' to a predefined "exception number",
 * by comparing the significant exception bits for each entry in the
 * exception table (`exception_list[]').
 * It is assumed that s!=0.
 */
static int decode_exception_nr(short s)	/* s must be short, because of sign-extension */
{
	int i;

	for (i=1; i<NR_OF_EXC; i++) {
		if ((s & exception_list[i].mask)==exception_list[i].code)
			return i;
	}
	tpqputs("decode_exception_nr: exception not recognized");
	return 0;
} /* decode_exception_nr */


#ifdef OBSOLETE
/* There are exactly 14 possible exceptions, as defined in QIC-02 rev F.
 * Some are FATAL, some aren't. Currently all exceptions are treated as fatal.
 * Especially 6 and 14 should not abort the transfer. RSN...
 * Should probably let sense() figure out the exception number using the code
 * below, and just report the error based on the number here, returning a code
 * for FATAL/CONTINUABLE.
 */
static void report_error(int s)
{
	short n = -1;

	if (s & TP_ST1) {
		if (s & TP_ILL)		/* 12: Illegal command. FATAL */
			n = 12;
		if (s & TP_POR)		/* 13: Reset occurred. FATAL */
			n = 13;
	} 
	else if (s & TP_ST0) {
		if (s & TP_EOR) {	/* extra: 15: End of Recorded Media. CONTINUABLE */
			n = 15;
			/********** should set flag here **********/
		}
		else if (s & TP_EOM)		/* 4: End Of Media. CONTINUABLE */
			n = 4;
		else if (s & TP_USL)		/* 2: Drive not online. FATAL */
			n = 2;
		else if (s & TP_CNI) {	/* 1: Cartridge not in place. FATAL */
			n = 1;
			need_rewind = YES;
			status_eof_detected = NO;
			status_eom_detected = NO;
		}
		else if (s & TP_UDA) {
			if (s & TP_BNL) {
				if (s & TP_NDT) {
					if (s & TP_BOM)		/* 9: Read error. No data detected & EOM. CONTINUABLE */
						n = 9;
					else if (s & TP_EOM)	/* 10: Read error. No data detected & BOM. CONTINUABLE */
						n = 10;
					else			/* 8: Read error. No data detected. CONTINUABLE */
						n = 8;
				} else { /* 7: Read error. Cannot recover block, filler substituted. CONTINUABLE */
					tpqputs("[Bad block -- filler data transferred.]");
					n = 7;
				}
			}
			else {
				if (s & TP_EOM)	/* 5: Read or Write error. Rewind tape. FATAL */
					n = 5;
				else {		/* 6: Read error. Bad block transferred. CONTINUABLE */
					/* block is bad, but transfer may continue.
					 * This is why some people prefer not to
					 * use compression on backups...
					 */
					tpqputs("[CRC failed!]");
					n = 6;
				}
			}
		}
		else if (s & TP_FIL) {
			if (s & TP_MBD) {	/* 14: Marginal block detected. CONTINUABLE */
				tpqputs("[Marginal block]");
				n = 14;
			} else			/* 11: File mark detected. CONTINUABLE */
				n = 11;
		}
		else if (s & TP_WRP)		/* 3: Write protected cartridge. FATAL */
			n = 3;
	}
	if (n >= 0)
 		sensemsg(n);
} /* report_error */
#endif


/* perform appropriate action for certain exceptions */
static void handle_exception(int exnr, int exbits)
{
	if (exnr==EXC_NCART) {
		/* Cartridge was changed. Redo sense().
		 * EXC_NCART should be handled in open().
		 * It is not permitted to remove the tape while
		 * the tape driver has open files. 
		 */
		need_rewind = YES;
		status_eof_detected = NO;
		status_eom_detected = NO;
	}
	else if (exnr==EXC_XFILLER)
		tpqputs("[Bad block -- filler data transferred.]");
	else if (exnr==EXC_XBAD)
		tpqputs("[CRC failed!]");
	else if (exnr==EXC_MARGINAL) {
		/* A marginal block behaves much like a FM.
		 * User may continue reading, if desired.
		 */
		tpqputs("[Marginal block]");
		doing_read = NO;
	} else if (exnr==EXC_FM)
		doing_read = NO;
} /* handle_exception */


static inline int is_exception(void)
{
	return (inb(QIC_STAT_PORT) & QIC_STAT_EXCEPTION) == 0;
} /* is_exception */


/* Reset the tape drive and controller.
 * When reset fails, it marks  the drive as dead and all
 * requests (except reset) are to be ignored (ENXIO).
 */
static int tape_reset(int verbose)
{
	ifc_init();				/* reset interface card */

	outb_p(ctlbits | QIC_CTL_RESET, QIC_CTL_PORT);	/* assert reset */

	/* Next, we need to wait >=25 usec. */
	udelay(30);

	/* after reset, we will be at BOT (modulo an automatic rewind) */
	status_eof_detected = NO;
	status_eom_detected = NO;
	status_cmd_pending = 0;
	need_rewind = YES;
	doing_read = doing_write = NO;
	ioctl_status.mt_fileno = ioctl_status.mt_blkno = 0;

	outb_p(ctlbits & ~QIC_CTL_RESET, QIC_CTL_PORT);	/* de-assert reset */
	/* KLUDGE FOR G++ BUG */
	{ int stat = inb_p(QIC_STAT_PORT);
	  status_dead = ((stat & QIC_STAT_RESETMASK) != QIC_STAT_RESETVAL); }
	/* if successful, inb(STAT) returned RESETVAL */
	if (status_dead)
		printk(TPQIC_NAME ": reset failed!\n");
	else if (verbose)
		printk(TPQIC_NAME ": reset successful\n");

	return (status_dead)? TE_DEAD : TE_OK;
} /* tape_reset */



/* Notify tape drive of a new command. It only waits for the
 * command to be accepted, not for the actual command to complete.
 *
 * Before calling this routine, QIC_CMD_PORT must have been loaded
 * with the command to be executed.
 * After this routine, the exception bit must be checked.
 * This routine is also used by rdstatus(), so in that case, any exception
 * must be ignored (`ignore_ex' flag).
 */
static int notify_cmd(char cmd, short ignore_ex)
{
	int i;

	outb_p(cmd, QIC_CMD_PORT);    /* output the command */

	/* wait 1 usec before asserting /REQUEST */
	udelay(1);

	if ((!ignore_ex) && is_exception()) {
		tpqputs("*** exception detected in notify_cmd");
		/** force a reset here **/
		if (tape_reset(1)==TE_DEAD)
			return TE_DEAD;
		if (is_exception()) {
			tpqputs("exception persists after reset.");
			tpqputs(" ^ exception ignored.");
		}
	}

	outb_p(ctlbits | QIC_CTL_REQUEST, QIC_CTL_PORT);  /* set request bit */
	i = TAPE_NOTIFY_TIMEOUT;
	/* The specs say this takes about 500 usec, but there is no upper limit!
	 * If the drive were busy retensioning or something like that,
	 * it could be *much* longer!
	 */
	while ((inb_p(QIC_STAT_PORT) & QIC_STAT_READY) && (--i>0))
		/*skip*/;			  /* wait for ready */
	if (i==0) {
		tpqputs("timed out waiting for ready in notify_cmd");
		status_dead = YES;
		return TE_TIM;
	}

	outb_p(ctlbits & ~QIC_CTL_REQUEST, QIC_CTL_PORT); /* reset request bit */
	i = TAPE_NOTIFY_TIMEOUT;
	/* according to the specs this one should never time-out */
	while (((inb_p(QIC_STAT_PORT) & QIC_STAT_READY) == 0) && (--i>0))
		/*skip*/;			  /* wait for not ready */
	if (i==0) {
		tpqputs("timed out waiting for !ready in notify_cmd");
		status_dead = YES;
		return TE_TIM;
	}
	/* command accepted */
	return TE_OK;
} /* notify_cmd */



/* Wait for a command to complete, with timeout */
static int wait_for_ready(time_t timeout)
{
	int stat;
	time_t spin_t;

	/* Wait for ready or exception, without driving the loadavg up too much.
	 * In most cases, the tape drive already has READY asserted,
	 * so optimize for that case.
	 *
	 * First, busy wait a few usec:
	 */
	spin_t = 50;
	while (((stat = inb_p(QIC_STAT_PORT) & QIC_STAT_MASK) == QIC_STAT_MASK) && (--spin_t>0))
		/*SKIP*/;
	if ((stat & QIC_STAT_READY) == 0)
		return TE_OK;			/* covers 99.99% of all calls */

	/* Then use schedule() a few times */
	spin_t = 3;	/* max 0.03 sec busy waiting */
	if (spin_t > timeout)
		spin_t = timeout;
	timeout -= spin_t;
	spin_t += jiffies;

	while (((stat = inb_p(QIC_STAT_PORT) & QIC_STAT_MASK) == QIC_STAT_MASK) && (jiffies<spin_t))
		schedule();		/* don't waste all the CPU time */
	if ((stat & QIC_STAT_READY) == 0)
		return TE_OK;

	/* If we reach this point, we probably need to wait much longer, or
	 * an exception occurred. Either case is not very time-critical.
	 * Check the status port only a few times every second.
	 * A interval of less than 0.10 sec will not be noticed by the user,
	 * more than 0.40 sec may give noticeable delays.
	 */
	spin_t += timeout;
	TPQDEB({printk("wait_for_ready: additional timeout: %d\n", spin_t);})

		/* not ready and no exception && timeout not expired yet */
	while (((stat = inb_p(QIC_STAT_PORT) & QIC_STAT_MASK) == QIC_STAT_MASK) && (jiffies<spin_t)) {
		/* be `nice` to other processes on long operations... */
		current->timeout = jiffies + 30;	/* nap 0.30 sec between checks, */
		current->state = TASK_INTERRUPTIBLE;
		schedule();		 /* but could be woken up earlier by signals... */
	}

	/* don't use jiffies for this test because it may have changed by now */
	if ((stat & QIC_STAT_MASK) == QIC_STAT_MASK) {
		tpqputs("wait_for_ready() timed out");
		return TE_TIM;
	}

	if ((stat & QIC_STAT_EXCEPTION) == 0) {
		tpqputs("exception detected after waiting_for_ready");
		return TE_EX;
	} else {
		return TE_OK;
	}
} /* wait_for_ready */


/* Send some data to the drive */
static int send_qic02_data(char sb[], unsigned size, int ignore_ex)
{
	int i, stat;

	for (i=0; i<size; i++) {

		stat = wait_for_ready(TIM_S);
		if (stat != TE_OK)
			return stat;

		stat = notify_cmd(sb[i], ignore_ex);
		if (stat != TE_OK)
			return stat;
	}
	return TE_OK;
	
} /* send_qic02_data */


/* Send a QIC-02 command (`cmd') to the tape drive, with
 * a time-out (`timeout').
 * This one is also used by tp_sense(), so we must have
 * a flag to disable exception checking (`ignore_ex'). 
 *
 * On entry, the controller is supposed to be READY.
 */
static int send_qic02_cmd(int cmd, time_t timeout, int ignore_ex)
{
	int stat;

	stat = inb_p(QIC_STAT_PORT);
	if ((stat & QIC_STAT_EXCEPTION) == 0) {	/* if exception */
		tpqputs("send_qic02_cmd: Exception!");
		return TE_EX;
	}
	if (stat & QIC_STAT_READY) {			/* if not ready */
		tpqputs("send_qic02_cmd: not Ready!");
		return TE_ERR;
	}

	/* assert(ready & !exception) */

	/* Remember current command for later re-use with dma transfers.
	 * (For reading/writing multiple blocks.)
	 */
	status_cmd_pending = cmd;

	stat = notify_cmd(cmd, ignore_ex); /* tell drive new command was loaded, */
					   /* inherit exception check. */
	if (cmd == QCMDV_SEEK_BLK) {
		/* This one needs to send 3 more bytes, MSB first */
		stat = send_qic02_data(seek_addr_buf, sizeof(seek_addr_buf), ignore_ex);
	}
	if (stat != TE_OK) {
		tpqputs("send_qic02_cmd failed");
	}
	return stat;
} /* send_qic02_cmd */



/* Get drive status. Assume drive is ready or has exception set.
 * (or will be in <1000 usec.)
 * Extra parameters added because of 'Read Extended Status 3' command.
 */
static int rdstatus(char *stp, unsigned size, char qcmd)
{
	int	s, n;
	char	*q = stp;

	/* Try to busy-wait a few (700) usec, after that de-schedule.
	 *
	 * The problem is, if we don't de-schedule, performance will
	 * drop to zero when the drive is not responding and if we
	 * de-schedule immediately, we waste a lot of time because a
	 * task switch is much longer than we usually have to wait here.
	 */
	n = 1000;	/* 500 is not enough on a 486/33 */
	while ((n>0) && ((inb_p(QIC_STAT_PORT) & QIC_STAT_MASK) == QIC_STAT_MASK))
		n--;  /* wait for ready or exception or timeout */
	if (n==0) {
		/* n (above) should be chosen such that on your machine
		 * you rarely ever see the message below, and it should
		 * be small enough to give reasonable response time.]
		 */
		tpqputs("waiting looong in rdstatus() -- drive dead?");
		while ((inb_p(QIC_STAT_PORT) & QIC_STAT_MASK) == QIC_STAT_MASK)
			schedule();
		tpqputs("finished waiting in rdstatus()");
	}

	(void) notify_cmd(qcmd, 1);			/* send read status command */
	/* ignore return code -- should always be ok, STAT may contain 
	 * exception flag from previous exception which we are trying to clear.
	 */

	if (TP_DIAGS(current_tape_dev))
		printk(TPQIC_NAME ": reading status bytes: ");

	for (q=stp; q<stp+size; q++)
	{
		do s = inb_p(QIC_STAT_PORT);
		while ((s & QIC_STAT_MASK) == QIC_STAT_MASK);	/* wait for ready or exception */

		if ((s & QIC_STAT_EXCEPTION) == 0) {		/* if exception */
			tpqputs("rdstatus: exception error");
			ioctl_status.mt_erreg = 0;		/* dunno... */
			return TE_NS;				/* error, shouldn't happen... */
		}

		*q = inb_p(QIC_DATA_PORT);			/* read status byte */

		if (TP_DIAGS(current_tape_dev))
			printk("[%1d]=0x%x  ", q-stp, (unsigned) (*q) & 0xff);

		outb_p(ctlbits | QIC_CTL_REQUEST, QIC_CTL_PORT);	/* set request */

		while ((inb_p(QIC_STAT_PORT) & QIC_STAT_READY) == 0);	/* wait for not ready */

		udelay(22);	/* delay >20 usec */

		outb_p(ctlbits & ~QIC_CTL_REQUEST, QIC_CTL_PORT);	/* un-set request */

	}

	/* Specs say we should wait for READY here.
	 * My drive doesn't seem to need it here yet, but others do?
	 */
	while (inb_p(QIC_STAT_PORT) & QIC_STAT_READY)
		/*skip*/;			  /* wait for ready */

	if (TP_DIAGS(current_tape_dev))
		printk("\n");

	return TE_OK;
} /* rdstatus */



/* Get standard status (6 bytes).
 * The `.dec' and `.urc' fields are in MSB-first byte-order,
 * so they have to be swapped first.
 */
static int get_status(char *stp)
{
	int stat = rdstatus(stp, TPSTATSIZE, QCMD_RD_STAT);
#if defined(i386) || defined(i486)
	byte_swap_w(&tperror.dec);
	byte_swap_w(&tperror.urc);
#else
	/* should probably swap status bytes #definition */
#endif
	return stat;
} /* get_status */


#if 0
/* This fails for my Wangtek drive */
/* get "Extended Status Register 3" (64 bytes)
 *
 * If the meaning of the returned bytes were known, the MT_TYPE
 * identifier could be used to decode them, since they are
 * "vendor unique". :-(
 */
static int get_ext_status3(void)
{
	char vus[64];	/* vendor unique status */
	int stat, i;

	tpqputs("Attempting to read Extended Status 3...");
	stat = rdstatus(vus, sizeof(vus), QCMD_RD_STAT_X3);
	if (stat != TE_OK)
		return stat;

	tpqputs("Returned status bytes:");
	for (i=0; i<sizeof(vus); i++) {
		if ( i % 8 == 0 )
			printk("\n" TPQIC_NAME ": %2d:");
		printk(" %2x", vus[i] & 0xff);
	}
	printk("\n");

	return TE_OK;
} /* get_ext_status3 */
#endif


/* Read drive status and set generic status too.
 * NOTE: Once we do a tp_sense(), read/write transfers are killed.
 */
static int tp_sense(int ignore)
{
	unsigned err = 0, exnr = 0, gs = 0;
	static void finish_rw(int cmd);

	printk(TPQIC_NAME ": tp_sense(ignore=0x%x) enter\n", ignore);

	/* sense() is not allowed during a read or write cycle */
	if (doing_write == YES)
		tpqputs("Warning: File Mark inserted because of sense() request");
	/* The extra test is to avoid calling finish_rw during booting */
	if ((doing_read!=NO) || (doing_write!=NO))
		finish_rw(QCMD_RD_STAT);

	if (get_status((char *) &tperror) != TE_OK) {
		tpqputs("tp_sense: could not read tape drive status");
		return TE_ERR;
	}

	err = tperror.exs;	/* get exception status bits */
	if (err & (TP_ST0|TP_ST1))
		printk(TPQIC_NAME ": tp_sense: status: %x, error count: %d, underruns: %d\n",
			tperror.exs, tperror.dec, tperror.urc);
	else
		printk(TPQIC_NAME ": tp_sense: no errors at all, soft error count: %d, underruns: %d\n",
			tperror.dec, tperror.urc);

	/* Set generic status. HP-UX defines these, but some extra would 
	 * be useful. Problem is to remain compatible. [Do we want to be
	 * compatible??]
	 */
	if (err & TP_ST0) {
		if (err & TP_CNI)		/* no cartridge */
			gs |= GMT_DR_OPEN(-1);
		if (status_dead == NO)
			gs |= GMT_ONLINE(-1);	/* always online */
		if (err & TP_USL)		/* not online */
			gs &= ~GMT_ONLINE(-1);
		if (err & TP_WRP)
			gs |= GMT_WR_PROT(-1);
		if (err & TP_EOM) {		/* end of media */
			gs |= GMT_EOT(-1);	/* not sure this is correct for writes */
			status_eom_detected = YES;
			/* I don't know whether drive always reports EOF at or before EOM. */
			status_eof_detected = YES;
		}
		/** if (err & TP_UDA) "Unrecoverable data error" **/
		/** if (err & TP_BNL) "Bad block not located" **/
		if (err & TP_FIL) {
			gs |= GMT_EOF(-1);
			status_eof_detected = YES;
		}
	}
	if (err & TP_ST1) {
		/** if (err & TP_ILL) "Illegal command" **/
		/** if (err & TP_NDT) "No data detected" **/
		/** if (err & TP_MBD) "Marginal block detected" **/
		if (err & TP_BOM)
			gs |= GMT_BOT(-1);	/* beginning of tape */
	}
	ioctl_status.mt_gstat = gs;
	ioctl_status.mt_dsreg = tperror.exs;	/* "drive status" */
	ioctl_status.mt_erreg = tperror.dec;	/* "sense key error" */

	if (err!=0) {
		exnr = decode_exception_nr(err);
		handle_exception(exnr, err);		/* update driver state wrt drive status */
		report_exception(exnr);
	}
	err &= ~ignore;		/* mask unwanted errors -- not the correct way, use exception nrs?? */
	if (((err & TP_ST0) && (err & REPORT_ERR0)) ||
	    ((err & TP_ST1) && (err & REPORT_ERR1)))
		return TE_ERR;
	return TE_OK;
} /* tp_sense */



/* Wait for a wind or rewind operation to finish or
 * to time-out. (May take very long).
 */
static int wait_for_rewind(time_t timeout)
{
	int stat;

	stat = inb(QIC_STAT_PORT) & QIC_STAT_MASK;
	printk(TPQIC_NAME ": Waiting for (re-)wind to finish: stat=0x%x\n", stat);

	stat = wait_for_ready(timeout);

	if (stat != TE_OK) {
			tpqputs("(re-) winding failed\n");
	}
	return stat;
} /* wait_for_rewind */



/* Perform a full QIC02 command, and wait for completion,
 * check status when done. Complain about exceptions.
 *
 * This function should return an OS error code when
 * something goes wrong, 0 otherwise.
 */
static int ll_do_qic_cmd(int cmd, time_t timeout)
{
	int stat;

	if (status_dead) {
		tpqputs("Drive is dead. Do a `mt reset`.");
		return -ENXIO;			/* User should do an MTRESET. */
	}

	stat = wait_for_ready(timeout);		/* wait for ready or exception */
	if (stat == TE_EX) {
		if (tp_sense(TP_WRP|TP_BOM|TP_EOM|TP_FIL)!=TE_OK)
			return -EIO;
		/* else nothing to worry about, I hope */
		stat = TE_OK;
	}
	if (stat != TE_OK) {
		printk(TPQIC_NAME ": ll_do_qic_cmd(%x, %d) failed\n", cmd, timeout);
		return -EIO;
	}


#if OBSOLETE
	/* wait for ready since it may not be active immediately after reading status */
	while ((inb_p(QIC_STAT_PORT) & QIC_STAT_READY) != 0);
#endif

	stat = send_qic02_cmd(cmd, timeout, 0);	/* (checks for exceptions) */

	if (cmd==QCMD_RD_FM) {
		status_eof_detected = NO;
		ioctl_status.mt_fileno++;
		/* Should update block count as well, but can't.
		 * Can do a `read address' for some drives, when MTNOP is done.
		 */
	} else if (cmd==QCMD_WRT_FM) {
		status_eof_detected = NO;
		ioctl_status.mt_fileno++;
	} else if ((cmd==QCMD_REWIND) || (cmd==QCMD_ERASE) || (cmd==QCMD_RETEN)) {
		status_eof_detected = NO;
		status_eom_detected = NO;
		status_eot_detected = NO;
		need_rewind = NO;
		ioctl_status.mt_fileno = ioctl_status.mt_blkno = 0;
		extra_blocks_left = BLOCKS_BEYOND_EW;
		return_write_eof = NO;
		return_read_eof = NO;
		reported_read_eof = NO;
		reported_write_eof = NO;
	}
	/* sense() will set eof/eom as required */
	if (stat==TE_EX) {
		if (tp_sense(TP_WRP|TP_BOM|TP_EOM|TP_FIL)!=TE_OK) {
			printk(TPQIC_NAME ": Exception persist in ll_do_qic_cmd[1](%x, %d)", cmd, timeout);
			status_dead = YES;
			return -ENXIO;
			/* if rdstatus fails too, we're in trouble */
		}
	}
	else if (stat!=TE_OK) {
		printk(TPQIC_NAME ": ll_do_qic_cmd: send_qic02_cmd failed, stat = 0x%x\n", stat);
		return -EIO;	/*** -EIO is probably not always appropriate */
	}


	if (timeout == TIM_R)
		stat = wait_for_rewind(timeout);
	else
		stat = wait_for_ready(timeout);

	if (stat==TE_EX) {
		if (tp_sense((cmd==QCMD_SEEK_EOD ?		/*****************************/
		      TP_EOR|TP_NDT|TP_UDA|TP_BNL|TP_WRP|TP_BOM|TP_EOM|TP_FIL :
		      TP_WRP|TP_BOM|TP_EOM|TP_FIL))!=TE_OK) {
			printk(TPQIC_NAME ": Exception persist in ll_do_qic_cmd[2](%x, %d)\n", cmd, timeout);
			if (cmd!=QCMD_RD_FM)
				status_dead = YES;
			return -ENXIO;
			/* if rdstatus fails too, we're in trouble */
		}
	}
	else if (stat!=TE_OK) {
		printk(TPQIC_NAME ": ll_do_qic_cmd %x: wait failed, stat == 0x%x\n", cmd, stat);
		return -EIO;
	}
	return 0;
} /* ll_do_qic_cmd */


/* 
 * Problem: What to do when the user cancels a read/write operation
 * in-progress?
 *
 * "Deactivating ONLINE during a READ also causes the"
 * "tape to be rewound to BOT." Ditto for WRITEs, except
 * a FM is written first. "The host may alternatively terminate
 * the READ/WRITE command by issuing a RFM/WFM command."
 *
 * For READs:
 * Neither option will leave the tape positioned where it was.
 * Another (better?) solution is to terminate the READ by two
 * subsequent sense() operations, the first to stop the current
 * READ cycle, the second to clear the `Illegal command' exception,
 * because the QIC-02 specs didn't anticipate this. This is
 * delayed until actually needed, so a tar listing can be aborted
 * by the user and continued later.
 * If anybody has a better solution, let me know! [Also, let me
 * know if your drive (mine is a Wangtek5150EQ) does not accept
 * this sequence for canceling the read-cycle.]
 *
 * For WRITEs it's simple: Just do a WRITE_FM, leaving the tape
 * positioned after the FM.
 */

static void terminate_read(int cmd)
{
	if (doing_read == YES) {
		doing_read = NO;
		if (cmd != QCMD_RD_FM) {
			/* if the command is a RFM, there is no need to do this
			 * because a RFM will legally terminate the read-cycle.
			 */
			tpqputs("terminating pending read-cycle");
			if (tp_sense(TP_FIL|TP_EOM|TP_WRP) != TE_OK) {
				tpqputs("finish_rw[read1]: ignore the 2 lines above");
				if (is_exception()) {
					if (tp_sense(TP_ILL|TP_FIL|TP_EOM|TP_WRP) != TE_OK)
						tpqputs("finish_rw[read2]: read cycle error");
				}
			}
		}
	}
} /* terminate_read */


static void terminate_write(int cmd)
{
	int stat;

	if (doing_write == YES) {
		doing_write = NO;
		/* Finish writing by appending a FileMark at the end. */
		if (cmd != QCMD_WRT_FM) {
			/* finish off write cycle */
			stat = ll_do_qic_cmd(QCMD_WRT_FM, TIM_M);
			if (stat != TE_OK)
				tpqputs("Couldn't finish write cycle properly");
			(void) tp_sense(0);
		}
		/* If there is an EOF token waiting to be returned to
		 * the (writing) application, discard it now.
		 * We could be at EOT, so don't reset return_write_eof.
		 */
		reported_write_eof=YES;
	}
} /* terminate_write */


/* terminate read or write cycle because of command `cmd' */
static void finish_rw(int cmd)
{
	if (wait_for_ready(TIM_S) != TE_OK) {
		tpqputs("error: drive not ready in finish_rw() !");
		return;
	}
	terminate_read(cmd);
	terminate_write(cmd);
} /* finish_rw */


/* Perform a QIC command through ll_do_qic_cmd().
 * If necessary, rewind the tape first.
 * Return an OS error code if something goes wrong, 0 if all is well.
 */
static int do_qic_cmd(int cmd, time_t timeout)
{
	int stat;


	finish_rw(cmd);

	if (need_rewind) {
		tpqputs("Rewinding tape...");
		stat = ll_do_qic_cmd(QCMD_REWIND, TIM_R);
		if (stat != 0) {
			printk(TPQIC_NAME ": rewind failed in do_qic_cmd(). stat=0x%2x", stat);
			return stat;
		}
		need_rewind = NO;
		if (cmd==QCMD_REWIND)	/* don't wind beyond BOT ;-) */
			return 0;
	}

	return ll_do_qic_cmd(cmd, timeout);
} /* do_qic_cmd */


/* Not all ioctls are supported for all drives. Some rely on
 * optional QIC-02 commands. Check tpqic02.h for configuration.
 * Some of these commands may require ONLINE to be active.
 */
static int do_ioctl_cmd(int cmd)
{
	int stat;

	/* It is not permitted to read or wind the tape after bytes have
	 * been written. It is not permitted to write the tape while in
	 * read mode.
	 * We try to be kind and allow reading again after writing a FM...
	 */

	switch (cmd) {
		case MTRESET:
			/* reset verbose */
			return (tape_reset(1)==TE_OK)? 0 : -EIO;

		case MTFSF:
			tpqputs("MTFSF forward searching filemark");
			if ((mode_access==WRITE) && status_bytes_wr)
				return -EACCES;
			return do_qic_cmd(QCMD_RD_FM, TIM_F);

		case MTBSF:
#ifdef TP_HAVE_BSF
			tpqputs("MTBSF backward searching filemark -- optional command");
			if ((mode_access==WRITE) && status_bytes_wr)
				return -EACCES;
			stat = do_qic_cmd(QCMD_RD_FM_BCK, TIM_F);
#else
			tpqputs("MTBSF not supported");
			stat = -ENXIO;
#endif
			status_eom_detected = status_eof_detected = NO;
			return stat;

		case MTFSR:
#ifdef TP_HAVE_FSR	/* This is an optional QIC-02 command */
			tpqputs("MTFSR forward space record");
			if ((mode_access==WRITE) && status_bytes_wr)
				return -EACCES;
			stat = do_qic_cmd(QCMD_SPACE_FWD, TIM_F);
#else
			/**** fake it by doing a read data block command? ******/
			tpqputs("MTFSR not supported");
			stat = -ENXIO;
#endif
			return stat;

		case MTBSR:
#ifdef TP_HAVE_BSR	/* This is an optional QIC-02 command */
			/* we need this for appending files with GNU tar!! */
			tpqputs("MTFSR backward space record");
			if ((mode_access==WRITE) && status_bytes_wr)
				return -EACCES;
			stat = do_qic_cmd(QCMD_SPACE_BCK, TIM_F);
#else
			tpqputs("MTBSR not supported");
			stat = -ENXIO;
#endif
			status_eom_detected = status_eof_detected = NO;
			return stat;

		case MTWEOF:
			tpqputs("MTWEOF write eof mark");
			/* Plain GNU mt(1) 2.2 uses read-only mode for writing FM. :-( */
			if (mode_access==READ)
				return -EACCES;

			/* allow tape movement after writing FM */
			status_bytes_rd = status_bytes_wr;	/* Kludge-O-Matic */
			status_bytes_wr = NO;
			return do_qic_cmd(QCMD_WRT_FM, TIM_M);
			/* not sure what to do with status_bytes when WFM should fail */

		case MTREW:
			tpqputs("MTREW rewinding tape");
			if ((mode_access==WRITE) && status_bytes_wr)
				return -EACCES;
			status_eom_detected = status_eof_detected = NO;
			return do_qic_cmd(QCMD_REWIND, TIM_R);

		case MTOFFL:
			tpqputs("MTOFFL rewinding & going offline"); /*---*/
			/******* What exactly are we supposed to do, to take it offline????
			 *****/
			/* Doing a drive select will clear (unlock) the current drive.
			 * But that requires support for multiple drives and locking.
			 */
			if ((mode_access==WRITE) && status_bytes_wr)
				return -EACCES;
			status_eom_detected = status_eof_detected = NO;
			/**** do rewind depending on minor bits??? ***/
			stat = do_qic_cmd(QCMD_REWIND, TIM_R);
			return stat;

		case MTNOP:
			tpqputs("MTNOP setting status only");
			/********** should do `read position' for drives that support it **********/
			return (tp_sense(-1)==TE_OK)? 0 : -EIO;	/**** check return codes ****/

		case MTRETEN:
			tpqputs("MTRETEN retension tape");
			if ((mode_access==WRITE) && status_bytes_wr)
				return -EACCES;
			status_eom_detected = status_eof_detected = NO;
			return do_qic_cmd(QCMD_RETEN, TIM_R);

		case MTBSFM:
			/* Think think is like MTBSF, except that
			 * we shouldn't skip the FM. Tricky.
			 * Maybe use RD_FM_BCK, then do a SPACE_FWD?
			 */
			tpqputs("MTBSFM not supported");
			if ((mode_access==WRITE) && status_bytes_wr)
				return -EACCES;
			return -ENXIO;

		case MTFSFM:
			/* I think this is like MTFSF, except that
			 * we shouldn't skip the FM. Tricky.
			 * Maybe use QCMD_RD_DATA until we get a TP_FIL exception?
			 * But then the FM will have been skipped...
			 * Maybe use RD_FM, then RD_FM_BCK, but not all
			 * drives will support that!
			 */
			tpqputs("MTFSFM not supported");
			if ((mode_access==WRITE) && status_bytes_wr)
				return -EACCES;
			return -ENXIO;

		case MTEOM:
			/* This should leave the tape ready for appending
			 * another file to the end, such that it would append
			 * after the last FM on tape.
			 */
			tpqputs("MTEOM search for End Of recorded Media");
			if ((mode_access==WRITE) && status_bytes_wr)
				return -EACCES;
#ifdef TP_HAVE_EOD
			/* Use faster seeking when possible.
			 * This requires the absence of data beyond the EOM.
			 */
# if TAPE_QIC02_DRIVE == MT_ISWT5150
			/* It seems that my drive does not always perform the
			 * SEEK_EOD correctly, unless it is preceded by a
			 * rewind command.
			 */
# if 0
			status_eom_detected = status_eof_detected = NO;
# endif
			stat = do_qic_cmd(QCMD_REWIND, TIM_R);
			if (stat)
				return stat;
			
# endif
			stat = do_qic_cmd(QCMD_SEEK_EOD, TIM_F);
			/* After a successful seek, TP_EOR should be returned */
#else
			/* else just seek until the drive returns exception "No Data" */
			stat = 0;
			while ((stat==0) && (!status_eom_detected)) {
				stat = do_qic_cmd(QCMD_RD_FM, TIM_F); /***** should use MTFSFM here???? ******/
			}
			if (tperror.exs & TP_NDT)
				return 0;
#endif
			return stat;

		case MTERASE:
			tpqputs("MTERASE -- ERASE TAPE !");
			if  ((tperror.exs & TP_ST0) && (tperror.exs & TP_WRP)) {
				tpqputs("Cartridge is write-protected.");
				return -EACCES;
			} else {
				time_t t = jiffies;
				/* give user a few seconds to pull out tape */
				while (jiffies - t < 3*HZ)
					schedule();
			}

			/* Plain GNU mt(1) 2.2 erases a tape in O_RDONLY. :-( */
			if (mode_access==READ) 
				return -EACCES;

			/* don't bother writing filemark */
			status_eom_detected = status_eof_detected = NO;
			return do_qic_cmd(QCMD_ERASE, TIM_R);


		case MTRAS1:
#ifdef TP_HAVE_RAS1
			tpqputs("MTRAS1: non-destructive self test");
			stat = do_qic_cmd(QCMD_SELF_TST1, TIM_R);
			if (stat != 0) {
				tpqputs("RAS1 failed");
				return stat;
			}
			return (tp_sense(0)==TE_OK)? 0 : -EIO; /* get_ext_status3(); */
#else
			tpqputs("RAS1 not supported");
			return -ENXIO;
#endif

		case MTRAS2:
#ifdef TP_HAVE_RAS2
			tpqputs("MTRAS2: destructive self test");
			stat = do_qic_cmd(QCMD_SELF_TST2, TIM_R);
			if (stat != 0) {
				tpqputs("RAS2 failed");
				return stat;
			}
			return (tp_sense(0)==TE_OK)? 0 : -EIO; /* get_ext_status3(); */
#else
			tpqputs("RAS2 not supported");
			return -ENXIO;
#endif

#ifdef TP_HAVE_SEEK
		case MTSEEK:
			tpqputs("MTSEEK seeking block");
			if ((mode_access==WRITE) && status_bytes_wr)
				return -EACCES;
			/* NOTE: address (24 bits) is in seek_addr_buf[] */
			return do_qic_cmd(QCMDV_SEEK_BLK, TIM_F);
#endif

		default:
			return -ENOTTY;
	}
} /* do_ioctl_cmd */


/* dma_transfer(): This routine is called for every 512 bytes to be read
 * from/written to the tape controller. Speed is important here!
 * (There must be enough time left for the hd controller!)
 * When other devices use DMA they must ensure they use un-interruptible
 * double byte accesses to the DMA controller. Floppy.c is ok.
 * Must have interrupts disabled when this function is invoked,
 * otherwise, the double-byte transfers to the DMA controller will not
 * be atomic. That could lead to nasty problems when they are interrupted
 * by other DMA interrupt-routines.
 *
 * This routine merely does the least possible to keep
 * the transfers going:
 *	- set the DMA count register for the next 512 bytes
 *	- adjust the DMA address and page registers
 *	- adjust the timeout
 *	- tell the tape controller to start transferring
 * We assume the dma address and mode are, and remain, valid.
 */ 
static inline void dma_transfer(void)
{

#if TAPE_QIC02_IFC == WANGTEK
	outb_p(WT_CTL_ONLINE, QIC_CTL_PORT);	/* back to normal */
#elif TAPE_QIC02_IFC == ARCHIVE
	outb_p(0, AR_RESET_DMA_PORT);
#endif

	clear_dma_ff(TAPE_QIC02_DMA);
	set_dma_mode(TAPE_QIC02_DMA, dma_mode);
	set_dma_addr(TAPE_QIC02_DMA, buffaddr+dma_bytes_done);	/* full address */
	set_dma_count(TAPE_QIC02_DMA, TAPE_BLKSIZE);

	/* start tape DMA controller */
#if TAPE_QIC02_IFC == WANGTEK
	outb_p(WT_CTL_DMA | WT_CTL_ONLINE, QIC_CTL_PORT); /* trigger DMA transfer */
#elif TAPE_QIC02_IFC == ARCHIVE
	outb_p(AR_CTL_IEN | AR_CTL_DNIEN, QIC_CTL_PORT);  /* enable interrupts again */
	outb_p(0, AR_START_DMA_PORT);			  /* start DMA transfer */
	/* In dma_end() AR_RESET_DMA_PORT is written too. */
#endif

	/* start computer DMA controller */
	enable_dma(TAPE_QIC02_DMA);
	/* block transfer should start now, jumping to the 
	 * interrupt routine when done or an exception was detected.
	 */
} /* dma_transfer */


/* start_dma() sets a DMA transfer up between the tape controller and
 * the kernel tape_qic02_buf buffer.
 * Normally bytes_todo==dma_bytes_done at the end of a DMA transfer. If not,
 * a filemark was read, or an attempt to write beyond the End Of Tape 
 * was made. [Or some other bad thing happened.]
 * Must do a sense() before returning error.
 */
static int start_dma(short mode, unsigned long bytes_todo)
/* assume 'bytes_todo'>0 */
{
	int stat;
	
	TPQPUTS("start_dma() enter");
	TPQDEB({printk(TPQIC_NAME ": doing_read==%d, doing_write==%d\n", doing_read, doing_write);})

	dma_bytes_done = 0;
	dma_bytes_todo = bytes_todo;
	status_error = NO;
	/* dma_mode!=0 indicates that the dma controller is in use */
	dma_mode = (mode == WRITE)? DMA_MODE_WRITE : DMA_MODE_READ;	

	/* Only give READ/WRITE DATA command to tape drive if we haven't
	 * done that already. Otherwise the drive will rewind to the beginning
	 * of the current file on tape. Any QIC command given other than
	 * R/W FM will break the read/write transfer cycle.
	 * do_qic_cmd() will terminate doing_{read,write}
	 */
	if ((doing_read == NO) && (doing_write == NO)) {
		/* First, we have to clear the status -- maybe remove TP_FIL???
		 */

#if 0
		/* Next dummy get status is to make sure CNI is valid,
                   since we're only just starting a read/write it doesn't
                   matter some exceptions are cleared by reading the status;
                   we're only interested in CNI and WRP. -Eddy */
		get_status((char *) &tperror);
#else
		/* TP_CNI should now be handled in open(). -Hennus */
#endif

		stat = tp_sense(((mode == WRITE)? 0 : TP_WRP) | TP_BOM | TP_FIL);
		if (stat != TE_OK)
			return stat;

#if OBSOLETE
		/************* not needed iff rd_status() would wait for ready!!!!!! **********/
		if (wait_for_ready(TIM_S) != TE_OK) {	/*** not sure this is needed ***/
			tpqputs("wait_for_ready failed in start_dma");
			return -EIO;
		}
#endif

		/* Tell the controller the data direction */

		/* r/w, timeout medium, check exceptions, sets status_cmd_pending. */
		stat = send_qic02_cmd((mode == WRITE)? QCMD_WRT_DATA : QCMD_RD_DATA, TIM_M, 0);
		if (stat!=TE_OK) {
			printk(TPQIC_NAME ": start_dma: init %s failed\n",
				(mode == WRITE)? "write" : "read");
			(void) tp_sense(0);
			return stat;
		}

		/* Do this last, because sense() will clear the doing_{read,write}
		 * flags, causing trouble next time around.
		 */
		if (wait_for_ready(TIM_M) != TE_OK)
			return -EIO;
		switch (mode) {
			case READ:
				doing_read = YES;
				break;
			case WRITE:
				doing_write = YES;
				break;
			default:
				printk(TPQIC_NAME ": requested unknown mode %d\n", mode);
				panic(TPQIC_NAME ": invalid mode in start_dma()");
		}

	} else if (is_exception()) {
		/* This is for Archive drives, to handle reads with 0 bytes
		 * left for the last read request.
		 *
		 * ******** this also affects EOF/EOT handling! ************
		 */
		tpqputs("detected exception in start_dma() while transfer in progress");
		status_error = YES;
		return TE_END;
	}


	status_expect_int = YES;

	/* This assumes tape is already positioned, but these
	 * semi-'intelligent' drives are unpredictable...
	 */
	TIMERON(TIM_M*2);

	/* initiate first data block read from/write to the tape controller */

	cli();
	dma_transfer();
	sti();

	TPQPUTS("start_dma() end");
	return TE_OK;
} /* start_dma */


/* This cleans up after the dma transfer has completed
 * (or failed). If an exception occurred, a sense()
 * must be done. If the exception was caused by a FM,
 * sense() will set `status_eof_detected' and
 * `status_eom_detected', as required.
 */
static void end_dma(unsigned long * bytes_done)
{
	int stat = TE_OK;

	TIMEROFF;

	TPQPUTS("end_dma() enter");

	disable_dma(TAPE_QIC02_DMA);
	clear_dma_ff(TAPE_QIC02_DMA);

#if TAPE_QIC02_IFC == WANGTEK
	outb_p(WT_CTL_ONLINE, QIC_CTL_PORT);	/* back to normal */
#elif TAPE_QIC02_IFC == ARCHIVE
	outb_p(0, AR_RESET_DMA_PORT);
#endif

	stat = wait_for_ready(TIM_M);
	if (status_error || (stat!=TE_OK)) {
		tpqputs("DMA transfer exception");
		stat = tp_sense((dma_mode==READ)? TP_WRP : 0);
		/* no return here -- got to clean up first! */
	}
	/* take the tape controller offline */

	/* finish off DMA stuff */


	dma_mode = 0;
	/* Note: The drive is left on-line, ready for the next
	 * data transfer.
	 * If the next command to the drive does not continue
	 * the pending cycle, it must do 2 sense()s first.
	 */

	*bytes_done = dma_bytes_done;
	status_expect_int = NO;
	ioctl_status.mt_blkno += (dma_bytes_done / TAPE_BLKSIZE);

	TPQPUTS("end_dma() exit");
	/*** could return stat here ***/
} /* end_dma */

/*********** Below are the (public) OS-interface procedures ***********/


/* tape_qic02_times_out() is called when a DMA transfer doesn't complete
 * quickly enough. Usually this means there is something seriously wrong
 * with the hardware/software, but it could just be that the controller
 * has decided to do a long rewind, just when I didn't expect it.
 * Just try again.
 */
static void tape_qic02_times_out(void)
{
	printk("time-out in %s driver\n", TPQIC_NAME);
	if ((status_cmd_pending>0) || dma_mode) {
		/* takes tooo long, shut it down */
		status_dead = YES;
		status_cmd_pending = 0;
		status_timer_on = NO;
		status_expect_int = NO;
		status_error = YES;
		if (dma_mode) {
			dma_mode = 0;	/* signal end to read/write routine */
			wake_up(&tape_qic02_transfer);
		}
	}
} /* tape_qic02_times_out */

/*
 * Interrupt handling:
 *
 * 1) Interrupt is generated iff at the end of 
 *    a 512-DMA-block transfer.
 * 2) EXCEPTION is not raised unless something 
 *    is wrong or EOT/FM is detected.
 * 3) FM EXCEPTION is set *after* the last byte has
 *    been transferred by DMA. By the time the interrupt
 *    is handled, the EXCEPTION may already be set.
 *
 * So,
 * 1) On EXCEPTION, assume data has been transferred, so
 *    continue as usual, but set a flag to indicate the
 *    exception was detected.
 *    Do a sense status when the flag is found set.
 * 2) Do not attempt to continue a transfer after an exception.
 *    [??? What about marginal blocks???????]
 */


/* tape_qic02_interrupt() is called when the tape controller completes 
 * a DMA transfer.
 * We are not allowed to sleep here! 
 *
 * Check if the transfer was successful, check if we need to transfer
 * more. If the buffer contains enough data/is empty enough, signal the
 * read/write() thread to copy to/from user space.
 * When we are finished, set flags to indicate end, disable timer.
 * NOTE: This *must* be fast! 
 */
static void tape_qic02_interrupt(int unused)
{
	int stat, r, i;

	TIMEROFF;

	if (status_expect_int) {
		if (TP_DIAGS(current_tape_dev))
			printk("@");
	
		stat = inb(QIC_STAT_PORT);	/* Knock, knock */
#if TAPE_QIC02_IFC == ARCHIVE			/* "Who's there?" */
		if (((stat & (AR_STAT_DMADONE)) == 0) &&
                      ((stat & (QIC_STAT_EXCEPTION)) != 0)) {
			TIMERCONT;
			return;			/* "Linux with IRQ sharing" */
		}
#endif
		if ((stat & QIC_STAT_EXCEPTION) == 0) {	/* exception occurred */
			/* Possible causes for an exception during a transfer:
			 * 	- during a write-cycle: end of tape (EW) hole detected.
			 *	- during a read-cycle: filemark or EOD detected.
			 *	- something went wrong
			 * So don't continue with the next block.
			 */
			tpqputs("isr: exception on tape controller");
			printk("      status %02x\n", stat);
			status_error = TE_EX;

			dma_bytes_done += TAPE_BLKSIZE;

			dma_mode = 0;	/* wake up rw() */
			status_expect_int = NO;
			wake_up(&tape_qic02_transfer);
			return;
		}
		/* return if tape controller not ready, or
		 * if dma channel hasn't finished last byte yet.
		 */
		r = 0;
/* Skip next ready check for Archive controller because
 * it may be busy reading ahead. Weird. --hhb
 */
#if TAPE_QIC02_IFC != ARCHIVE		/* I think this is a drive-dependency, not IFC -- hhb */
		if (stat & QIC_STAT_READY) {		/* not ready */
			tpqputs("isr: ? Tape controller not ready");
			r = 1;
		}
#endif

		if ( (i = get_dma_residue(TAPE_QIC02_DMA)) != 0 ) {
			printk(TPQIC_NAME ": dma_residue == %x !!!\n", i);
			r = 1;	/* big trouble, but can't do much about it... */
		}

		if (r) 
			return;

		/* finish DMA cycle */

		/* no errors detected, continue */
		dma_bytes_done += TAPE_BLKSIZE;
		if (dma_bytes_done >= dma_bytes_todo) {
			/* finished! Wakeup rw() */
			dma_mode = 0;
			status_expect_int = NO;
			TPQPUTS("isr: dma_bytes_done");
			wake_up(&tape_qic02_transfer);
		} else {
			/* start next transfer, account for track-switching time */
			timer_table[TAPE_QIC02_TIMER].expires = jiffies + 6*HZ;
			dma_transfer();
		}
	} else {
		printk(TPQIC_NAME ": Unexpected interrupt, stat == %x\n",
		       inb(QIC_STAT_PORT));
	}
} /* tape_qic02_interrupt */


static int tape_qic02_lseek(struct inode * inode, struct file * file, off_t offset, int origin)
{
	return -EINVAL;	/* not supported */
} /* tape_qic02_lseek */


/* read/write routines:
 * This code copies between a kernel buffer and a user buffer. The 
 * actual data transfer is done using DMA and interrupts. Time-outs
 * are also used.
 *
 * When a filemark is read, we return '0 bytes read' and continue with the
 * next file after that.
 * When EOM is read, we return '0 bytes read' twice.
 * When the EOT marker is detected on writes, '0 bytes read' should be
 * returned twice. If user program does a MTNOP after that, 2 additional
 * blocks may be written.	------- FIXME: Implement this correctly  *************************************************
 *
 * Only read/writes in multiples of 512 bytes are accepted.
 * When no bytes are available, we sleep() until they are. The controller will
 * generate an interrupt, and we (should) get a wake_up() call.
 *
 * Simple buffering is used. User program should ensure that a large enough
 * buffer is used. Usually the drive does some buffering as well (something
 * like 4k or so).
 *
 * Scott S. Bertilson suggested to continue filling the user buffer, rather
 * than waste time on a context switch, when the kernel buffer fills up.
 */

/*
 * Problem: tar(1) doesn't always read the entire file. Sometimes the entire file
 * has been read, but the EOF token is never returned to tar(1), simply because
 * tar(1) knows it has already read all of the data it needs. So we must use
 * open/release to reset the `reported_read_eof' flag. If we don't, the next read
 * request would return the EOF flag for the previous file.
 */

static int tape_qic02_read(struct inode * inode, struct file * filp, char * buf, int count)
{
	int error;
	dev_t dev = inode->i_rdev;
	unsigned short flags = filp->f_flags;
	unsigned long bytes_todo, bytes_done, total_bytes_done = 0;
	int stat;

	if (TP_DIAGS(current_tape_dev))
		printk(TPQIC_NAME ": request READ, minor=%x, buf=%p, count=%x, pos=%x, flags=%x\n",
			MINOR(dev), buf, count, filp->f_pos, flags);

	if (count % TAPE_BLKSIZE) {	/* Only allow mod 512 bytes at a time. */
		tpqputs("Wrong block size");
		return -EINVAL;
	}

	/* Just assume everything is ok. Controller will scream if not. */

	if (status_bytes_wr)	/* Once written, no more reads, 'till after WFM. */
		return -EACCES;


	/* Make sure buffer is safe to write into. */
	error = verify_area(VERIFY_WRITE, buf, count);
	if (error) {
		printk(TPQIC_NAME ": read: verify_area(WRITE, %p, %x) failed\n", buf, count);
		return error;
	}

	/* This is rather ugly because it has to implement a finite state
	 * machine in order to handle the EOF situations properly.
	 */
	while (count>=0) {
		bytes_done = 0;
		/* see how much fits in the kernel buffer */
		bytes_todo = TPQBUF_SIZE;
		if (bytes_todo>count)
			bytes_todo = count;

		/* Must ensure that user program sees exactly one EOF token (==0) */
		if (return_read_eof==YES) {
			printk("read: return_read_eof==%d, reported_read_eof==%d, total_bytes_done==%d\n", return_read_eof, reported_read_eof, total_bytes_done);

			if (reported_read_eof==NO) {
				/* have not yet returned EOF to user program */
				if (total_bytes_done>0) {
					return total_bytes_done; /* next time return EOF */
				} else {
					reported_read_eof = YES; /* move on next time */
					return 0;		 /* return EOF */
				}				
			} else {
				/* Application program has already received EOF
				 * (above), now continue with next file on tape,
				 * if possible.
				 * When the FM is reached, EXCEPTION is set,
				 * causing a sense(). Subsequent read/writes will
				 * continue after the FM.
				 */
/*********** ?????????? this should check for (EOD|NDT), not EOM, 'cause we can read past EW: ************/
				if (status_eom_detected)
					/* If EOM, nothing left to read, so keep returning EOFs.
					 *** should probably set some flag to avoid clearing
					 *** status_eom_detected through ioctls or something
					 */
					return 0;
				else {
					/* just eof, there may be more files ahead... */
					return_read_eof = NO;
					reported_read_eof = NO;
					status_eof_detected = NO; /* reset this too */
					/*fall through*/
				}
			}
		}

/*****************************/
		if (bytes_todo==0)
			return total_bytes_done;

		if (bytes_todo>0) {
			/* start reading data */
			if (is_exception())	/****************************************/
				tpqputs("is_exception() before start_dma()!");
/******************************************************************
 ***** if start_dma() fails because the head is positioned 0 bytes
 ***** before the FM, (causing EXCEPTION to be set) return_read_eof should
 ***** be set to YES, and we should return total_bytes_done, rather than -ENXIO.
 ***** The app should recognize this as an EOF condition.
 ***************************************************************************/
			stat = start_dma(READ, bytes_todo);
			if (stat == TE_OK) {
				/* Wait for transfer to complete, interrupt should wake us */
				while (dma_mode != 0) {
					sleep_on(&tape_qic02_transfer);
				}
				if (status_error)
					return_read_eof = YES;
			} else if (stat != TE_END) {
				/* should do sense() on error here */
#if 0
				return -ENXIO;
#else
				printk("Trouble: stat==%02x\n", stat);
				return_read_eof = YES;
				/*************** check EOF/EOT handling!!!!!! **/
#endif
			}
			end_dma(&bytes_done);
			if (bytes_done>bytes_todo) {
				tpqputs("read: Oops, read more bytes than requested");
				return -EIO;
			}
			/* copy buffer to user-space in one go */
			if (bytes_done>0)
				memcpy_tofs( (void *) buf, (void *) buffaddr, bytes_done);
#if 1
			/* Checks Ton's patch below */
			if ((return_read_eof == NO) && (status_eof_detected == YES)) {
				printk(TPQIC_NAME ": read(): return_read_eof=%d, status_eof_detected=YES. return_read_eof:=YES\n", return_read_eof);
			}
#endif
			if ((bytes_todo != bytes_done) || (status_eof_detected == YES))
				/* EOF or EOM detected. return EOF next time. */
				return_read_eof = YES;
		} /* else: ignore read request for 0 bytes */

		if (bytes_done>0) {
			status_bytes_rd = YES;
			buf += bytes_done;
			filp->f_pos += bytes_done;
			total_bytes_done += bytes_done;
			count -= bytes_done;
		}
	}
	tpqputs("read request for <0 bytes");
	return -EINVAL;
} /* tape_qic02_read */



/* The drive detects near-EOT by means of the holes in the tape.
 * When the holes are detected, there is some space left. The drive
 * reports this as a TP_EOM exception. After clearing the exception,
 * the drive should accept two extra blocks.
 *
 * It seems there are some archiver programs that would like to use the
 * extra space for writing a continuation marker. The driver should return
 * end-of-file to the user program on writes, when the holes are detected.
 * If the user-program wants to use the extra space, it should use the
 * MTNOP ioctl() to get the generic status register and may then continue
 * writing (max 1kB).	----------- doesn't work yet...............
 *
 * EOF behaviour on writes:
 * If there is enough room, write all of the data.
 * If there is insufficient room, write as much as will fit and
 * return the amount written. If the requested amount differs from the
 * written amount, the application program should recognize that as the
 * end of file. Subsequent writes will return -ENOSPC.
 * Unless the minor bits specify a rewind-on-close, the tape will not
 * be rewound when it is full. The user-program should do that, if desired.
 * If the driver were to do that automatically, a user-program could be 
 * confused about the EOT/BOT condition after re-opening the tape device.
 *
 * Multiple volume support: Tar closes the tape device before prompting for
 * the next tape. The user may then insert a new tape and tar will open the
 * tape device again. The driver will detect an exception status in (No Cartridge)
 * and force a rewind. After that tar may continue writing.
 */
static int tape_qic02_write(struct inode * inode, struct file * filp, char * buf, int count)
{
	int error;
	dev_t dev = inode->i_rdev;
	unsigned short flags = filp->f_flags;
	unsigned long bytes_todo, bytes_done, total_bytes_done = 0;

	if (TP_DIAGS(current_tape_dev))
		printk(TPQIC_NAME ": request WRITE, minor=%x, buf=%p, count=%x, pos=%x, flags=%x\n",
			MINOR(dev), buf, count, filp->f_pos, flags);

	if (count % TAPE_BLKSIZE) {	/* only allow mod 512 bytes at a time */
		tpqputs("Wrong block size");
		return -EINVAL;
	}

	if (mode_access==READ) {
		tpqputs("Not in write mode");
		return -EACCES;
	}

	/* open() does a sense() and we can assume the tape isn't changed
	 * between open() and release(), so the tperror.exs bits will still
	 * be valid.
	 */
	if ((tperror.exs & TP_ST0) && (tperror.exs & TP_WRP)) {
		tpqputs("Cartridge is write-protected.");
		return -EACCES;	/* don't even try when write protected */
	}

	/* Make sure buffer is safe to read from. */
	error = verify_area(VERIFY_READ, buf, count);
	if (error) {
		printk(TPQIC_NAME ": write: verify_area(READ, %p, %x) failed\n", buf, count);
		return error;
	}

	if (doing_read == YES)
		terminate_read(0);

	while (count>=0) {
		/* see how much fits in the kernel buffer */
		bytes_done = 0;
		bytes_todo = TPQBUF_SIZE;
		if (bytes_todo>count)
			bytes_todo = count;
	
		if (return_write_eof == YES) {
			/* return_write_eof should be reset on reverse tape movements. */

			if (reported_write_eof==NO) {
				if (bytes_todo>0) {
					tpqputs("partial write");
					/* partial write signals EOF to user program */
				}
				reported_write_eof = YES;
				return total_bytes_done;
			} else {
				return -ENOSPC;		 /* return error */
			}	
		}

		/* Quit when done. */
		if (bytes_todo==0)
			return total_bytes_done;


		/* copy from user to DMA buffer and initiate transfer. */
		if (bytes_todo>0) {
			memcpy_fromfs( (void *) buffaddr, (void *) buf, bytes_todo);

/****************** similar problem with read() at FM could happen here at EOT.
 ******************/

/***** if at EOT, 0 bytes can be written. start_dma() will
 ***** fail and write() will return ENXIO error
 *****/
			if (start_dma(WRITE, bytes_todo) != TE_OK) {
				tpqputs("write: start_dma() failed");
				/* should do sense() on error here */
				return -ENXIO;	/*********** FIXTHIS **************/
			}

			/* Wait for write to complete, interrupt should wake us. */
			while ((status_error == 0) && (dma_mode != 0)) {
				sleep_on(&tape_qic02_transfer);
			}

			end_dma(&bytes_done);
			if (bytes_done>bytes_todo) {
				tpqputs("write: Oops, wrote more bytes than requested");
				return -EIO;
			}
			/* If the dma-transfer was aborted because of an exception,
			 * status_error will have been set in the interrupt handler.
			 * Then end_dma() will do a sense().
			 * If the exception was EXC_EOM, the EW-hole was encountered
			 * and two more blocks could be written. For the time being we'll
			 * just consider this to be the EOT.
			 * Otherwise, something Bad happened, such as the maximum number
			 * of block-rewrites was exceeded. [e.g. A very bad spot on tape was
			 * encountered. Normally short dropouts are compensated for by
			 * rewriting the block in error, up to 16 times. I'm not sure
			 * QIC-24 drives can do this.]
			 */
			if (status_error) {
				if (status_eom_detected == YES)	{
					tpqputs("write: EW detected");
					return_write_eof = YES;
				} else {
					/* probably EXC_RWA */
					tpqputs("write: dma: error in writing");
					return -EIO;
				}
			}
			if (bytes_todo != bytes_done)
				/* EOF or EOM detected. return EOT next time. */
				return_write_eof = YES;
		}
		/* else: ignore write request for 0 bytes. */

		if (bytes_done>0) {
			status_bytes_wr = YES;
			buf += bytes_done;
			filp->f_pos += bytes_done;
			total_bytes_done += bytes_done;
			count -= bytes_done;
		}
	}
	tpqputs("write request for <0 bytes");
	printk(TPQIC_NAME ": status_bytes_wr %x, buf %p, total_bytes_done %x, count %x\n", status_bytes_wr, buf, total_bytes_done, count);
	return -EINVAL;
} /* tape_qic02_write */



/* tape_qic02_open()
 * We allow the device to be opened, even if it is marked 'dead' because
 * we want to be able to reset the tape device without rebooting.
 * Only one open tape file at a time, except when minor=255.
 * Minor 255 is only allowed for resetting and always returns <0.
 * 
 * The density command is only allowed when TP_BOM is set. Thus, remember
 * the most recently used minor bits. When they are different from the
 * remembered values, rewind the tape and set the required density.
 * Don't rewind if the minor bits specify density 0.
 */
static int tape_qic02_open(struct inode * inode, struct file * filp)
{
	dev_t dev = inode->i_rdev;
	unsigned short flags = filp->f_flags;
	unsigned short dens;
	int s;

	status_open = NO;

	if (TP_DIAGS(dev)) {
		printk("tape_qic02_open: dev=%x, flags=%x     ", dev, flags);
	}

	if (MINOR(dev)==255)	/* special case for resetting */
		if (suser())
			return (tape_reset(1)==TE_OK) ? -EAGAIN : -ENXIO;
		else
			return -EPERM;

	if (status_open==YES) {
		return -EBUSY;	/* only one at a time... */
	}
	status_open = YES;

	status_bytes_rd = NO;
	status_bytes_wr = NO;

	return_read_eof = NO;	/********????????????????*****/
	return_write_eof = (status_eot_detected)? YES : NO;

	/* Clear this in case user app close()d before reading EOF token */
	status_eof_detected = NO;

	reported_read_eof = NO;
	reported_write_eof = NO;


	switch (flags & O_ACCMODE) {
		case O_RDONLY:
			mode_access = READ;
			break;
		case O_WRONLY:	/* Fallthru... Strictly speaking this is not correct... */
		case O_RDWR:	/* reads are allowed as long as nothing is written */
			mode_access = WRITE;
			break;
	}


	/* This is to avoid tape-changed problems (TP_CNI exception). */
	if (is_exception()) {
		s = tp_sense(TP_WRP|TP_EOM|TP_BOM|TP_CNI|TP_EOR);
		if (s != TE_OK) {
			status_open = NO;
			tpqputs("open: sense() failed after exception was detected");
			return -EIO;
		}
	}


	/* not allowed to do QCMD_DENS_* unless tape is rewound */
	if ((TP_DENS(dev)!=0) && (TP_DENS(current_tape_dev) != TP_DENS(dev))) {
		/* force rewind if minor bits have changed,
		 * i.e. user wants to use tape in different format.
		 * [assuming single drive operation]
		 */
		tpqputs("Density minor bits have changed. Forcing rewind.");
		need_rewind = YES;
	} else {
		/* density bits still the same, but TP_DIAGS bit 
		 * may have changed.
		 */
		current_tape_dev = dev;
	}

	if (need_rewind == YES) {
		s = do_qic_cmd(QCMD_REWIND, TIM_R);
		if (s != 0) {
			tpqputs("open: rewind failed");
			status_open = NO;
			return -EIO;
		}
	}


/* Note: After a reset command, the controller will rewind the tape
 *	 just before performing any tape movement operation!
 */ 
	if (status_dead) {
		tpqputs("open: tape dead, attempting reset");
		if (tape_reset(1)!=TE_OK) {
			status_open = NO;
			return -ENXIO;
		} else {
			status_dead = NO;
			if (tp_sense(~(TP_ST1|TP_ILL)) != TE_OK) {
				tpqputs("open: tp_sense() failed\n");
				status_dead = YES;	/* try reset next time */
				status_open = NO;
				return -EIO;
			}
		}
	}

	/* things should be ok, once we get here */


	/* set density: only allowed when TP_BOM status bit is set,
	 * so we must have done a rewind by now. If not, just skip over.
	 * Only give set density command when minor bits have changed.
	 */
	if (TP_DENS(current_tape_dev) == TP_DENS(dev) )
		return 0;

	current_tape_dev = dev;
	need_rewind = NO;
	dens = TP_DENS(dev);
	if (dens < sizeof(format_names)/sizeof(char *))
		printk(TPQIC_NAME ": format: %s%s\n", (dens!=0)? "QIC-" : "", format_names[dens]);
	else
		tpqputs("Wait for retensioning...");

	switch (TP_DENS(dev)) {
		case 0: /* This one's for Eddy ;-) */
			s = 0;
			break;
		case 1:
			s = do_qic_cmd(QCMD_DENS_11, TIM_S);
			break;
		case 2:
			s = do_qic_cmd(QCMD_DENS_24, TIM_S);
			break;
		case 3:
			s = do_qic_cmd(QCMD_DENS_120, TIM_S);
			break;
		case 4:
			s = do_qic_cmd(QCMD_DENS_150, TIM_S);
			break;
		case 5:
			s = do_qic_cmd(QCMD_DENS_300, TIM_S);
			break;
		case 6:
			s = do_qic_cmd(QCMD_DENS_600, TIM_S);
			break;
		default:  /* otherwise do a retension before anything else */
			s = do_qic_cmd(QCMD_RETEN, TIM_R);
	}
	if (s != 0) {
		status_open = NO;	/* fail if fault occurred */
		status_dead = YES;	/* force reset */
		current_tape_dev = 0xff80;
		return -EIO;
	}

	return 0;
} /* tape_qic02_open */


static int tape_qic02_readdir(struct inode * inode, struct file * filp, struct dirent * dp, int count)
{
	return -ENOTDIR;	/* not supported */
} /* tape_qic02_readdir */


static void tape_qic02_release(struct inode * inode, struct file * filp)
{
	dev_t dev = inode->i_rdev;

	if (TP_DIAGS(dev))
		printk("tape_qic02_release: dev=%x\n", dev);

	/* Terminate any pending write cycle. Terminating the read-cycle
	 * is delayed until it is required to do so for a new command.
	 */
	terminate_write(-1);

	if (status_dead==YES)
		tpqputs("release: device dead!?");

	if (status_open==NO) {
		tpqputs("release: device not open");
		return;
	}
 	/* Rewind only if minor number requires it AND 
	 * read/writes have been done.
	 */
	if ((TP_REWCLOSE(dev)) && (status_bytes_rd | status_bytes_wr)) {
		tpqputs("release: Doing rewind...");
		(void) do_qic_cmd(QCMD_REWIND, TIM_R);
	}

	status_open = NO;
	return;
} /* tape_qic02_release */



/* ioctl allows user programs to rewind the tape and stuff like that */
static int tape_qic02_ioctl(struct inode * inode, struct file * filp, 
		     unsigned int iocmd, unsigned long ioarg)
{
	int error;
	short i;
	int dev_maj = MAJOR(inode->i_rdev);
	int c;
	struct mtop operation;
	char *stp, *argp;

#ifdef TP_HAVE_TELL
	unsigned char blk_addr[6];
	struct mtpos ioctl_tell;
#endif

	if (TP_DIAGS(current_tape_dev))
		printk(TPQIC_NAME ": ioctl(%4x, %4x, %4x)\n", dev_maj, iocmd, ioarg);

	if (!inode || !ioarg)
		return -EINVAL;

	/* check iocmd first */

	if (dev_maj != QIC02_TAPE_MAJOR) {
		printk(TPQIC_NAME ": Oops! Wrong device?\n");
		/* A panic() would be appropriate here */
		return -ENODEV;
	}

	c = iocmd & IOCCMD_MASK;
	if (c == (MTIOCTOP & IOCCMD_MASK)) {

		/* Compare expected struct size and actual struct size. This
		 * is useful to catch programs compiled with old #includes.
		 */
		if (((iocmd & IOCSIZE_MASK) >> IOCSIZE_SHIFT) != sizeof(struct mtop)) {
			tpqputs("sizeof(struct mtop) does not match!");
			return -EFAULT;
		}
		error = verify_area(VERIFY_READ, (char *) ioarg, sizeof(operation));
		if (error)
			return error;

		/* copy mtop struct from user space to kernel space */
		stp = (char *) &operation;
		argp = (char *) ioarg;
		for (i=0; i<sizeof(operation); i++)
			*stp++ = get_fs_byte(argp++);

		/* ---note: mt_count is signed, negative seeks must be
		 * ---	    translated to seeks in opposite direction!
		 * (only needed for Sun-programs, I think.)
		 */
		/* ---note: MTFSF with count 0 should position the
		 * ---	    tape at the beginning of the current file.
		 */

		if (TP_DIAGS(current_tape_dev))
			printk("OP op=%4x, count=%4x\n", operation.mt_op, operation.mt_count);

		if (operation.mt_count < 0)
			tpqputs("Warning: negative mt_count ignored");

		ioctl_status.mt_resid = operation.mt_count;
		if (operation.mt_op == MTSEEK) {
			seek_addr_buf[0] = (operation.mt_count>>16)&0xff;
			seek_addr_buf[1] = (operation.mt_count>>8)&0xff;
			seek_addr_buf[2] = (operation.mt_count)&0xff;
			if (operation.mt_count>>24)
				return -EINVAL;

			if ((error = do_ioctl_cmd(operation.mt_op)) != 0)
				return error;
			ioctl_status.mt_resid = 0;
		} else {
			while (operation.mt_count > 0) {
				operation.mt_count--;
				if ((error = do_ioctl_cmd(operation.mt_op)) != 0)
					return error;
				ioctl_status.mt_resid = operation.mt_count;
			}
		}
		return 0;

	} else if (c == (MTIOCGET & IOCCMD_MASK)) {
		if (TP_DIAGS(current_tape_dev))
			printk("GET ");

		/* compare expected struct size and actual struct size */
		if (((iocmd & IOCSIZE_MASK) >> IOCSIZE_SHIFT) != sizeof(struct mtget)) {
			tpqputs("sizeof(struct mtget) does not match!");
			return -EFAULT;
		}

		/* check for valid user address */
		error =	verify_area(VERIFY_WRITE, (void *) ioarg, sizeof(ioctl_status));
		if (error)
			return error;

		/* It appears (gmt(1)) that it is normal behaviour to
		 * first set the status with MTNOP, and then to read
		 * it out with MTIOCGET
		 */

		/* copy results to user space */
		stp = (char *) &ioctl_status;
		argp = (char *) ioarg;
		for (i=0; i<sizeof(ioctl_status); i++) 
			put_fs_byte(*stp++, argp++);
		return 0;

#ifdef TP_HAVE_TELL
	} else if (c == (MTIOCPOS & IOCCMD_MASK)) {
		if (TP_DIAGS(current_tape_dev))
			printk("POS ");

		/* compare expected struct size and actual struct size */
		if (((iocmd & IOCSIZE_MASK) >> IOCSIZE_SHIFT) != sizeof(struct mtpos)) {
			tpqputs("sizeof(struct mtpos) does not match!");
			return -EFAULT;
		}

		/* check for valid user address */
		error = verify_area(VERIFY_WRITE, (void *) ioarg, sizeof(ioctl_tell));
		if (error)
			return error;

		tpqputs("MTTELL reading block address");
		if ((doing_read==YES) || (doing_write==YES))
			finish_rw(QCMDV_TELL_BLK);

		c = rdstatus((char *) blk_addr, sizeof(blk_addr), QCMDV_TELL_BLK);
		if (c!=TE_OK)
			return -EIO;

		ioctl_tell.mt_blkno = (blk_addr[3] << 16) | (blk_addr[4] << 8) | blk_addr[5];

		/* copy results to user space */
		stp = (char *) &ioctl_tell;
		argp = (char *) ioarg;
		for (i=0; i<sizeof(ioctl_tell); i++) 
			put_fs_byte(*stp++, argp++);
		return 0;
#endif
	} else
		return -ENOTTY;	/* Other cmds not supported. */
} /* tape_qic02_ioctl */



/* These are (most) of the interface functions: */
static struct file_operations tape_qic02_fops = {
	tape_qic02_lseek,		/* not allowed */
	tape_qic02_read,		/* read */
	tape_qic02_write,		/* write */
	tape_qic02_readdir,		/* not allowed */
	NULL,				/* select ??? */
	tape_qic02_ioctl,		/* ioctl */
	NULL,				/* mmap not allowed */
	tape_qic02_open,		/* open */
	tape_qic02_release,		/* release */
	NULL				/* fsync */
};


/* Attribute `SA_INTERRUPT' makes the interrupt atomic with
 * interrupts disabled. We could do without the atomic stuff, but
 * then dma_transfer() would have to disable interrupts explicitly.
 * System load is high enough as it is :-(
 */
static struct sigaction tape_qic02_sigaction = {
	tape_qic02_interrupt,
	0,
	SA_INTERRUPT,
	NULL
};


/* align `a' at `size' bytes. `size' must be a power of 2 */
static inline unsigned long const align_buffer(unsigned long a, unsigned size)
{
	if (a & (size-1))			/* if not aligned */
		return (a | (size-1)) + 1;
	else					/* else is aligned */
		return a;
}


/* init() is called from chr_dev_init() in kernel/chr_drv/mem.c */
long tape_qic02_init(long kmem_start)
	/* Shouldn't this be a caddr_t ? */
{

	printk(TPQIC_NAME ": IRQ %d, DMA %d, IO %xh, IFC %s, %s, %s\n",
		 TAPE_QIC02_IRQ, TAPE_QIC02_DMA, TAPE_QIC02_PORT,
#if TAPE_QIC02_IFC == WANGTEK
		 "Wangtek",
#elif TAPE_QIC02_IFC == ARCHIVE
		 "Archive",
#else
# error
#endif
		 rcs_revision, rcs_date);

	/* First perform some checks. If one of them fails,
	 * the tape driver will not be registered to the system.
	 */

	/* Should do IRQ/DMA allocation in open(). Use free_irq() in release()
	 * return -EBUSY, if allocation fails in open().
	 * Make IRQ settable/readable through ioctls. DMA is trickier because
	 * some bits change for different DMA numbers. Also, this would add 
	 * runtime overhead for having the dma channel number be a variable
	 * rather than a constant. Probably need to make the DMA stuff #ifdef'd
	 * to choose between hardcoded and changeable DMA channel.
	 *
	 * Also, Linux needs a more generic way to set DMA/IRQ and other stuff.
	 * Right now, this is done differently for every device. (setserial,
	 * ctrlaltdel, setdfprm, setfdthr, tunelp, ((setterm))...)
	 *
	 * #include <hint.h>
	 */

	/* get IRQ */
	if (irqaction(TAPE_QIC02_IRQ, &tape_qic02_sigaction)) {
		printk(TPQIC_NAME ": can't allocate IRQ%d for QIC-02 tape\n",
			TAPE_QIC02_IRQ);
		return kmem_start;
	}

	/* After IRQ, allocate DMA channel */
	if (request_dma(TAPE_QIC02_DMA)) {
		printk(TPQIC_NAME ": can't allocate DMA%d for QIC-02 tape\n",
			TAPE_QIC02_DMA);
		free_irq(TAPE_QIC02_IRQ);
		return kmem_start;
	}

	if (TPSTATSIZE != 6) {
		printk(TPQIC_NAME ": internal error: tpstatus struct incorrect!\n");
		return kmem_start;
	}
	if ((TPQBUF_SIZE<512) || (TPQBUF_SIZE>=0x10000)) {
		printk(TPQIC_NAME ": internal error: DMA buffer size out of range\n");
		return kmem_start;
	}

	printk(TPQIC_NAME ": DMA buffers: %u blocks", NR_BLK_BUF);

	/* Setup the page-address for the dma transfer.
	 * This assumes a one-to-one identity mapping between
	 * kernel addresses and physical memory.
	 */
	buffaddr = align_buffer((unsigned long) &tape_qic02_buf, TAPE_BLKSIZE);
	printk(", at address 0x%lx (0x%lx)\n", buffaddr, (unsigned long) &tape_qic02_buf);

#ifndef CONFIG_MAX_16M
	if (buffaddr+TPQBUF_SIZE>=0x1000000) {
		printk(TPQIC_NAME ": DMA buffer *must* be in lower 16MB\n");
		return kmem_start;
	}
#endif

	/* If we got this far, install driver functions */
	if (register_chrdev(QIC02_TAPE_MAJOR, TPQIC_NAME, &tape_qic02_fops)) {
		printk(TPQIC_NAME ": Unable to get chrdev major %d\n",
		       QIC02_TAPE_MAJOR);
		return kmem_start;
	}

	/* prepare timer */
	TIMEROFF;
	timer_table[TAPE_QIC02_TIMER].expires = 0;
	timer_table[TAPE_QIC02_TIMER].fn = tape_qic02_times_out;

	if (tape_reset(0)!=TE_OK || tp_sense(TP_WRP|TP_POR|TP_CNI)!=TE_OK) {
		status_dead = YES;
	} else {
		if (is_exception()) {
			tpqputs("exception detected\n");
			(void) tp_sense(TP_WRP|TP_POR|TP_CNI);
		}
	}

	/* initialize generic status for ioctl requests */

	ioctl_status.mt_type	= TAPE_QIC02_DRIVE;	/* MT_IS* id nr */

	ioctl_status.mt_resid	= 0;	/* ---residual count */
	ioctl_status.mt_gstat	= 0;	/* ---generic status */
	ioctl_status.mt_erreg	= 0;	/* not used */
	ioctl_status.mt_fileno	= 0;	/* number of current file on tape */
	ioctl_status.mt_blkno	= 0;	/* number of current (logical) block */

	return kmem_start;
} /* tape_qic02_init */


#endif /* CONFIG_TAPE_QIC02 */
