/* $Id: wd7000.c,v 1.2 1994/01/15 06:02:32 drew Exp $
 *  linux/kernel/wd7000.c
 *
 *  Copyright (C) 1992  Thomas Wuensche
 *	closely related to the aha1542 driver from Tommy Thorn
 *	( as close as different hardware allows on a lowlevel-driver :-) )
 *
 *  Revised (and renamed) by John Boyd <boyd@cis.ohio-state.edu> to
 *  accomodate Eric Youngdale's modifications to scsi.c.  Nov 1992.
 *
 *  Additional changes to support scatter/gather.  Dec. 1992.  tw/jb
 */

#include <stdarg.h>
#include <linux/kernel.h>
#include <linux/head.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <asm/system.h>
#include <asm/dma.h>
#include <asm/io.h>
#include <linux/ioport.h>

#include "../block/blk.h"
#include "scsi.h"
#include "hosts.h"

/* #define DEBUG  */

#include "wd7000.h"


#ifdef DEBUG
#define DEB(x) x
#else
#define DEB(x)
#endif


/*
   Driver data structures:
   - mb and scbs are required for interfacing with the host adapter.
     An SCB has extra fields not visible to the adapter; mb's
     _cannot_ do this, since the adapter assumes they are contiguous in
     memory, 4 bytes each, with ICMBs following OGMBs, and uses this fact
     to access them.
   - An icb is for host-only (non-SCSI) commands.  ICBs are 16 bytes each;
     the additional bytes are used only by the driver.
   - For now, a pool of SCBs are kept in global storage by this driver,
     and are allocated and freed as needed.

  The 7000-FASST2 marks OGMBs empty as soon as it has _started_ a command,
  not when it has finished.  Since the SCB must be around for completion,
  problems arise when SCBs correspond to OGMBs, which may be reallocated
  earlier (or delayed unnecessarily until a command completes).
  Mailboxes are used as transient data structures, simply for
  carrying SCB addresses to/from the 7000-FASST2.

  Note also since SCBs are not "permanently" associated with mailboxes,
  there is no need to keep a global list of Scsi_Cmnd pointers indexed
  by OGMB.   Again, SCBs reference their Scsi_Cmnds directly, so mailbox
  indices need not be involved.
*/

static struct {
       struct wd_mailbox ogmb[OGMB_CNT]; 
       struct wd_mailbox icmb[ICMB_CNT];
} mb;
static int next_ogmb = 0;   /* to reduce contention at mailboxes */

static Scb scbs[MAX_SCBS];
static Scb *scbfree = NULL;

static int wd7000_host = 0;
static unchar controlstat = 0;

static unchar rev_1 = 0, rev_2 = 0;  /* filled in by wd7000_revision */

#define wd7000_intr_ack()  outb(0,INTR_ACK)

#define WAITnexttimeout 3000000


static inline void wd7000_enable_intr(void)
{
    controlstat |= INT_EN;
    outb(controlstat,CONTROL);
}


static inline void wd7000_enable_dma(void)
{
    controlstat |= DMA_EN;
    outb(controlstat,CONTROL);
    set_dma_mode(DMA_CH, DMA_MODE_CASCADE);
    enable_dma(DMA_CH);
}


#define WAIT(port, mask, allof, noneof)					\
 { register WAITbits;							\
   register WAITtimeout = WAITnexttimeout;				\
   while (1) {								\
     WAITbits = inb(port) & (mask);					\
     if ((WAITbits & (allof)) == (allof) && ((WAITbits & (noneof)) == 0)) \
       break;                                                         	\
     if (--WAITtimeout == 0) goto fail;					\
   }									\
 }


static inline void delay( unsigned how_long )
{
     unsigned long time = jiffies + how_long;

     while (jiffies < time);
}


static inline int command_out(unchar *cmdp, int len)
{
    while (len--)  {
        WAIT(ASC_STAT, STATMASK, CMD_RDY, 0);
	outb(*cmdp++, COMMAND);
    }
    return 1;

fail:
    printk("wd7000_out WAIT failed(%d): ", len+1);
    return 0;
}

static inline Scb *alloc_scb(void)
{
    Scb *scb;
    unsigned long flags;

    save_flags(flags);
    cli();

    if (scbfree == NULL)  {
        panic("wd7000: can't allocate free SCB.\n");
	restore_flags(flags);
	return NULL;
    }
    scb = scbfree;  scbfree = scb->next;
    memset(scb, 0, sizeof(Scb));  scb->next = NULL;

    restore_flags(flags);

    return scb;
}


static inline void free_scb( Scb *scb )
{
    unsigned long flags;

    save_flags(flags);
    cli();

    memset(scb, 0, sizeof(Scb));
    scb->next = scbfree;  scbfree = scb;

    restore_flags(flags);
}


static inline void init_scbs(void)
{
    int i;
    unsigned long flags;

    save_flags(flags);
    cli();

    scbfree = &(scbs[0]);
    for (i = 0;  i < MAX_SCBS-1;  i++)  scbs[i].next = &(scbs[i+1]);
    scbs[MAX_SCBS-1].next = NULL;

    restore_flags(flags);
}    
    

static int mail_out( Scb *scbptr )
/*
 *  Note: this can also be used for ICBs; just cast to the parm type.
 */
{
    int i, ogmb;
    unsigned long flags;

    DEB(printk("wd7000_scb_out: %06x");)

    /* We first look for a free outgoing mailbox */
    save_flags(flags);
    cli();
    ogmb = next_ogmb;
    for (i = 0; i < OGMB_CNT; i++) {
	if (mb.ogmb[ogmb].status == 0)  {
	    DEB(printk(" using OGMB %x",ogmb));
	    mb.ogmb[ogmb].status = 1;
	    any2scsi(mb.ogmb[ogmb].scbptr, scbptr);

	    next_ogmb = (ogmb+1) % OGMB_CNT;
	    break;
	}  else
	    ogmb = (++ogmb) % OGMB_CNT;
    }
    restore_flags(flags);
    DEB(printk(", scb is %x",scbptr);)

    if (i >= OGMB_CNT) {
        DEB(printk(", no free OGMBs.\n");)
	/* Alternatively, issue "interrupt on free OGMB", and sleep... */
        return 0;
    }

    wd7000_enable_intr(); 
    do  {
        WAIT(ASC_STAT,STATMASK,CMD_RDY,0);
	outb(START_OGMB|ogmb, COMMAND);
	WAIT(ASC_STAT,STATMASK,CMD_RDY,0);
    }  while (inb(ASC_STAT) & CMD_REJ);

    DEB(printk(", awaiting interrupt.\n");)
    return 1;

fail:
    DEB(printk(", WAIT timed out.\n");)
    return 0;
}


int make_code(unsigned hosterr, unsigned scsierr)
{   
#ifdef DEBUG
    int in_error = hosterr;
#endif

    switch ((hosterr>>8)&0xff){
	case 0:	/* Reserved */
		hosterr = DID_ERROR;
		break;
	case 1:	/* Command Complete, no errors */
		hosterr = DID_OK;
		break;
	case 2: /* Command complete, error logged in scb status (scsierr) */ 
		hosterr = DID_OK;
		break;
	case 4:	/* Command failed to complete - timeout */
		hosterr = DID_TIME_OUT;
		break;
	case 5:	/* Command terminated; Bus reset by external device */
		hosterr = DID_RESET;
		break;
	case 6:	/* Unexpected Command Received w/ host as target */
		hosterr = DID_BAD_TARGET;
		break;
	case 80: /* Unexpected Reselection */
        case 81: /* Unexpected Selection */
		hosterr = DID_BAD_INTR;
		break;
        case 82: /* Abort Command Message  */
		hosterr = DID_ABORT;
		break;
	case 83: /* SCSI Bus Software Reset */
	case 84: /* SCSI Bus Hardware Reset */
		hosterr = DID_RESET;
		break;
        default: /* Reserved */
		hosterr = DID_ERROR;
		break;
	}
#ifdef DEBUG
    if (scsierr||hosterr)
        printk("\nSCSI command error: SCSI %02x host %04x return %d",
	       scsierr,in_error,hosterr);
#endif
    return scsierr | (hosterr << 16);
}


static void wd7000_scsi_done(Scsi_Cmnd * SCpnt)
{
    DEB(printk("wd7000_scsi_done: %06x\n",SCpnt);)
    SCpnt->SCp.phase = 0;
}


void wd7000_intr_handle(int irq)
{
    int flag, icmb, errstatus, icmb_status;
    int host_error, scsi_error;
    Scb *scb;             /* for SCSI commands */
    unchar *icb;          /* for host commands */
    Scsi_Cmnd *SCpnt;

    flag = inb(INTR_STAT);
    DEB(printk("wd7000_intr_handle: intr stat = %02x",flag);)

    if (!(inb(ASC_STAT)&0x80)){ 
	DEB(printk("\nwd7000_intr_handle: phantom interrupt...\n");)
	wd7000_intr_ack();
	return; 
    }

    /* check for an incoming mailbox */
    if ((flag & 0x40) == 0) {
        /*  for a free OGMB - need code for this case... */
        DEB(printk("wd7000_intr_handle: free outgoing mailbox\n");)
        wd7000_intr_ack();
	return;
    }
    /* The interrupt is for an incoming mailbox */
    icmb = flag & 0x3f;
    scb = (struct scb *) scsi2int(mb.icmb[icmb].scbptr);
    icmb_status = mb.icmb[icmb].status;
    mb.icmb[icmb].status = 0;

#ifdef DEBUG
    printk(" ICMB %d posted for SCB/ICB %06x, status %02x, vue %02x",
 	   icmb, scb, icmb_status, scb->vue );
#endif

    if (!(scb->op & 0x80))  {   /* an SCB is done */
        SCpnt = scb->SCpnt;
	if (--(SCpnt->SCp.phase) <= 0)  {  /* all scbs for SCpnt are done */
	    host_error = scb->vue | (icmb_status << 8);
	    scsi_error = scb->status;
	    errstatus = make_code(host_error,scsi_error);    
	    SCpnt->result = errstatus;

 	    if (SCpnt->host_scribble != NULL)
 	        scsi_free(SCpnt->host_scribble,WD7000_SCRIBBLE);
 	    free_scb(scb);

	    SCpnt->scsi_done(SCpnt);
	}
    }  else  {    /* an ICB is done */
        icb = (unchar *) scb;
        icb[ICB_STATUS] = icmb_status;
	icb[ICB_PHASE] = 0;
    }

    wd7000_intr_ack();
    DEB(printk(".\n");)
    return;
}


int wd7000_queuecommand(Scsi_Cmnd * SCpnt, void (*done)(Scsi_Cmnd *))
{
    Scb *scb;
    Sgb *sgb;
    unchar *cdb;
    unchar idlun;
    short cdblen;

    cdb = (unchar *) SCpnt->cmnd;
    cdblen = COMMAND_SIZE(*cdb);
    idlun = ((SCpnt->target << 5) & 0xe0) | (SCpnt->lun & 7);
    SCpnt->scsi_done = done;
    SCpnt->SCp.phase = 1;
    scb = alloc_scb();
    scb->idlun = idlun;
    memcpy(scb->cdb, cdb, cdblen);
    scb->direc = 0x40;		/* Disable direction check */
    scb->SCpnt = SCpnt;         /* so we can find stuff later */
    SCpnt->host_scribble = NULL;
    DEB(printk("request_bufflen is %x, bufflen is %x\n",\
        SCpnt->request_bufflen, SCpnt->bufflen);)

    if (SCpnt->use_sg)  {
        struct scatterlist *sg = (struct scatterlist *) SCpnt->request_buffer;
        unsigned i;

        if (scsi_hosts[wd7000_host].sg_tablesize <= 0)  {
	    panic("wd7000_queuecommand: scatter/gather not supported.\n");
	}
#ifdef DEBUG
 	printk("Using scatter/gather with %d elements.\n",SCpnt->use_sg);
#endif
  	/*
 	    Allocate memory for a scatter/gather-list in wd7000 format.
 	    Save the pointer at host_scribble.
  	*/
#ifdef DEBUG
 	if (SCpnt->use_sg > WD7000_SG)
 	    panic("WD7000: requesting too many scatterblocks\n");
#endif
 	SCpnt->host_scribble = (unsigned char *) scsi_malloc(WD7000_SCRIBBLE);
	sgb = (Sgb *) SCpnt->host_scribble;
 	if (sgb == NULL)
            panic("wd7000_queuecommand: scsi_malloc() failed.\n");

 	scb->op = 1;
 	any2scsi(scb->dataptr, sgb);
 	any2scsi(scb->maxlen, SCpnt->use_sg * sizeof (Sgb) );

	for (i = 0;  i < SCpnt->use_sg;  i++)  {
 	    any2scsi(sgb->ptr, sg[i].address);
 	    any2scsi(sgb->len, sg[i].length);
 	    sgb++;
        }
 	DEB(printk("Using %d bytes for %d scatter/gather blocks\n",\
 	    scsi2int(scb->maxlen), SCpnt->use_sg);)
    }  else  {
	scb->op = 0;
	any2scsi(scb->dataptr, SCpnt->request_buffer);
	any2scsi(scb->maxlen, SCpnt->request_bufflen);
    }

    return mail_out(scb);
}


int wd7000_command(Scsi_Cmnd *SCpnt)
{
    wd7000_queuecommand(SCpnt, wd7000_scsi_done);

    while (SCpnt->SCp.phase > 0);  /* phase counts scbs down to 0 */

    return SCpnt->result;
}


int wd7000_init(void)
{   int i;
    unchar init_block[] = {
        INITIALIZATION, 7, BUS_ON, BUS_OFF, 0, 0, 0, 0, OGMB_CNT, ICMB_CNT
    };

    /* Reset the adapter. */
    outb(SCSI_RES|ASC_RES, CONTROL);
    delay(1);  /* reset pulse: this is 10ms, only need 25us */
    outb(0,CONTROL);  controlstat = 0;
    /*
       Wait 2 seconds, then expect Command Port Ready.

       I suspect something else needs to be done here, but I don't know
       what.  The OEM doc says power-up diagnostics take 2 seconds, and
       indeed, SCSI commands submitted before then will time out, but
       none of what follows seems deterred by _not_ waiting 2 secs.
    */
    delay(200);

    WAIT(ASC_STAT, STATMASK, CMD_RDY, 0);
    DEB(printk("wd7000_init: Power-on Diagnostics finished\n");)
    if (((i=inb(INTR_STAT)) != 1) && (i != 7)) {
	panic("wd7000_init: Power-on Diagnostics error\n"); 
	return 0;
    }
    
    /* Clear mailboxes */
    memset(&mb,0,sizeof (mb));
    /* Set up SCB free list */
    init_scbs();

    /* Set up init block */
    any2scsi(init_block+5,&mb);
    /* Execute init command */
    if (!command_out(init_block,sizeof(init_block)))  {
	panic("WD-7000 Initialization failed.\n"); 
	return 0;
    }
    
    /* Wait until init finished */
    WAIT(ASC_STAT, STATMASK, CMD_RDY | ASC_INI, 0);
    outb(DISABLE_UNS_INTR, COMMAND); 
    WAIT(ASC_STAT, STATMASK, CMD_RDY | ASC_INI, 0);

    /* Enable Interrupt and DMA */
    if (request_irq(IRQ_LVL, wd7000_intr_handle)) {
      panic("Unable to allocate IRQ for WD-7000.\n");
      return 0;
    };
    if(request_dma(DMA_CH)) {
      panic("Unable to allocate DMA channel for WD-7000.\n");
      free_irq(IRQ_LVL);
      return 0;
    };
    wd7000_enable_dma();
    wd7000_enable_intr();

    printk("WD-7000 initialized.\n");
    return 1;
  fail:
    return 0;					/* 0 = not ok */
}


void wd7000_revision(void)
{
    volatile unchar icb[ICB_LEN] = {0x8c};  /* read firmware revision level */

    icb[ICB_PHASE] = 1;
    mail_out( (struct scb *) icb );
    while (icb[ICB_PHASE]) /* wait for completion */;
    rev_1 = icb[1];
    rev_2 = icb[2];

    /*
        For boards at rev 7.0 or later, enable scatter/gather.
    */
    if (rev_1 >= 7)  scsi_hosts[wd7000_host].sg_tablesize = WD7000_SG;
}


static const char *wd_bases[] = {(char *)0xce000,(char *)0xd8000};

typedef struct {
    char * signature;
    unsigned offset;
    unsigned length;
} Signature;

static const Signature signatures[] = {{"SSTBIOS",0xd,0x7}};

#define NUM_SIGNATURES (sizeof(signatures)/sizeof(Signature))


int wd7000_detect(int hostnum)
/* 
 *  return non-zero on detection
 */
{
    int i,j;
    char const *base_address = NULL;

    if(check_region(IO_BASE, 4)) return 0;  /* IO ports in use */
    for(i=0;i<(sizeof(wd_bases)/sizeof(char *));i++){
	for(j=0;j<NUM_SIGNATURES;j++){
	    if(!memcmp((void *)(wd_bases[i] + signatures[j].offset),
		(void *) signatures[j].signature,signatures[j].length)){
		    base_address=wd_bases[i];
		    printk("WD-7000 detected.\n");
	    }	
	}
    }
    if (base_address == NULL) return 0;

    snarf_region(IO_BASE, 4); /* Register our ports */
    /* Store our host number */
    wd7000_host = hostnum;

    wd7000_init();    
    wd7000_revision();  /* will set scatter/gather by rev level */

    return 1;
}



static void wd7000_append_info( char *info, const char *fmt, ... )
/*
 *  This is just so I can use vsprintf...
 */
{
    va_list args;
    extern int vsprintf(char *buf, const char *fmt, va_list args);

    va_start(args, fmt);
    vsprintf(info, fmt, args);
    va_end(args);

    return;
}


const char *wd7000_info(void)
{
    static char info[80] = "Western Digital WD-7000, Firmware Revision ";

    wd7000_revision();
    wd7000_append_info( info+strlen(info), "%d.%d.\n", rev_1, rev_2 );

    return info;
}

int wd7000_abort(Scsi_Cmnd * SCpnt, int i)
{
#ifdef DEBUG
    printk("wd7000_abort: Scsi_Cmnd = 0x%08x, code = %d ", SCpnt, i);
    printk("id %d lun %d cdb", SCpnt->target, SCpnt->lun);
    {  int j;  unchar *cdbj = (unchar *) SCpnt->cmnd;
       for (j=0; j < COMMAND_SIZE(*cdbj);  j++)  printk(" %02x", *(cdbj++));
       printk(" result %08x\n", SCpnt->result);
    }
#endif
    return 0;
}


/* We do not implement a reset function here, but the upper level code assumes
   that it will get some kind of response for the command in SCpnt.  We must
   oblige, or the command will hang the scsi system */

int wd7000_reset(Scsi_Cmnd * SCpnt)
{
#ifdef DEBUG
    printk("wd7000_reset\n");
#endif
    if (SCpnt) SCpnt->flags |= NEEDS_JUMPSTART;
    return 0;
}


int wd7000_biosparam(int size, int dev, int* ip)
/*
 *  This is borrowed directly from aha1542.c, but my disks are organized
 *   this way, so I think it will work OK.
 */
{
  ip[0] = 64;
  ip[1] = 32;
  ip[2] = size >> 11;
/*  if (ip[2] >= 1024) ip[2] = 1024; */
  return 0;
}

