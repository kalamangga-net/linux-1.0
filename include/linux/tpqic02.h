/* $Id: tpqic02.h,v 0.16 1993/04/19 23:15:39 root Exp root $
 *
 * Include file for QIC-02 driver for Linux.
 *
 * Copyright (c) 1992 by H. H. Bergman. All rights reserved.
 *
 * ******* USER CONFIG SECTION BELOW *******
 */

#ifndef _LINUX_TPQIC02_H
#define _LINUX_TPQIC02_H

#include <linux/config.h>

#if CONFIG_TAPE_QIC02

/* need to have TAPE_QIC02_DRIVE and TAPE_QIC02_IFC expand to something */
#include <linux/mtio.h>


/* make TAPE_QIC02_IFC expand to something */
#define WANGTEK		1		   /* don't know about Wangtek QIC-36 */
#define EVEREX		WANGTEK      /* I heard *some* of these are identical */
#define EVEREX_811V	EVEREX			      /* With TEAC MT 2ST 45D */
#define EVEREX_831V	EVEREX
#define ARCHIVE		3
#define ARCHIVE_SC400	ARCHIVE	       /* rumoured to be from the pre-SMD-age */
#define ARCHIVE_SC402	ARCHIVE		       /* don't know much about SC400 */
#define ARCHIVE_SC499	ARCHIVE       /* SC402 and SC499R should be identical */


/*********** START OF USER CONFIGURABLE SECTION ************/

/* Tape configuration: 
 *
 * Tape drive configuration:	(MT_IS* constants are defined in sys/mtio.h)
 *
 * TAPE_QIC02_DRIVE = MT_ISWT5150
 *	- Wangtek 5150, format: up to QIC-150.
 * TAPE_QIC02_DRIVE = MT_ISQIC02_ALL_FEATURES
 *	- Enables some optional QIC commands that some drives may lack.
 *	  It is provided so you can check which are supported by your drive.
 *	  Refer to tpqic02.h for others.
 *
 * Supported interface cards: TAPE_QIC02_IFC =
 *	WANGTEK,
 *	ARCHIVE_SC402, ARCHIVE_SC499.	(both same programming interface)
 *
 * Make sure you have the I/O ports/DMA channels 
 * and IRQ stuff configured properly!
 * NOTE: Check for conflicts with TAPE_QIC02_TIMER in timer.h.
 */

#define TAPE_QIC02_DRIVE	MT_ISQIC02_ALL_FEATURES	/* drive type */
/* #define TAPE_QIC02_DRIVE	MT_ISWT5150 */
#define TAPE_QIC02_IFC		WANGTEK		/* interface card type */
/* #define TAPE_QIC02_IFC		ARCHIVE */
#define TAPE_QIC02_PORT 	0x300	/* controller port adress */
#define TAPE_QIC02_IRQ		5	/* Muhammad, please don't use 2 here. -- Hennus */
#define TAPE_QIC02_DMA		1	/* either 1 or 3, because 2 is used by the floppy */


/************ END OF USER CONFIGURABLE SECTION *************/


/* NOTE: TP_HAVE_DENS should distinguish between available densities
 * NOTE: Drive select is not implemented -- I have only one tape streamer,
 *	 so I'm unable and unmotivated to test and implement that. ;-) ;-)
 */
#if TAPE_QIC02_DRIVE == MT_ISWT5150
#define TP_HAVE_DENS
#define TP_HAVE_BSF	/* nope */
#define TP_HAVE_FSR	/* nope */
#define TP_HAVE_BSR	/* nope */
#define TP_HAVE_EOD	/* most of the time */
#define TP_HAVE_RAS1
#define TP_HAVE_RAS2

#elif TAPE_QIC02_DRIVE == MT_ISARCHIVESC499	/* Archive SC-499 QIC-36 controller */
#define TP_HAVE_DENS		/* can do set density (QIC-11 / QIC-24) */
#define TP_HAVE_FSR		/* can skip one block forwards */
#define TP_HAVE_BSR		/* can skip one block backwards */
#define TP_HAVE_EOD		/* can seek to end of recorded data */
#define TP_HAVE_RAS1		/* can run selftest 1 */
#define TP_HAVE_RAS2		/* can run selftest 2 */
/* These last two selftests shouldn't be used yet! */

#elif (TAPE_QIC02_DRIVE == MT_ISARCHIVE_2060L) || (TAPE_QIC02_DRIVE == MT_ISARCHIVE_2150L)
#define TP_HAVE_DENS		/* can do set density (QIC-24 / QIC-120 / QIC-150) */
#define TP_HAVE_FSR		/* can skip one block forwards */
#define TP_HAVE_BSR		/* can skip one block backwards */
#define TP_HAVE_EOD		/* can seek to end of recorded data */
#define TP_HAVE_TELL		/* can read current block address */
#define TP_HAVE_SEEK		/* can seek to block */
#define TP_HAVE_RAS1		/* can run selftest 1 */
#define TP_HAVE_RAS2		/* can run selftest 2 */
/* These last two selftests shouldn't be used yet! */

#elif TAPE_QIC02_DRIVE == MT_ISQIC02_ALL_FEATURES
#define TP_HAVE_DENS		/* can do set density */
#define TP_HAVE_BSF		/* can search filemark backwards */
#define TP_HAVE_FSR		/* can skip one block forwards */
#define TP_HAVE_BSR		/* can skip one block backwards */
#define TP_HAVE_EOD		/* can seek to end of recorded data */
#define TP_HAVE_SEEK		/* seek to block address */
#define TP_HAVE_TELL		/* tell current block address */
#define TP_HAVE_RAS1		/* can run selftest 1 */
#define TP_HAVE_RAS2		/* can run selftest 2 */
/* These last two selftests shouldn't be used yet! */


#else
#error No QIC-02 tape drive type defined!
/* If your drive is not listed above, first try the 'ALL_FEATURES',
 * to see what commands are supported, then create your own entry in
 * the list above. You may want to mail it to me, so that I can include
 * it in the next release.
 */
#endif


/* NR_BLK_BUF is a `tuneable parameter'. If you're really low on
 * kernel space, you could decrease it to 1, or if you got a very
 * slow machine, you could increase it up to 128 blocks. Less kernel
 * buffer blocks result in more context-switching.
 */
#define NR_BLK_BUF	20				    /* max 128 blocks */
#define TAPE_BLKSIZE	512		  /* streamer tape block size (fixed) */
#define TPQBUF_SIZE	(TAPE_BLKSIZE*NR_BLK_BUF)	       /* buffer size */


#define BLOCKS_BEYOND_EW	2	/* nr of blocks after Early Warning hole */

#if TAPE_QIC02_IFC == WANGTEK	
  /* Wangtek interface card port locations */
# define QIC_STAT_PORT	TAPE_QIC02_PORT
# define QIC_CTL_PORT	TAPE_QIC02_PORT
# define QIC_CMD_PORT	(TAPE_QIC02_PORT+1)
# define QIC_DATA_PORT	(TAPE_QIC02_PORT+1)

/* status register bits (Active LOW!) */
# define QIC_STAT_READY		0x01
# define QIC_STAT_EXCEPTION	0x02
# define QIC_STAT_MASK		(QIC_STAT_READY|QIC_STAT_EXCEPTION)

# define QIC_STAT_RESETMASK	0x07
# define QIC_STAT_RESETVAL	(QIC_STAT_RESETMASK & ~QIC_STAT_EXCEPTION)

/* controller register (QIC_CTL_PORT) bits */
# define WT_CTL_ONLINE		0x01
# define QIC_CTL_RESET		0x02
# define QIC_CTL_REQUEST	0x04
# define WT_CTL_CMDOFF		0xC0 
# if TAPE_QIC02_DMA == 3   /* dip-switches alone don't seem to cut it */
#  define WT_CTL_DMA		0x10			  /* enable dma chan3 */
# elif TAPE_QIC02_DMA == 1
#  define WT_CTL_DMA		0x08	         /* enable dma chan1 or chan2 */
# else
#  error Unsupported or incorrect DMA configuration.
# endif

#elif TAPE_QIC02_IFC == ARCHIVE
 /* Archive interface card port locations */
# define QIC_STAT_PORT		(TAPE_QIC02_PORT+1)
# define QIC_CTL_PORT		(TAPE_QIC02_PORT+1)
# define QIC_CMD_PORT		(TAPE_QIC02_PORT)
# define QIC_DATA_PORT		(TAPE_QIC02_PORT)
# define AR_START_DMA_PORT	(TAPE_QIC02_PORT+2)
# define AR_RESET_DMA_PORT	(TAPE_QIC02_PORT+3)

  /* STAT port bits */
# define AR_STAT_IRQF		0x80	/* active high, interrupt request flag */
# define QIC_STAT_READY		0x40	/* active low */
# define QIC_STAT_EXCEPTION	0x20	/* active low */
# define QIC_STAT_MASK		(QIC_STAT_READY|QIC_STAT_EXCEPTION)
# define AR_STAT_DMADONE	0x10	/* active high, DMA done */
# define AR_STAT_DIRC		0x08	/* active high, direction */

# define QIC_STAT_RESETMASK	0x70	/* check RDY,EXC,DMADONE */
# define QIC_STAT_RESETVAL	((QIC_STAT_RESETMASK & ~AR_STAT_IRQF & ~QIC_STAT_EXCEPTION) | AR_STAT_DMADONE)

  /* CTL port bits */
# define QIC_CTL_RESET		0x80	/* drive reset */
# define QIC_CTL_REQUEST	0x40	/* notify of new command */
# define AR_CTL_IEN		0x20	/* interrupt enable */
# define AR_CTL_DNIEN		0x10	/* done-interrupt enable */
  /* Note: All of these bits are cleared automatically when writing to
   * AR_RESET_DMA_PORT. So AR_CTL_IEN and AR_CTL_DNIEN must be
   * reprogrammed before the write to AR_START_DMA_PORT.
   */

# if TAPE_QIC02_DMA > 3		/* channel 2 is used by the floppy driver */
#  error DMA channels other than 1 and 3 are not supported.
# endif

#else
# error No valid interface card specified!
#endif /* TAPE_QIC02_IFC */

/* Standard QIC-02 commands -- rev F.  All QIC-02 drives must support these */
#define QCMD_SEL_1	0x01		/* select drive 1 */
#define QCMD_SEL_2	0x02		/* select drive 2 */
#define QCMD_SEL_3	0x04		/* select drive 3 */
#define QCMD_SEL_4	0x08		/* select drive 4 */
#define	QCMD_REWIND	0x21		/* rewind tape*/
#define QCMD_ERASE	0x22		/* erase tape */
#define QCMD_RETEN	0x24		/* retension tape */
#define	QCMD_WRT_DATA	0x40		/* write data */
#define	QCMD_WRT_FM	0x60		/* write file mark */
#define	QCMD_RD_DATA	0x80		/* read data */
#define	QCMD_RD_FM	0xA0		/* read file mark (forward direction) */
#define	QCMD_RD_STAT	0xC0		/* read status */

/* Other (optional/vendor unique) commands */
 /* Density commands are only valid when TP_BOM is set! */
#define QCMD_DENS_11	0x26		/* QIC-11 */
#define QCMD_DENS_24	0x27		/* QIC-24: 9 track 60MB */
#define QCMD_DENS_120	0x28		/* QIC-120: 15 track 120MB */
#define QCMD_DENS_150	0x29		/* QIC-150: 18 track 150MB */
#define QCMD_DENS_300	0x2A		/* QIC-300/QIC-2100 */
#define QCMD_DENS_600	0x2B		/* QIC-600/QIC-2200 */
/* don't know about QIC-1000 and QIC-1350 */

#define	QCMD_WRTNU_DATA	0x40		/* write data, no underruns, insert filler. */
#define QCMD_SPACE_FWD	0x81		/* skip next block */
#define QCMD_SPACE_BCK	0x89		/* move tape head one block back -- very useful! */
#define QCMD_RD_FM_BCK	0xA8		/* read filemark (backwards) */
#define QCMD_SEEK_EOD	0xA3		/* skip to EOD */
#define	QCMD_RD_STAT_X1	0xC1		/* read extended status 1 */
#define	QCMD_RD_STAT_X2	0xC4		/* read extended status 2 */
#define	QCMD_RD_STAT_X3	0xE0		/* read extended status 3 */
#define QCMD_SELF_TST1	0xC2		/* run self test 1 (nondestructive) */
#define QCMD_SELF_TST2	0xCA		/* run self test 2 (destructive) */



/* "Vendor Unique" codes */
#if defined(MT_ISARCHIVESC499) || defined(MT_ISARCHIVE_2150L)
# define QCMDV_TELL_BLK		0xAE		/* read current block address */
# define QCMDV_SEEK_BLK		0xAD		/* seek to specific block */
# define SEEK_BUF_SIZE		3		/* address is 3 bytes */
#endif


/* Optional, QFA (Quick File Access) commands.
 * Not all drives support this, but those that do could use these commands
 * to implement semi-non-sequential access. `mt fsf` would benefit from this.
 * QFA divides the tape into 2 partitions, a data and a directory partition,
 * causing some incompatibility problems wrt std QIC-02 data exchange.
 * It would be useful to cache the directory info, but that might be tricky
 * to do in kernel-space. [Size constraints.]
 * Refer to QIC-02, appendix A for more information.
 * I have no idea how other *nix variants implement QFA.
 * I have no idea which drives support QFA and which don't.
 */
#define QFA_ENABLE	0x2D		/* enter QFA mode, give @ BOT only */
#define QFA_DATA	0x20		/* select data partition */
#define QFA_DIR		0x23		/* select directory partition */
#define QFA_RD_POS	0xCF		/* read position+status bytes */
#define QFA_SEEK_EOD	0xA1		/* seek EOD within current partition */
#define QFA_SEEK_BLK	0xAF		/* seek to a block within current partition */



/* Minor device codes for tapes:
 * |7|6|5|4|3|2|1|0|
 *  | \ | / \ | / |_____ 1=rewind on close, 0=no rewind on close
 *  |  \|/    |_________ Density: 000=none, 001=QIC-11, 010=24, 011=120,
 *  |   |                100=QIC-150, 101..111 reserved.
 *  |   |_______________ Reserved for unit numbers.
 *  |___________________ Reserved for diagnostics during debugging.
 */

#define	TP_REWCLOSE(d)	((MINOR(d)&0x01) == 1)	   		/* rewind bit */
			   /* rewind is only done if data has been transfered */
#define	TP_DENS(dev)	((MINOR(dev) >> 1) & 0x07) 	      /* tape density */
#define TP_UNIT(dev)	((MINOR(dev) >> 4) & 0x07)	       /* unit number */
#define TP_DIAGS(dev)	(MINOR(dev) & 0x80)    /* print excessive diagnostics */


/* status codes returned by a WTS_RDSTAT call */
struct tpstatus {	/* sizeof(short)==2), LSB first */
	unsigned short	exs;	/* Drive exception flags */
	unsigned short	dec;	/* data error count: nr of blocks rewritten/soft read errors */
	unsigned short	urc;	/* underrun count: nr of times streaming was interrupted */
};
#define TPSTATSIZE	sizeof(struct tpstatus)


/* defines for tpstatus.exs -- taken from 386BSD wt driver */
#define	TP_POR		0x100	/* Power on or reset occurred */
#define	TP_EOR		0x200	/* REServed for end of RECORDED media */
#define	TP_PAR		0x400	/* REServed for bus parity */
#define	TP_BOM		0x800	/* Beginning of media */
#define	TP_MBD		0x1000	/* Marginal block detected */
#define	TP_NDT		0x2000	/* No data detected */
#define	TP_ILL		0x4000	/* Illegal command */
#define	TP_ST1		0x8000	/* Status byte 1 flag */
#define	TP_FIL		0x01	/* File mark detected */
#define	TP_BNL		0x02	/* Bad block not located */
#define	TP_UDA		0x04	/* Unrecoverable data error */
#define	TP_EOM		0x08	/* End of media */
#define	TP_WRP		0x10	/* Write protected cartridge */
#define	TP_USL		0x20	/* Unselected drive */
#define	TP_CNI		0x40	/* Cartridge not in place */
#define	TP_ST0		0x80	/* Status byte 0 flag */

#define REPORT_ERR0	(TP_CNI|TP_USL|TP_WRP|TP_EOM|TP_UDA|TP_BNL|TP_FIL)
#define REPORT_ERR1	(TP_ILL|TP_NDT|TP_MBD|TP_PAR)


#define EXC_UNKNOWN	0	/* (extra) Unknown exception code */
#define EXC_NCART	1	/* No cartridge */
#define EXC_NDRV	2	/* No drive */
#define EXC_WP		3	/* Write protected */
#define EXC_EOM		4	/* EOM */
#define EXC_RWA		5	/* read/write abort */
#define EXC_XBAD	6	/* read error, bad block transfered */
#define EXC_XFILLER	7	/* read error, filler block transfered */
#define EXC_NDT		8	/* read error, no data */
#define EXC_NDTEOM	9	/* read error, no data & EOM */
#define EXC_NDTBOM	10	/* read error, no data & BOM */
#define EXC_FM		11	/* Read a filemark */
#define EXC_ILL		12	/* Illegal command */
#define EXC_POR		13	/* Power on/reset */
#define EXC_MARGINAL	14	/* Marginal block detected */
#define EXC_EOR		15	/* (extra, for SEEKEOD) End Of Recorded data reached */
#define EXC_BOM		16	/* (extra) BOM reached */


#define TAPE_NOTIFY_TIMEOUT	1000000

/* internal function return codes */
#define TE_OK	0		/* everything is fine */
#define TE_EX	1		/* exception detected */
#define TE_ERR	2		/* some error */
#define TE_NS	3		/* can't read status */
#define TE_TIM	4		/* timed out */
#define TE_DEAD	5		/* tape drive doesn't respond */
#define TE_END	6		/******** Archive hack *****/

/* timeout timer values -- check these! */
#define TIM_S	(4*HZ)		/* 4 seconds (normal cmds) */
#define TIM_M	(30*HZ)		/* 30 seconds (write FM) */
#define TIM_R	(8*60*HZ)	/* 8 minutes (retensioning) */
#define TIM_F	(2*3600*HZ)	/* est. 1.2hr for full tape read/write+2 retens */

#define TIMERON(t)	timer_table[TAPE_QIC02_TIMER].expires = jiffies + (t); \
			timer_active |= (1<<TAPE_QIC02_TIMER)
#define TIMEROFF	timer_active &= ~(1<<TAPE_QIC02_TIMER)
#define TIMERCONT	timer_active |= (1<<TAPE_QIC02_TIMER)


typedef char flag;
#define NO	0	/* NO must be 0 */
#define YES	1	/* YES must be != 0 */


#ifdef TDEBUG
# define TPQDEB(s)	s
# define TPQPUTS(s)	tpqputs(s)
#else
# define TPQDEB(s)
# define TPQPUTS(s)
#endif



extern long tape_qic02_init(long);			  /* for kernel/mem.c */


#endif /* CONFIG_TAPE_QIC02 */

#endif /* _LINUX_TPQIC02_H */
