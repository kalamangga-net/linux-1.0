/* $Id: scsi_debug.c,v 1.1 1992/07/24 06:27:38 root Exp root $
 *  linux/kernel/scsi_debug.c
 *
 *  Copyright (C) 1992  Eric Youngdale
 *  Simulate a host adapter with 2 disks attached.  Do a lot of checking
 *  to make sure that we are not getting blocks mixed up, and panic if
 *  anything out of the ordinary is seen.
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/head.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/genhd.h>
#include <linux/fs.h>

#include <asm/system.h>
#include <asm/io.h>

#include "../block/blk.h"
#include "scsi.h"
#include "hosts.h"

/* Number of real scsi disks that will be detected ahead of time */
static int NR_REAL=-1;

#define MAJOR_NR SCSI_DISK_MAJOR
#define START_PARTITION 4
#define SCSI_DEBUG_TIMER 20
/* Number of jiffies to wait before completing a command */
#define DISK_SPEED     10
#define CAPACITY (0x80000)

static int starts[] = {4, 1000, 50000, CAPACITY, 0};
static int npart = 0;

#include "scsi_debug.h"
#ifdef DEBUG
#define DEB(x) x
#else
#define DEB(x)
#endif

#define VERIFY1_DEBUG(RW)			       			\
      if (bufflen != 1024) {printk("%d", bufflen); panic("(1)Bad bufflen");};			\
      start = 0;							\
      if ((SCpnt->request.dev & 0xf) != 0) start = starts[(SCpnt->request.dev & 0xf) - 1];		\
      if (bh){							\
	if (bh->b_size != 1024) panic ("Wrong bh size");	\
	if ((bh->b_blocknr << 1) + start != block)	       	\
	  {  printk("Wrong bh block# %d %d ",bh->b_blocknr, block);  \
	  panic ("Wrong bh block#");};  \
	if (bh->b_dev != SCpnt->request.dev) panic ("Bad bh target");\
      };

#if 0
/* This had been in the VERIFY_DEBUG macro, but it fails if there is already
   a disk on the system */
      if ((SCpnt->request.dev & 0xfff0) != ((target + NR_REAL) << 4) +(MAJOR_NR << 8)){	\
	printk("Dev #s %x %x ",SCpnt->request.dev, target);			\
	panic ("Bad target");};						\

#endif

#define VERIFY_DEBUG(RW)			       			\
      if (bufflen != 1024 && (!SCpnt->use_sg)) {printk("%x %d\n ",bufflen, SCpnt->use_sg); panic("Bad bufflen");};   	\
      start = 0;							\
      if ((SCpnt->request.dev & 0xf) > npart) panic ("Bad partition");	\
      if ((SCpnt->request.dev & 0xf) != 0) start = starts[(SCpnt->request.dev & 0xf) - 1];		\
      if (SCpnt->request.cmd != RW) panic ("Wrong  operation");		\
      if (SCpnt->request.sector + start != block) panic("Wrong block.");	\
      if (SCpnt->request.current_nr_sectors != 2 && (!SCpnt->use_sg)) panic ("Wrong # blocks");	\
      if (SCpnt->request.bh){							\
	if (SCpnt->request.bh->b_size != 1024) panic ("Wrong bh size");	\
	if ((SCpnt->request.bh->b_blocknr << 1) + start != block)	       	\
	  {  printk("Wrong bh block# %d %d ",SCpnt->request.bh->b_blocknr, block);  \
	  panic ("Wrong bh block#");};  \
	if (SCpnt->request.bh->b_dev != SCpnt->request.dev) panic ("Bad bh target");\
      };

static volatile void (*do_done[SCSI_DEBUG_MAILBOXES])(Scsi_Cmnd *) = {NULL, };
static int scsi_debug_host = 0;
extern void scsi_debug_interrupt();

volatile Scsi_Cmnd * SCint[SCSI_DEBUG_MAILBOXES] = {NULL,};
static volatile unsigned int timeout[SCSI_DEBUG_MAILBOXES] ={0,};

static char sense_buffer[128] = {0,};

static void scsi_dump(Scsi_Cmnd * SCpnt, int flag){
  int i;
#if 0
  unsigned char * pnt;
#endif
  unsigned int * lpnt;
  struct scatterlist * sgpnt = NULL;
  printk("use_sg: %d",SCpnt->use_sg);
  if (SCpnt->use_sg){
    sgpnt = (struct scatterlist *) SCpnt->buffer;
    for(i=0; i<SCpnt->use_sg; i++) {
      lpnt = (int *) sgpnt[i].alt_address;
      printk(":%x %x %d\n",sgpnt[i].alt_address, sgpnt[i].address, sgpnt[i].length);
      if (lpnt) printk(" (Alt %x) ",lpnt[15]);
    };
  } else {
    printk("nosg: %x %x %d\n",SCpnt->request.buffer, SCpnt->buffer,
	   SCpnt->bufflen);
    lpnt = (int *) SCpnt->request.buffer;
    if (lpnt) printk(" (Alt %x) ",lpnt[15]);
  };
  lpnt = (unsigned int *) SCpnt;
  for (i=0;i<sizeof(Scsi_Cmnd)/4+1; i++) {
    if ((i & 7) == 0) printk("\n");
    printk("%x ",*lpnt++);
  };
  printk("\n");
  if (flag == 0) return;
  lpnt = (unsigned int *) sgpnt[0].alt_address;
  for (i=0;i<sizeof(Scsi_Cmnd)/4+1; i++) {
    if ((i & 7) == 0) printk("\n");
    printk("%x ",*lpnt++);
  };
#if 0
  printk("\n");
  lpnt = (unsigned int *) sgpnt[0].address;
  for (i=0;i<sizeof(Scsi_Cmnd)/4+1; i++) {
    if ((i & 7) == 0) printk("\n");
    printk("%x ",*lpnt++);
  };
  printk("\n");
#endif
  printk("DMA free %d sectors.\n", dma_free_sectors);
}

int scsi_debug_queuecommand(Scsi_Cmnd * SCpnt, void (*done)(Scsi_Cmnd *))
{
    unchar *cmd = (unchar *) SCpnt->cmnd;
    struct partition * p;
    int block, start;
    struct buffer_head * bh = NULL;
    unsigned char * buff;
    int nbytes, sgcount;
    int scsi_debug_errsts;
    struct scatterlist * sgpnt;
    int target = SCpnt->target;
    int bufflen = SCpnt->request_bufflen;
    int i;
    sgcount = 0;
    sgpnt = NULL;

    DEB(if (target > 1) { SCpnt->result = DID_TIME_OUT << 16;done(SCpnt);return 0;});
    
    buff = (unsigned char *) SCpnt->request_buffer;

    if(target>=2 || SCpnt->lun != 0) {
      SCpnt->result =  DID_NO_CONNECT << 16;
      done(SCpnt);
      return 0;
    };
    
    switch(*cmd){
    case REQUEST_SENSE:
      printk("Request sense...\n");
#ifndef DEBUG
      { int i;
	printk("scsi_debug: Requesting sense buffer (%x %x %x %d):", SCpnt, buff, done, bufflen);
	for(i=0;i<12;i++) printk("%d ",sense_buffer[i]);
	printk("\n");
      };
#endif
      memset(buff, 0, bufflen);
      memcpy(buff, sense_buffer, bufflen);
      memset(sense_buffer, 0, sizeof(sense_buffer));
      SCpnt->result = 0;
      done(SCpnt); 
      return 0;
    case ALLOW_MEDIUM_REMOVAL:
      if(cmd[4]) printk("Medium removal inhibited...");
      else printk("Medium removal enabled...");
      scsi_debug_errsts = 0;
      break;
    case INQUIRY:
      printk("Inquiry...(%x %d)\n", buff, bufflen);
      memset(buff, 0, bufflen);
      buff[0] = TYPE_DISK;
      buff[1] = 0x80;  /* Removable disk */
      buff[2] = 1;
      memcpy(&buff[8],"Foo Inc",7);
      memcpy(&buff[16],"XYZZY",5);
      memcpy(&buff[32],"1",1);
      scsi_debug_errsts = 0;
      break;
    case TEST_UNIT_READY:
      printk("Test unit ready.\n");
      if (buff)
	memset(buff, 0, bufflen);
      scsi_debug_errsts = 0;
      break;
    case READ_CAPACITY:
      printk("Read Capacity\n");
      if(NR_REAL < 0) NR_REAL = (SCpnt->request.dev >> 4) & 0x0f;
      memset(buff, 0, bufflen);
      buff[0] = (CAPACITY >> 24);
      buff[1] = (CAPACITY >> 16) & 0xff;
      buff[2] = (CAPACITY >> 8) & 0xff;
      buff[3] = CAPACITY & 0xff;
      buff[6] = 2; /* 512 byte sectors */
      scsi_debug_errsts = 0;
      break;
    case READ_10:
    case READ_6:
#ifdef DEBUG
      printk("Read...");
#endif
      if ((*cmd) == READ_10)
	block = cmd[5] + (cmd[4] << 8) + (cmd[3] << 16) + (cmd[2] << 24); 
      else 
	block = cmd[3] + (cmd[2] << 8) + ((cmd[1] & 0x1f) << 16);
      VERIFY_DEBUG(READ);
      printk("(r%d)",SCpnt->request.nr_sectors);
      nbytes = bufflen;
      if(SCpnt->use_sg){
	sgcount = 0;
	sgpnt = (struct scatterlist *) buff;
	buff = sgpnt[sgcount].address;
	bufflen = sgpnt[sgcount].length;
	bh = SCpnt->request.bh;
      };
      scsi_debug_errsts = 0;
      do{
	VERIFY1_DEBUG(READ);
	memset(buff, 0, bufflen);
/* If this is block 0, then we want to read the partition table for this
   device.  Let's make one up */
	if(block == 0 && target == 0) {
	  *((unsigned short *) (buff+510)) = 0xAA55;
	  p = (struct partition* ) (buff + 0x1be);
	  npart = 0;
	  while(starts[npart+1]){
	    p->start_sect = starts[npart];
	    p->nr_sects = starts[npart+1] - starts [npart];
	    p->sys_ind = 0x81;  /* Linux partition */
	    p++;
	    npart++;
	  };
	  scsi_debug_errsts = 0;
	  break;
	};
#ifdef DEBUG
	if (SCpnt->use_sg) printk("Block %x (%d %d)\n",block, SCpnt->request.nr_sectors,
	       SCpnt->request.current_nr_sectors);
#endif
	/* Simulate a disk change */
	if(block == 0xfff0) {
	  sense_buffer[0] = 0x70;
	  sense_buffer[2] = UNIT_ATTENTION;
	  starts[0] += 10;
	  starts[1] += 10;
	  starts[2] += 10;
	 
#ifdef DEBUG
      { int i;
	printk("scsi_debug: Filling sense buffer:");
	for(i=0;i<12;i++) printk("%d ",sense_buffer[i]);
	printk("\n");
      };
#endif
	  scsi_debug_errsts = (COMMAND_COMPLETE << 8) | (CHECK_CONDITION << 1);
	  break;
	} /* End phony disk change code */
	memset(buff, 0, bufflen);
	memcpy(buff, &target, sizeof(target));
	memcpy(buff+sizeof(target), cmd, 24);
	memcpy(buff+60, &block, sizeof(block));
	memcpy(buff+64, SCpnt, sizeof(Scsi_Cmnd));
	nbytes -= bufflen;
	if(SCpnt->use_sg){
	  memcpy(buff+128, bh, sizeof(struct buffer_head));
	  block += bufflen >> 9;
	  bh = bh->b_reqnext;
	  sgcount++;
	  if (nbytes) {
	    if(!bh) panic("Too few blocks for linked request.");
	    buff = sgpnt[sgcount].address;
	    bufflen = sgpnt[sgcount].length;
	  };
	}
      } while(nbytes);
      if (SCpnt->use_sg && !scsi_debug_errsts)
	if(bh) scsi_dump(SCpnt, 0);
      break;
    case WRITE_10:
    case WRITE_6:
#ifdef DEBUG
      printk("Write\n");
#endif
      if ((*cmd) == WRITE_10)
	block = cmd[5] + (cmd[4] << 8) + (cmd[3] << 16) + (cmd[2] << 24); 
      else 
	block = cmd[3] + (cmd[2] << 8) + ((cmd[1] & 0x1f) << 16);
      VERIFY_DEBUG(WRITE);
      printk("(w%d)",SCpnt->request.nr_sectors);
      if (SCpnt->use_sg){
	if ((bufflen >> 9) != SCpnt->request.nr_sectors)
	  panic ("Trying to write wrong number of blocks\n");
	sgpnt = (struct scatterlist *) buff;
	buff = sgpnt[sgcount].address;
      };
#if 0
      if (block != *((unsigned long *) (buff+60))) {
	printk("%x %x :",block,  *((unsigned long *) (buff+60)));
	scsi_dump(SCpnt,1);
	panic("Bad block written.\n");
      };
#endif
      scsi_debug_errsts = 0;
      break;
     default:
      printk("Unknown command %d\n",*cmd);
      SCpnt->result =  DID_NO_CONNECT << 16;
      done(SCpnt);
      return 0;
    };

    cli();
    for(i=0;i<SCSI_DEBUG_MAILBOXES; i++){
      if (SCint[i] == 0) break;
    };

    if (i >= SCSI_DEBUG_MAILBOXES || SCint[i] != 0) 
      panic("Unable to find empty SCSI_DEBUG command slot.\n");

    SCint[i] = SCpnt;

    if (done) {
	DEB(printk("scsi_debug_queuecommand: now waiting for interrupt "););
	if (do_done[i])
	  printk("scsi_debug_queuecommand: Two concurrent queuecommand?\n");
	else
	  do_done[i] = done;
    }
    else
      printk("scsi_debug_queuecommand: done cant be NULL\n");

    timeout[i] = jiffies+DISK_SPEED;

/* If no timers active, then set this one */
    if ((timer_active & (1 << SCSI_DEBUG_TIMER)) == 0) {
      timer_table[SCSI_DEBUG_TIMER].expires = timeout[i];
      timer_active |= 1 << SCSI_DEBUG_TIMER;
    };

    SCpnt->result = scsi_debug_errsts;
    sti();

#if 0
    printk("Sending command (%d %x %d %d)...", i, done, timeout[i],jiffies);
#endif

    return 0;
}

volatile static int internal_done_flag = 0;
volatile static int internal_done_errcode = 0;
static void internal_done(Scsi_Cmnd * SCpnt)
{
    internal_done_errcode = SCpnt->result;
    ++internal_done_flag;
}

int scsi_debug_command(Scsi_Cmnd * SCpnt)
{
    DEB(printk("scsi_debug_command: ..calling scsi_debug_queuecommand\n"));
    scsi_debug_queuecommand(SCpnt, internal_done);

    while (!internal_done_flag);
    internal_done_flag = 0;
    return internal_done_errcode;
}

/* A "high" level interrupt handler.  This should be called once per jiffy
 to simulate a regular scsi disk.  We use a timer to do this. */

static void scsi_debug_intr_handle(void)
{
    Scsi_Cmnd * SCtmp;
    int i, pending;
    void (*my_done)(Scsi_Cmnd *); 
   int to;

    timer_table[SCSI_DEBUG_TIMER].expires = 0;
    timer_active &= ~(1 << SCSI_DEBUG_TIMER);

  repeat:
    cli();
    for(i=0;i<SCSI_DEBUG_MAILBOXES; i++) {
      if (SCint[i] == 0) continue;
      if (timeout[i] == 0) continue;
      if (timeout[i] <= jiffies) break;
    };

    if(i == SCSI_DEBUG_MAILBOXES){
      pending = INT_MAX;
      for(i=0;i<SCSI_DEBUG_MAILBOXES; i++) {
	if (SCint[i] == 0) continue;
	if (timeout[i] == 0) continue;
	if (timeout[i] <= jiffies) {sti(); goto repeat;};
	if (timeout[i] > jiffies) {
	  if (pending > timeout[i]) pending = timeout[i];
	  continue;
	};
      };
      if (pending && pending != INT_MAX) {
	timer_table[SCSI_DEBUG_TIMER].expires = 
	  (pending <= jiffies ? jiffies+1 : pending);
	timer_active |= 1 << SCSI_DEBUG_TIMER;
      };
      sti();
      return;
    };

    if(i < SCSI_DEBUG_MAILBOXES){
      timeout[i] = 0;
      my_done = do_done[i];
      do_done[i] = NULL;
      to = timeout[i];
      timeout[i] = 0;
      SCtmp = (Scsi_Cmnd *) SCint[i];
      SCint[i] = NULL;
      sti();

      if (!my_done) {
	printk("scsi_debug_intr_handle: Unexpected interrupt\n"); 
	return;
      }
      
#ifdef DEBUG
      printk("In intr_handle...");
      printk("...done %d %x %d %d\n",i , my_done, to, jiffies);
      printk("In intr_handle: %d %x %x\n",i, SCtmp, my_done);
#endif

      my_done(SCtmp);
#ifdef DEBUG
      printk("Called done.\n");
#endif
    };
    goto repeat;
}


int scsi_debug_detect(int hostnum)
{
    scsi_debug_host = hostnum;
    timer_table[SCSI_DEBUG_TIMER].fn = scsi_debug_intr_handle;
    timer_table[SCSI_DEBUG_TIMER].expires = 0;
    return 1;
}

int scsi_debug_abort(Scsi_Cmnd * SCpnt,int i)
{
    int j;
    void (*my_done)(Scsi_Cmnd *);
    DEB(printk("scsi_debug_abort\n"));
    SCpnt->result = i << 16;
    for(j=0;j<SCSI_DEBUG_MAILBOXES; j++) {
      if(SCpnt == SCint[j]) {
	my_done = do_done[j];
	my_done(SCpnt);
	cli();
	timeout[j] = 0;
	SCint[j] = NULL;
	do_done[j] = NULL;
	sti();
      };
    };
    return 0;
}

int scsi_debug_biosparam(int size, int* info){
  info[0] = 32;
  info[1] = 64;
  info[2] = (size + 2047) >> 11;
  if (info[2] >= 1024) info[2] = 1024;
  return 0;
}

int scsi_debug_reset(Scsi_Cmnd * SCpnt)
{
    int i;
    void (*my_done)(Scsi_Cmnd *);
    DEB(printk("scsi_debug_reset called\n"));
    for(i=0;i<SCSI_DEBUG_MAILBOXES; i++) {
      if (SCint[i] == NULL) continue;
      SCint[i]->result = DID_ABORT << 16;
      my_done = do_done[i];
      my_done(SCint[i]);
      cli();
      SCint[i] = NULL;
      do_done[i] = NULL;
      timeout[i] = 0;
      sti();
    };
    return 0;
}

char *scsi_debug_info(void)
{
    static char buffer[] = " ";			/* looks nicer without anything here */
    return buffer;
}


