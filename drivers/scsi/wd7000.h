#ifndef _WD7000_H

/* $Id: $
 *
 * Header file for the WD-7000 driver for Linux
 *
 * $Log: $
 * Revision 1.1  1992/07/24  06:27:38  root
 * Initial revision
 *
 * Revision 1.1  1992/07/05  08:32:32  root
 * Initial revision
 *
 * Revision 1.1  1992/05/15  18:38:05  root
 * Initial revision
 *
 * Revision 1.1  1992/04/02  03:23:13  drew
 * Initial revision
 *
 * Revision 1.3  1992/01/27  14:46:29  tthorn
 * *** empty log message ***
 *
 */

#include <linux/types.h>

#undef STATMASK
#undef CONTROL

#define IO_BASE 	0x350
#define IRQ_LVL 	15
#define DMA_CH  	6
#define OGMB_CNT	8
#define ICMB_CNT	16

/* I/O Port interface 4.2 */
/* READ */
#define ASC_STAT IO_BASE
#define INT_IM	0x80		/* Interrupt Image Flag */
#define CMD_RDY	0x40		/* Command Port Ready */
#define CMD_REJ	0x20		/* Command Port Byte Rejected */
#define ASC_INI	0x10		/* ASC Initialized Flag */
#define STATMASK 0xf0		/* The lower 4 Bytes are reserved */

/* This register serves two purposes
 * Diagnostics error code
 * Interrupt Status
 */
#define INTR_STAT ASC_STAT+1
#define ANYINTR	0x80		/* Mailbox Service possible/required */
#define IMB	0x40		/* 1 Incoming / 0 Outgoing */
#define MBMASK 0x3f
/* if MSb is zero, the lower bits are diagnostic status *
 * Diagnostics:
 * 01	No diagnostic error occurred
 * 02	RAM failure
 * 03	FIFO R/W failed
 * 04   SBIC register read/write failed
 * 05   Initialization D-FF failed
 * 06   Host IRQ D-FF failed
 * 07   ROM checksum error
 * Interrupt status (bitwise):
 * 10NNNNNN   outgoing mailbox NNNNNN is free
 * 11NNNNNN   incoming mailbox NNNNNN needs service
 */

/* WRITE */
#define COMMAND ASC_STAT
/*
 *  COMMAND opcodes
 */
#define NO_OP             0
#define INITIALIZATION    1     /* initialization after reset (10 bytes) */
#define DISABLE_UNS_INTR  2     /* disable unsolicited interrupts */
#define ENABLE_UNS_INTR   3     /* enable unsolicited interrupts */
#define INTR_ON_FREE_OGMB 4     /* interrupt on free OGMB */
#define SCSI_SOFT_RESET   5     /* SCSI soft reset */
#define SCSI_HARD_RESET   6     /* SCSI hard reset acknowledge */
#define START_OGMB        0x80  /* start command in OGMB (n) */
#define SCAN_OGMBS        0xc0  /* start multiple commands, signature (n) */
                                /*    where (n) = lower 6 bits */
/*
 *  For INITIALIZATION:
 */
#define BUS_ON            48    /* x 125ns, 48 = 6000ns, BIOS uses 8000ns */
#define BUS_OFF           24    /* x 125ns, 24 = 3000ns, BIOS uses 1875ns */
 
#define INTR_ACK ASC_STAT+1


#define CONTROL ASC_STAT+2
#define INT_EN	0x08		/* Interrupt Enable	*/
#define DMA_EN	0x04		/* DMA Enable		*/
#define SCSI_RES	0x02	/* SCSI Reset		*/
#define ASC_RES	0x01		/* ASC Reset		*/

/* Mailbox Definition */

struct wd_mailbox{
	unchar status;
	unchar scbptr[3];
};


/* These belong in scsi.h also */
#undef any2scsi
#define any2scsi(up, p)			\
(up)[0] = (((long)(p)) >> 16);	\
(up)[1] = ((long)(p)) >> 8;		\
(up)[2] = ((long)(p));

#define scsi2int(up) ( (((long)*(up)) << 16) + (((long)(up)[1]) << 8) + ((long)(up)[2]) )

#define xany2scsi(up, p)	\
(up)[0] = ((long)(p)) >> 24;	\
(up)[1] = ((long)(p)) >> 16;	\
(up)[2] = ((long)(p)) >> 8;	\
(up)[3] = ((long)(p));

#define xscsi2int(up) ( (((long)(up)[0]) << 24) + (((long)(up)[1]) << 16) \
		      + (((long)(up)[2]) <<  8) +  ((long)(up)[3]) )

#define MAX_CDB 12
#define MAX_SENSE 14

typedef struct scb {		/* Command Control Block 5.4.1 */
  unchar op;			/* Command Control Block Operation Code */
  unchar idlun;			/* op=0,2:Target Id, op=1:Initiator Id */
				/* Outbound data transfer, length is checked*/
				/* Inbound data transfer, length is checked */
				/* Logical Unit Number */
  unchar cdb[12];		/* SCSI Command Block */
  unchar status;		/* SCSI Return Status */
  unchar vue;			/* Vendor Unique Error Code */
  unchar maxlen[3];		/* Maximum Data Transfer Length */
  unchar dataptr[3];		/* SCSI Data Block Pointer */
  unchar linkptr[3];		/* Next Command Link Pointer */
  unchar direc;			/* Transfer Direction */
  unchar reserved2[6];		/* SCSI Command Descriptor Block */
                                /* end of hardware SCB */
  Scsi_Cmnd *SCpnt;             /* Scsi_Cmnd using this SCB */
  struct scb *next;             /* for lists of scbs */
} Scb;

/*
 *  WD7000-specific scatter/gather element structure
 */
typedef struct sgb {
    unchar len[3];
    unchar ptr[3];
} Sgb;

/*
 *  Note:  MAX_SCBS _must_ be defined large enough to keep ahead of the
 *  demand for SCBs, which will be at most WD7000_Q * WD7000_SG.  1 is
 *  added to each because they can be 0.
 */
#define MAX_SCBS  ((WD7000_Q+1) * (WD7000_SG+1))

/*
 *  The driver is written to allow host-only commands to be executed.  These
 *  use a 16-byte block called an ICB.
 *
 *  (Currently, only wd7000_info uses this, to get the firmware rev. level.)
 */
#define ICB_STATUS  16          /* set to icmb status by wd7000_intr_handle */
#define ICB_PHASE   17          /* set to 0 by wd7000_intr_handle */
#define ICB_LEN     18          /* actually 16; this includes the above */

int wd7000_detect(int);
int wd7000_command(Scsi_Cmnd *);
int wd7000_queuecommand(Scsi_Cmnd *, void (*done)(Scsi_Cmnd *));
int wd7000_abort(Scsi_Cmnd *, int);
const char *wd7000_info(void);
int wd7000_reset(Scsi_Cmnd *);
int wd7000_biosparam(int, int, int*);

#ifndef NULL
	#define NULL 0
#endif

/*
 *  Define WD7000_SG to be the number of Sgbs that will fit in a block of
 *  size WD7000_SCRIBBLE.  WD7000_SCRIBBLE must be 512, 1024, 2048, or 4096.
 *
 *  The sg_tablesize value will default to SG_NONE for older boards (before
 *  rev 7.0), but will be changed to WD7000_SG when a newer board is
 *  detected.
 */
#define WD7000_SCRIBBLE  512

#define WD7000_Q    OGMB_CNT
#define WD7000_SG   (WD7000_SCRIBBLE / sizeof(Sgb))

#define WD7000 {\
	"Western Digital WD-7000",      \
	wd7000_detect,                  \
	wd7000_info, wd7000_command,	\
	wd7000_queuecommand,	        \
	wd7000_abort,			\
	wd7000_reset,			\
	NULL,                           \
	wd7000_biosparam,               \
	WD7000_Q, 7, SG_NONE, 1, 0, 1}
#endif
