/* $Id: aha1542.c,v 1.1 1992/07/24 06:27:38 root Exp root $
 *  linux/kernel/aha1542.c
 *
 *  Copyright (C) 1992  Tommy Thorn
 *
 *  Modified by Eric Youngdale
 *        Use request_irq and request_dma to help prevent unexpected conflicts
 *        Set up on-board DMA controller, such that we do not have to
 *        have the bios enabled to use the aha1542.
 *  Modified by David Gentzel
 *	  Don't call request_dma if dma mask is 0 (for BusLogic BT-445S VL-Bus controller).
 */

#include <linux/kernel.h>
#include <linux/head.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/ioport.h>
#include <linux/config.h>

#include <linux/sched.h>
#include <asm/dma.h>

#include <asm/system.h>
#include <asm/io.h>
#include "../block/blk.h"
#include "scsi.h"
#include "hosts.h"

#include "aha1542.h"

#ifdef DEBUG
#define DEB(x) x
#else
#define DEB(x)
#endif
/*
static const char RCSid[] = "$Header: /usr/src/linux/kernel/blk_drv/scsi/RCS/aha1542.c,v 1.1 1992/07/24 06:27:38 root Exp root $";
*/

/* The adaptec can be configured for quite a number of addresses, but
I generally do not want the card poking around at random.  We allow
two addresses - this allows people to use the Adaptec with a Midi
card, which also used 0x330 */

static unsigned int bases[]={0x330, 0x334};

/* The DMA-Controller.  We need to fool with this because we want to 
   be able to use the aha1542 without having to have the bios enabled */
#define DMA_MODE_REG	0xd6
#define DMA_MASK_REG	0xd4
#define	CASCADE		0xc0

#define BIOS_TRANSLATION_1632 0  /* Used by some old 1542A boards */
#define BIOS_TRANSLATION_6432 1 /* Default case these days */
#define BIOS_TRANSLATION_25563 2 /* Big disk case */

struct aha1542_hostdata{
	/* This will effectively start both of them at the first mailbox */
        int bios_translation;   /* Mapping bios uses - for compatibility */
	int aha1542_last_mbi_used;
	int aha1542_last_mbo_used;
	Scsi_Cmnd * SCint[AHA1542_MAILBOXES];
	struct mailbox mb[2*AHA1542_MAILBOXES];
	struct ccb ccb[AHA1542_MAILBOXES];
};

#define HOSTDATA(host) ((struct aha1542_hostdata *) &host->hostdata)

static struct Scsi_Host * aha_host[7] = {NULL,};  /* One for each IRQ level (9-15) */




#define WAITnexttimeout 3000000

static void setup_mailboxes(int base_io, struct Scsi_Host * shpnt);

#define aha1542_intr_reset(base)  outb(IRST, CONTROL(base))

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

static void aha1542_stat(void)
{
/*    int s = inb(STATUS), i = inb(INTRFLAGS);
  printk("status=%x intrflags=%x\n", s, i, WAITnexttimeout-WAITtimeout); */
}

static int aha1542_out(unsigned int base, unchar *cmdp, int len)
{
    while (len--)
      {
	  WAIT(STATUS(base), CDF, 0, CDF);
	  outb(*cmdp++, DATA(base));
      }
    return 0;
  fail:
    printk("aha1542_out failed(%d): ", len+1); aha1542_stat();
    return 1;
}

static int aha1542_in(unsigned int base, unchar *cmdp, int len)
{
    while (len--)
      {
	  WAIT(STATUS(base), DF, DF, 0);
	  *cmdp++ = inb(DATA(base));
      }
    return 0;
  fail:
    printk("aha1542_in failed(%d): ", len+1); aha1542_stat();
    return 1;
}

static int makecode(unsigned hosterr, unsigned scsierr)
{
    switch (hosterr) {
      case 0x0:
      case 0xa: /* Linked command complete without error and linked normally */
      case 0xb: /* Linked command complete without error, interrupt generated */
	hosterr = 0;
	break;

      case 0x11: /* Selection time out-The initiator selection or target
		    reselection was not complete within the SCSI Time out period */
	hosterr = DID_TIME_OUT;
	break;

      case 0x12: /* Data overrun/underrun-The target attempted to transfer more data
		    thean was allocated by the Data Length field or the sum of the
		    Scatter / Gather Data Length fields. */

      case 0x13: /* Unexpected bus free-The target dropped the SCSI BSY at an unexpected time. */

      case 0x15: /* MBO command was not 00, 01 or 02-The first byte of the CB was
		    invalid. This usually indicates a software failure. */

      case 0x16: /* Invalid CCB Operation Code-The first byte of the CCB was invalid.
		    This usually indicates a software failure. */

      case 0x17: /* Linked CCB does not have the same LUN-A subsequent CCB of a set
		    of linked CCB's does not specify the same logical unit number as
		    the first. */
      case 0x18: /* Invalid Target Direction received from Host-The direction of a
		    Target Mode CCB was invalid. */

      case 0x19: /* Duplicate CCB Received in Target Mode-More than once CCB was
		    received to service data transfer between the same target LUN
		    and initiator SCSI ID in the same direction. */

      case 0x1a: /* Invalid CCB or Segment List Parameter-A segment list with a zero
		    length segment or invalid segment list boundaries was received.
		    A CCB parameter was invalid. */
	DEB(printk("Aha1542: %x %x\n", hosterr, scsierr));
	hosterr = DID_ERROR; /* Couldn't find any better */
	break;

      case 0x14: /* Target bus phase sequence failure-An invalid bus phase or bus
		    phase sequence was requested by the target. The host adapter
		    will generate a SCSI Reset Condition, notifying the host with
		    a SCRD interrupt */
	hosterr = DID_RESET;
	break;
      default:
	printk("makecode: unknown hoststatus %x\n", hosterr);
	break;
    }
    return scsierr|(hosterr << 16);
}

static int aha1542_test_port(int bse, struct Scsi_Host * shpnt)
{
    int i;
    unchar inquiry_cmd[] = {CMD_INQUIRY };
    unchar inquiry_result[4];
    unchar *cmdp;
    int len;
    volatile int debug = 0;
    
    /* Quick and dirty test for presence of the card. */
    if(inb(STATUS(bse)) == 0xff) return 0;

    /* Reset the adapter. I ought to make a hard reset, but it's not really nessesary */
    
    /*  DEB(printk("aha1542_test_port called \n")); */
    
    outb(SRST|IRST/*|SCRST*/, CONTROL(bse));

    i = jiffies + 2;
    while (i>jiffies); /* Wait a little bit for things to settle down. */
    
    debug = 1;
    /* Expect INIT and IDLE, any of the others are bad */
    WAIT(STATUS(bse), STATMASK, INIT|IDLE, STST|DIAGF|INVDCMD|DF|CDF);
    
    debug = 2;
    /* Shouldn't have generated any interrupts during reset */
    if (inb(INTRFLAGS(bse))&INTRMASK) goto fail;


    /* Perform a host adapter inquiry instead so we do not need to set
       up the mailboxes ahead of time */

    aha1542_out(bse, inquiry_cmd, 1);

    debug = 3;
    len = 4;
    cmdp = &inquiry_result[0];

    while (len--)
      {
	  WAIT(STATUS(bse), DF, DF, 0);
	  *cmdp++ = inb(DATA(bse));
      }
    
    debug = 8;
    /* Reading port should reset DF */
    if (inb(STATUS(bse)) & DF) goto fail;
    
    debug = 9;
    /* When HACC, command is completed, and we're though testing */
    WAIT(INTRFLAGS(bse), HACC, HACC, 0);
    /* now initialize adapter */
    
    debug = 10;
    /* Clear interrupts */
    outb(IRST, CONTROL(bse));
    
    debug = 11;
    
    return debug;				/* 1 = ok */
  fail:
    return 0;					/* 0 = not ok */
}

static const char aha_ident[] = "Adaptec 1542";

/* What's this little function for? */
const char *aha1542_info(void)
{
    return aha_ident;
}

/* A "high" level interrupt handler */
static void aha1542_intr_handle(int foo)
{
    void (*my_done)(Scsi_Cmnd *) = NULL;
    int errstatus, mbi, mbo, mbistatus;
    int number_serviced;
    struct Scsi_Host * shost;
    Scsi_Cmnd * SCtmp;
    int irqno, * irqp;
    struct mailbox * mb;
    struct ccb  *ccb;

    irqp = (int *) foo;
    irqp -= 2;  /* Magic - this is only required for slow interrupt handlers */
    irqno = *irqp;

    shost = aha_host[irqno - 9];
    mb = HOSTDATA(shost)->mb;
    ccb = HOSTDATA(shost)->ccb;

    if(!shost) panic("Splunge!");

#ifdef DEBUG
    {
    int flag = inb(INTRFLAGS(shost->io_port));
    printk("aha1542_intr_handle: ");
    if (!(flag&ANYINTR)) printk("no interrupt?");
    if (flag&MBIF) printk("MBIF ");
    if (flag&MBOA) printk("MBOF ");
    if (flag&HACC) printk("HACC ");
    if (flag&SCRD) printk("SCRD ");
    printk("status %02x\n", inb(STATUS(shost->io_port)));
  };
#endif
    number_serviced = 0;

    while(1==1){
      aha1542_intr_reset(shost->io_port);

      cli();
      mbi = HOSTDATA(shost)->aha1542_last_mbi_used + 1;
      if (mbi >= 2*AHA1542_MAILBOXES) mbi = AHA1542_MAILBOXES;
      
      do{
	if(mb[mbi].status != 0) break;
	mbi++;
	if (mbi >= 2*AHA1542_MAILBOXES) mbi = AHA1542_MAILBOXES;
      } while (mbi != HOSTDATA(shost)->aha1542_last_mbi_used);
      
      if(mb[mbi].status == 0){
	sti();
	/* Hmm, no mail.  Must have read it the last time around */
	if (number_serviced) return;
	printk("aha1542.c: interrupt received, but no mail.\n");
	return;
      };

      mbo = (scsi2int(mb[mbi].ccbptr) - ((unsigned int) &ccb[0])) / sizeof(struct ccb);
      mbistatus = mb[mbi].status;
      mb[mbi].status = 0;
      HOSTDATA(shost)->aha1542_last_mbi_used = mbi;
      sti();
      
#ifdef DEBUG
      {
	if (ccb[mbo].tarstat|ccb[mbo].hastat)
	  printk("aha1542_command: returning %x (status %d)\n", 
		 ccb[mbo].tarstat + ((int) ccb[mbo].hastat << 16), mb[mbi].status);
      };
#endif

      if(mbistatus == 3) continue; /* Aborted command not found */

#ifdef DEBUG
      printk("...done %d %d\n",mbo, mbi);
#endif
      
      SCtmp = HOSTDATA(shost)->SCint[mbo];

      if (!SCtmp || !SCtmp->scsi_done) {
	printk("aha1542_intr_handle: Unexpected interrupt\n");
	return;
      }
      
      my_done = SCtmp->scsi_done;
      if (SCtmp->host_scribble) scsi_free(SCtmp->host_scribble, 512);
      
      /* Fetch the sense data, and tuck it away, in the required slot.  The
	 Adaptec automatically fetches it, and there is no guarantee that
	 we will still have it in the cdb when we come back */
      if (ccb[mbo].tarstat == 2)
	memcpy(SCtmp->sense_buffer, &ccb[mbo].cdb[ccb[mbo].cdblen], 
	       sizeof(SCtmp->sense_buffer));
      
      
      /* is there mail :-) */
      
      /* more error checking left out here */
      if (mbistatus != 1)
	/* This is surely wrong, but I don't know what's right */
	errstatus = makecode(ccb[mbo].hastat, ccb[mbo].tarstat);
      else
	errstatus = 0;
      
#ifdef DEBUG
      if(errstatus) printk("(aha1542 error:%x %x %x) ",errstatus, 
			   ccb[mbo].hastat, ccb[mbo].tarstat);
#endif

      if (ccb[mbo].tarstat == 2) {
#ifdef DEBUG
	int i;
#endif
	DEB(printk("aha1542_intr_handle: sense:"));
#ifdef DEBUG
	for (i = 0; i < 12; i++)
	  printk("%02x ", ccb[mbo].cdb[ccb[mbo].cdblen+i]);
	printk("\n");
#endif
	/*
	  DEB(printk("aha1542_intr_handle: buf:"));
	  for (i = 0; i < bufflen; i++)
	  printk("%02x ", ((unchar *)buff)[i]);
	  printk("\n");
	  */
      }
      DEB(if (errstatus) printk("aha1542_intr_handle: returning %6x\n", errstatus));
      SCtmp->result = errstatus;
      HOSTDATA(shost)->SCint[mbo] = NULL;  /* This effectively frees up the mailbox slot, as
			     far as queuecommand is concerned */
      my_done(SCtmp);
      number_serviced++;
    };
}

int aha1542_queuecommand(Scsi_Cmnd * SCpnt, void (*done)(Scsi_Cmnd *))
{
    unchar ahacmd = CMD_START_SCSI;
    unchar direction;
    unchar *cmd = (unchar *) SCpnt->cmnd;
    unchar target = SCpnt->target;
    unchar lun = SCpnt->lun;
    void *buff = SCpnt->request_buffer;
    int bufflen = SCpnt->request_bufflen;
    int mbo;
    struct mailbox * mb;
    struct ccb  *ccb;

    DEB(int i);

    mb = HOSTDATA(SCpnt->host)->mb;
    ccb = HOSTDATA(SCpnt->host)->ccb;

    DEB(if (target > 1) {
      SCpnt->result = DID_TIME_OUT << 16;
      done(SCpnt); return 0;});
    
    if(*cmd == REQUEST_SENSE){
#ifndef DEBUG
      if (bufflen != sizeof(SCpnt->sense_buffer)) {
	printk("Wrong buffer length supplied for request sense (%d)\n",bufflen);
	panic("aha1542.c");
      };
#endif
      SCpnt->result = 0;
      done(SCpnt); 
      return 0;
    };

#ifdef DEBUG
    if (*cmd == READ_10 || *cmd == WRITE_10)
      i = xscsi2int(cmd+2);
    else if (*cmd == READ_6 || *cmd == WRITE_6)
      i = scsi2int(cmd+2);
    else
      i = -1;
    if (done)
      printk("aha1542_queuecommand: dev %d cmd %02x pos %d len %d ", target, *cmd, i, bufflen);
    else
      printk("aha1542_command: dev %d cmd %02x pos %d len %d ", target, *cmd, i, bufflen);
    aha1542_stat();
    printk("aha1542_queuecommand: dumping scsi cmd:");
    for (i = 0; i < (COMMAND_SIZE(*cmd)); i++) printk("%02x ", cmd[i]);
    printk("\n");
    if (*cmd == WRITE_10 || *cmd == WRITE_6)
      return 0; /* we are still testing, so *don't* write */
#endif
/* Use the outgoing mailboxes in a round-robin fashion, because this
   is how the host adapter will scan for them */

    cli();
    mbo = HOSTDATA(SCpnt->host)->aha1542_last_mbo_used + 1;
    if (mbo >= AHA1542_MAILBOXES) mbo = 0;

    do{
      if(mb[mbo].status == 0 && HOSTDATA(SCpnt->host)->SCint[mbo] == NULL)
	break;
      mbo++;
      if (mbo >= AHA1542_MAILBOXES) mbo = 0;
    } while (mbo != HOSTDATA(SCpnt->host)->aha1542_last_mbo_used);

    if(mb[mbo].status || HOSTDATA(SCpnt->host)->SCint[mbo])
      panic("Unable to find empty mailbox for aha1542.\n");

    HOSTDATA(SCpnt->host)->SCint[mbo] = SCpnt;  /* This will effectively prevent someone else from
			    screwing with this cdb. */

    HOSTDATA(SCpnt->host)->aha1542_last_mbo_used = mbo;    
    sti();

#ifdef DEBUG
    printk("Sending command (%d %x)...",mbo, done);
#endif

    any2scsi(mb[mbo].ccbptr, &ccb[mbo]); /* This gets trashed for some reason*/

    memset(&ccb[mbo], 0, sizeof(struct ccb));

    ccb[mbo].cdblen = COMMAND_SIZE(*cmd);     /* SCSI Command Descriptor Block Length */

    direction = 0;
    if (*cmd == READ_10 || *cmd == READ_6)
	direction = 8;
    else if (*cmd == WRITE_10 || *cmd == WRITE_6)
	direction = 16;

    memcpy(ccb[mbo].cdb, cmd, ccb[mbo].cdblen);

    if (SCpnt->use_sg) {
      struct scatterlist * sgpnt;
      struct chain * cptr;
#ifdef DEBUG
      unsigned char * ptr;
#endif
      int i;
      ccb[mbo].op = 2;	      /* SCSI Initiator Command  w/scatter-gather*/
      SCpnt->host_scribble = (unsigned char *) scsi_malloc(512);
      sgpnt = (struct scatterlist *) SCpnt->request_buffer;
      cptr = (struct chain *) SCpnt->host_scribble; 
      if (cptr == NULL) panic("aha1542.c: unable to allocate DMA memory\n");
      for(i=0; i<SCpnt->use_sg; i++) {
	if(sgpnt[i].length == 0 || SCpnt->use_sg > 16 || 
	   (((int)sgpnt[i].address) & 1) || (sgpnt[i].length & 1)){
	  unsigned char * ptr;
	  printk("Bad segment list supplied to aha1542.c (%d, %d)\n",SCpnt->use_sg,i);
	  for(i=0;i<SCpnt->use_sg;i++){
	    printk("%d: %x %x %d\n",i,(unsigned int) sgpnt[i].address, (unsigned int) sgpnt[i].alt_address,
		   sgpnt[i].length);
	  };
	  printk("cptr %x: ",(unsigned int) cptr);
	  ptr = (unsigned char *) &cptr[i];
	  for(i=0;i<18;i++) printk("%02x ", ptr[i]);
	  panic("Foooooooood fight!");
	};
	any2scsi(cptr[i].dataptr, sgpnt[i].address);
	if(((unsigned  int) sgpnt[i].address) & 0xff000000) goto baddma;
	any2scsi(cptr[i].datalen, sgpnt[i].length);
      };
      any2scsi(ccb[mbo].datalen, SCpnt->use_sg * sizeof(struct chain));
      if(((unsigned int) buff & 0xff000000)) goto baddma;
      any2scsi(ccb[mbo].dataptr, cptr);
#ifdef DEBUG
      printk("cptr %x: ",cptr);
      ptr = (unsigned char *) cptr;
      for(i=0;i<18;i++) printk("%02x ", ptr[i]);
#endif
    } else {
      ccb[mbo].op = 0;	      /* SCSI Initiator Command */
      SCpnt->host_scribble = NULL;
      any2scsi(ccb[mbo].datalen, bufflen);
      any2scsi(ccb[mbo].dataptr, buff);
    };
    ccb[mbo].idlun = (target&7)<<5 | direction | (lun & 7); /*SCSI Target Id*/
    ccb[mbo].rsalen = 12;
    ccb[mbo].linkptr[0] = ccb[mbo].linkptr[1] = ccb[mbo].linkptr[2] = 0;
    ccb[mbo].commlinkid = 0;

#ifdef DEBUGd
    { int i;
    printk("aha1542_command: sending.. ");
    for (i = 0; i < sizeof(ccb[mbo])-10; i++)
      printk("%02x ", ((unchar *)&ccb[mbo])[i]);
    };
#endif
    
    if (done) {
	DEB(printk("aha1542_queuecommand: now waiting for interrupt "); aha1542_stat());
	SCpnt->scsi_done = done;
	mb[mbo].status = 1;
	aha1542_out(SCpnt->host->io_port, &ahacmd, 1);		/* start scsi command */
	DEB(aha1542_stat());
    }
    else
      printk("aha1542_queuecommand: done can't be NULL\n");
    
    return 0;
 baddma:
    panic("Buffer at address  > 16Mb used for 1542B");
}

static void internal_done(Scsi_Cmnd * SCpnt)
{
	SCpnt->SCp.Status++;
}

int aha1542_command(Scsi_Cmnd * SCpnt)
{
    DEB(printk("aha1542_command: ..calling aha1542_queuecommand\n"));

    aha1542_queuecommand(SCpnt, internal_done);

    SCpnt->SCp.Status = 0;
    while (!SCpnt->SCp.Status);
    return SCpnt->result;
}

/* Initialize mailboxes */
static void setup_mailboxes(int bse, struct Scsi_Host * shpnt)
{
    int i;
    struct mailbox * mb;
    struct ccb  *ccb;

    unchar cmd[5] = {CMD_MBINIT, AHA1542_MAILBOXES};

    mb = HOSTDATA(shpnt)->mb;
    ccb = HOSTDATA(shpnt)->ccb;

    for(i=0; i<AHA1542_MAILBOXES; i++){
      mb[i].status = mb[AHA1542_MAILBOXES+i].status = 0;
      any2scsi(mb[i].ccbptr, &ccb[i]);
    };
    aha1542_intr_reset(bse);     /* reset interrupts, so they don't block */	
    any2scsi((cmd+2), mb);
    aha1542_out(bse, cmd, 5);
    WAIT(INTRFLAGS(bse), INTRMASK, HACC, 0);
    while (0) {
      fail:
	printk("aha1542_detect: failed setting up mailboxes\n");
    }
    aha1542_intr_reset(bse);
}

static int aha1542_getconfig(int base_io, unsigned char * irq_level, unsigned char * dma_chan)
{
  unchar inquiry_cmd[] = {CMD_RETCONF };
  unchar inquiry_result[3];
  int i;
  i = inb(STATUS(base_io));
  if (i & DF) {
    i = inb(DATA(base_io));
  };
  aha1542_out(base_io, inquiry_cmd, 1);
  aha1542_in(base_io, inquiry_result, 3);
  WAIT(INTRFLAGS(base_io), INTRMASK, HACC, 0);
  while (0) {
  fail:
    printk("aha1542_detect: query board settings\n");
  }
  aha1542_intr_reset(base_io);
  switch(inquiry_result[0]){
  case 0x80:
    *dma_chan = 7;
    break;
  case 0x40:
    *dma_chan = 6;
    break;
  case 0x20:
    *dma_chan = 5;
    break;
  case 0x01:
    printk("DMA priority 0 not available for Adaptec driver\n");
    return -1;
  case 0:
    /* This means that the adapter, although Adaptec 1542 compatible, doesn't use a DMA channel.
       Currently only aware of the BusLogic BT-445S VL-Bus adapter which needs this. */
    *dma_chan = 0xFF;
    break;
  default:
    printk("Unable to determine Adaptec DMA priority.  Disabling board\n");
    return -1;
  };
  switch(inquiry_result[1]){
  case 0x40:
    *irq_level = 15;
    break;
  case 0x20:
    *irq_level = 14;
    break;
  case 0x8:
    *irq_level = 12;
    break;
  case 0x4:
    *irq_level = 11;
    break;
  case 0x2:
    *irq_level = 10;
    break;
  case 0x1:
    *irq_level = 9;
    break;
  default:
    printk("Unable to determine Adaptec IRQ level.  Disabling board\n");
    return -1;
  };
  return 0;
}

/* This function should only be called for 1542C boards - we can detect
   the special firmware settings and unlock the board */

static int aha1542_mbenable(int base)
{
  static unchar mbenable_cmd[3];
  static unchar mbenable_result[2];
  int retval;
  
  retval = BIOS_TRANSLATION_6432;

  mbenable_cmd[0]=CMD_EXTBIOS;
  aha1542_out(base,mbenable_cmd,1);
  aha1542_in(base,mbenable_result,2);
  WAIT(INTRFLAGS(base),INTRMASK,HACC,0);
  aha1542_intr_reset(base);
  
  if ((mbenable_result[0] & 0x08) || mbenable_result[1]) {
     mbenable_cmd[0]=CMD_MBENABLE;
     mbenable_cmd[1]=0;
     mbenable_cmd[2]=mbenable_result[1];
     if(mbenable_result[1] & 1) retval = BIOS_TRANSLATION_25563;
     aha1542_out(base,mbenable_cmd,3);
     WAIT(INTRFLAGS(base),INTRMASK,HACC,0);
  };
  while(0) {
fail:
    printk("aha1542_mbenable: Mailbox init failed\n");
  }
aha1542_intr_reset(base);
return retval;
}

/* Query the board to find out if it is a 1542 or a 1740, or whatever. */
static int aha1542_query(int base_io, int * transl)
{
  unchar inquiry_cmd[] = {CMD_INQUIRY };
  unchar inquiry_result[4];
  int i;
  i = inb(STATUS(base_io));
  if (i & DF) {
    i = inb(DATA(base_io));
  };
  aha1542_out(base_io, inquiry_cmd, 1);
  aha1542_in(base_io, inquiry_result, 4);
  WAIT(INTRFLAGS(base_io), INTRMASK, HACC, 0);
  while (0) {
  fail:
    printk("aha1542_detect: query card type\n");
  }
  aha1542_intr_reset(base_io);

  *transl = BIOS_TRANSLATION_6432; /* Default case */

/* For an AHA1740 series board, we ignore the board since there is a
   hardware bug which can lead to wrong blocks being returned if the board
   is operating in the 1542 emulation mode.  Since there is an extended mode
   driver, we simply ignore the board and let the 1740 driver pick it up.
*/

  if (inquiry_result[0] == 0x43) {
    printk("aha1542.c: Emulation mode not supported for AHA 174N hardware.\n");
    return 1;
  };

  /* 1542C returns 0x44, 1542CF returns 0x45 */
  if (inquiry_result[0] == 0x44 || inquiry_result[0] == 0x45)
    { /* Detect 1542C  */
      *transl = aha1542_mbenable(base_io);
    };
  return 0;
}


/* return non-zero on detection */
int aha1542_detect(int hostnum)
{
    unsigned char dma_chan;
    unsigned char irq_level;
    unsigned int base_io;
    int trans;
    struct Scsi_Host * shpnt = NULL;
    int count = 0;
    int indx;

    DEB(printk("aha1542_detect: \n"));
    
    for(indx = 0; indx < sizeof(bases)/sizeof(bases[0]); indx++)
	    if(!check_region(bases[indx], 4)) { 
		    shpnt = scsi_register(hostnum,
					  sizeof(struct aha1542_hostdata));

		    if(!aha1542_test_port(bases[indx], shpnt)) goto unregister;


		    base_io = bases[indx];
		    
		    /* Set the Bus on/off-times as not to ruin floppy performance */
	    {
		    unchar oncmd[] = {CMD_BUSON_TIME, 7};
		    unchar offcmd[] = {CMD_BUSOFF_TIME, 5};
		    
		    aha1542_intr_reset(base_io);
		    aha1542_out(base_io, oncmd, 2);
		    WAIT(INTRFLAGS(base_io), INTRMASK, HACC, 0);
		    aha1542_intr_reset(base_io);
		    aha1542_out(base_io, offcmd, 2);
		    WAIT(INTRFLAGS(base_io), INTRMASK, HACC, 0);
		    while (0) {
		    fail:
			    printk("aha1542_detect: setting bus on/off-time failed\n");
		    }
		    aha1542_intr_reset(base_io);
	    }
		    if(aha1542_query(base_io, &trans))  goto unregister;
		    
		    if (aha1542_getconfig(base_io, &irq_level, &dma_chan) == -1)  goto unregister;
		    
		    printk("Configuring Adaptec at IO:%x, IRQ %d",base_io, irq_level);
		    if (dma_chan != 0xFF)
			    printk(", DMA priority %d", dma_chan);
		    printk("\n");
		    
		    DEB(aha1542_stat());
		    setup_mailboxes(base_io, shpnt);
		    
		    DEB(aha1542_stat());
		    
		    DEB(printk("aha1542_detect: enable interrupt channel %d\n", irq_level));
		    cli();
		    if (request_irq(irq_level,aha1542_intr_handle)) {
			    printk("Unable to allocate IRQ for adaptec controller.\n");
			    goto unregister;
		    }
		    
		    if (dma_chan != 0xFF) {
			    if (request_dma(dma_chan)) {
				    printk("Unable to allocate DMA channel for Adaptec.\n");
				    free_irq(irq_level);
				    goto unregister;
			    }
			    
			    if (dma_chan >= 5) {
				    outb((dma_chan - 4) | CASCADE, DMA_MODE_REG);
				    outb(dma_chan - 4, DMA_MASK_REG);
			    }
		    }
		    aha_host[irq_level - 9] = shpnt;
		    shpnt->io_port = base_io;
		    shpnt->dma_channel = dma_chan;
		    shpnt->irq = irq_level;
		    HOSTDATA(shpnt)->bios_translation  = trans;
		    if(trans == 2) 
		      printk("aha1542.c: Using extended bios translation\n");
		    HOSTDATA(shpnt)->aha1542_last_mbi_used  = (2*AHA1542_MAILBOXES - 1);
		    HOSTDATA(shpnt)->aha1542_last_mbo_used  = (AHA1542_MAILBOXES - 1);
		    memset(HOSTDATA(shpnt)->SCint, 0, sizeof(HOSTDATA(shpnt)->SCint));
		    sti();
#if 0
		    DEB(printk(" *** READ CAPACITY ***\n"));
		    
	    {
		    unchar buf[8];
		    static unchar cmd[] = {	READ_CAPACITY, 0, 0, 0, 0, 0, 0, 0, 0, 0};
		    int i;
		    
		    for (i = 0; i < sizeof(buf); ++i) buf[i] = 0x87;
		    for (i = 0; i < 2; ++i)
			    if (!aha1542_command(i, cmd, buf, sizeof(buf))) {
				    printk("aha_detect: LU %d sector_size %d device_size %d\n",
					   i, xscsi2int(buf+4), xscsi2int(buf));
			    }
	    }
		    
		    DEB(printk(" *** NOW RUNNING MY OWN TEST *** \n"));
		    
		    for (i = 0; i < 4; ++i)
		    {
			    unsigned char cmd[10];
			    static buffer[512];
			    
			    cmd[0] = READ_10;
			    cmd[1] = 0;
			    xany2scsi(cmd+2, i);
			    cmd[6] = 0;
			    cmd[7] = 0;
			    cmd[8] = 1;
			    cmd[9] = 0;
			    aha1542_command(0, cmd, buffer, 512);
		    }
#endif    
		    snarf_region(bases[indx], 4);  /* Register the IO ports that we use */
		    count++;
		    continue;
	    unregister:
		    scsi_unregister(shpnt, sizeof(struct aha1542_hostdata));
		    continue;
		    
	    };
    return count;
}

/* The abort command does not leave the device in a clean state where
   it is available to be used again.  Until this gets worked out, we will
   leave it commented out.  */

int aha1542_abort(Scsi_Cmnd * SCpnt, int i)
{
#if 0
    unchar ahacmd = CMD_START_SCSI;
    int mbo;
#endif
    DEB(printk("aha1542_abort\n"));
#if 0
    cli();
    for(mbo = 0; mbo < AHA1542_MAILBOXES; mbo++)
      if (SCpnt == HOSTDATA(SCpnt->host)->SCint[mbo]){
	mb[mbo].status = 2;  /* Abort command */
	aha1542_out(&ahacmd, 1);		/* start scsi command */
	sti();
	break;
      };
#endif
    return 0;
}

/* We do not implement a reset function here, but the upper level code assumes
   that it will get some kind of response for the command in SCpnt.  We must
   oblige, or the command will hang the scsi system */

int aha1542_reset(Scsi_Cmnd * SCpnt)
{
    DEB(printk("aha1542_reset called\n"));
    if(SCpnt) SCpnt->flags |= NEEDS_JUMPSTART;
    return 0;
}

#ifdef CONFIG_BLK_DEV_SD
#include "sd.h"
#endif

int aha1542_biosparam(int size, int dev, int * ip)
{
  int translation_algorithm;
#ifdef CONFIG_BLK_DEV_SD
  Scsi_Device *disk;

  disk = rscsi_disks[MINOR(dev) >> 4].device;
  translation_algorithm = HOSTDATA(disk->host)->bios_translation;
  /* Should this be > 1024, or >= 1024?  Enquiring minds want to know. */
  if((size>>11) > 1024 && translation_algorithm == 2) {
    /* Please verify that this is the same as what DOS returns */
    ip[0] = 255;
    ip[1] = 63;
    ip[2] = size /255/63;
  } else {
    ip[0] = 64;
    ip[1] = 32;
    ip[2] = size >> 11;
  };
/*  if (ip[2] >= 1024) ip[2] = 1024; */
#endif
  return 0;
}
