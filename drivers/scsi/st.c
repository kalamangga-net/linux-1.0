/*
  SCSI Tape Driver for Linux

  Version 0.02 for Linux 0.98.4 and Eric Youngdale's new scsi driver

  History:
  Rewritten from Dwayne Forsyth's SCSI tape driver by Kai Makisara.
  Contribution and ideas from several people including Eric Youngdale and
  Wolfgang Denk.

  Features:
  - support for different block sizes and internal buffering
  - support for fixed and variable block size (within buffer limit;
    blocksize set to zero)
  - *nix-style ioctl with codes from mtio.h from the QIC-02 driver by
    Hennus Bergman (command MTSETBLK added)
  - character device
  - rewind and non-rewind devices
  - capability to handle several tape drives simultaneously
  - one buffer if one drive, two buffers if more than one drive (limits the
    number of simultaneously open drives to two)
  - write behind
  - seek and tell (Tandberg compatible and SCSI-2)

  Devices:
  Autorewind devices have minor numbers equal to the tape numbers (0 > ).
  Nonrewind device has the minor number equal to tape number + 128.

  Problems:
  The end of media detection works correctly in writing only if the drive
  writes the buffer contents after the early-warning mark. If you want to
  be sure that EOM is reported correctly, you should uncomment the line
  defining ST_NO_DELAYED_WRITES. Note that when delayed writes are disabled
  each write byte count must be an integral number of blocks.

  Copyright 1992, 1993 Kai Makisara
		 email makisara@vtinsx.ins.vtt.fi or Kai.Makisara@vtt.fi

  Last modified: Thu Nov 25 21:49:02 1993 by root@kai.home
*/

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/mtio.h>
#include <linux/ioctl.h>
#include <linux/fcntl.h>
#include <asm/segment.h>
#include <asm/system.h>

#define MAJOR_NR SCSI_TAPE_MAJOR
#include "../block/blk.h"
#include "scsi.h"
#include "scsi_ioctl.h"
#include "st.h"
#include "constants.h"

/* Uncomment the following if you want the rewind, etc. commands return
   before command completion. */
/* #define ST_NOWAIT */

/* Uncomment the following if you want the tape to be positioned correctly
   within file after close (the tape is positioned correctly with respect
   to the filemarks even wihout ST_IN_FILE_POS defined */
/* #define ST_IN_FILE_POS */

/* Uncomment the following if you want recovered write errors to be
   fatal. */
/* #define ST_RECOVERED_WRITE_FATAL */

/* Uncomment the following if you want all data from a write command to
   be written to tape before the command returns. Disables write-behind. */
/* #define ST_NO_DELAYED_WRITES */

/* Number of ST_BLOCK_SIZE blocks in the buffers */
#define ST_BUFFER_BLOCKS 64
/* Write-behind can be disabled by setting ST_WRITE_THRESHOLD_BLOCKS equal
   to or larger than ST_BUFFER_BLOCKS */
#define ST_WRITE_THRESHOLD_BLOCKS 60
#define ST_BLOCK_SIZE 512
#define ST_BUFFER_SIZE (ST_BUFFER_BLOCKS * ST_BLOCK_SIZE)
#define ST_WRITE_THRESHOLD (ST_WRITE_THRESHOLD_BLOCKS * ST_BLOCK_SIZE)

#ifdef ST_NO_DELAYED_WRITES
#undef ST_WRITE_THRESHOLD_BLOCKS
#define ST_WRITE_THRESHOLD_BLOCKS ST_BUFFER_BLOCKS
#endif

/* The buffer size should fit into the 24 bits reserved for length in the
   6-byte SCSI read and write commands. */
#if ST_BUFFER_SIZE >= (2 << 24 - 1)
#error "Buffer size should not exceed (2 << 24 - 1) bytes!"
#endif

/* #define DEBUG */

#define MAX_RETRIES 0
#define MAX_READY_RETRIES 5
#define NO_TAPE  NOT_READY

#define ST_TIMEOUT 9000
#define ST_LONG_TIMEOUT 200000

static int st_nbr_buffers;
static ST_buffer *st_buffers[2];

static Scsi_Tape * scsi_tapes;
int NR_ST=0;
int MAX_ST=0;

static int st_int_ioctl(struct inode * inode,struct file * file,
	     unsigned int cmd_in, unsigned long arg);




/* Convert the result to success code */
	static int
st_chk_result(Scsi_Cmnd * SCpnt)
{
  int dev = SCpnt->request.dev;
  int result = SCpnt->result;
  unsigned char * sense = SCpnt->sense_buffer;
  char *stp;

  if (!result && SCpnt->sense_buffer[0] == 0)
    return 0;
#ifdef DEBUG
  printk("st%d: Error: %x\n", dev, result);
  print_sense("st", SCpnt);
#endif
/*  if ((sense[0] & 0x70) == 0x70 &&
       ((sense[2] & 0x80) ))
    return 0; */
  if ((sense[0] & 0x70) == 0x70 &&
      sense[2] == RECOVERED_ERROR
#ifdef ST_RECOVERED_WRITE_FATAL
      && SCpnt->cmnd[0] != WRITE_6
      && SCpnt->cmnd[0] != WRITE_FILEMARKS
#endif
      ) {
    scsi_tapes[dev].recover_count++;
    if (SCpnt->cmnd[0] == READ_6)
      stp = "read";
    else if (SCpnt->cmnd[0] == WRITE_6)
      stp = "write";
    else
      stp = "ioctl";
    printk("st%d: Recovered %s error (%d).\n", dev, stp,
	   scsi_tapes[dev].recover_count);
    return 0;
  }
  return (-EIO);
}


/* Wakeup from interrupt */
	static void
st_sleep_done (Scsi_Cmnd * SCpnt)
{
  int st_nbr, remainder;
  Scsi_Tape * STp;

  if ((st_nbr = SCpnt->request.dev) < NR_ST && st_nbr >= 0) {
    STp = &(scsi_tapes[st_nbr]);
    if ((STp->buffer)->writing &&
	(SCpnt->sense_buffer[0] & 0x70) == 0x70 &&
	(SCpnt->sense_buffer[2] & 0x40)) {
      /* EOM at write-behind, has all been written? */
      if ((SCpnt->sense_buffer[0] & 0x80) != 0)
	remainder = (SCpnt->sense_buffer[3] << 24) |
	      (SCpnt->sense_buffer[4] << 16) |
		(SCpnt->sense_buffer[5] << 8) | SCpnt->sense_buffer[6];
      else
	remainder = 0;
      if ((SCpnt->sense_buffer[2] & 0x0f) == VOLUME_OVERFLOW ||
	  remainder > 0)
	(STp->buffer)->last_result = SCpnt->result; /* Error */
      else
	(STp->buffer)->last_result = INT_MAX; /* OK */
    }
    else
      (STp->buffer)->last_result = SCpnt->result;
    (STp->buffer)->last_result_fatal = st_chk_result(SCpnt);
    if ((STp->buffer)->writing)
      SCpnt->request.dev = -1;
    else
      SCpnt->request.dev = 0xffff;
    if ((STp->buffer)->writing <= 0)
      wake_up( &(STp->waiting) );
  }
#ifdef DEBUG
  else
    printk("st?: Illegal interrupt device %x\n", st_nbr);
#endif
}


#if ST_WRITE_THRESHOLD_BLOCKS < ST_BUFFER_BLOCKS
/* Handle the write-behind checking */
	static void
write_behind_check(int dev)
{
  Scsi_Tape * STp;
  ST_buffer * STbuffer;

  STp = &(scsi_tapes[dev]);
  STbuffer = STp->buffer;

  cli();
  if (STbuffer->last_result < 0) {
    STbuffer->writing = (- STbuffer->writing);
    sleep_on( &(STp->waiting) );
    STbuffer->writing = (- STbuffer->writing);
  }
  sti();

  if (STbuffer->writing < STbuffer->buffer_bytes)
    memcpy(STbuffer->b_data,
	   STbuffer->b_data + STbuffer->writing,
	   STbuffer->buffer_bytes - STbuffer->writing);
  STbuffer->buffer_bytes -= STbuffer->writing;
  STbuffer->writing = 0;

  return;
}
#endif


/* Flush the write buffer (never need to write if variable blocksize). */
	static int
flush_write_buffer(int dev)
{
  int offset, transfer, blks;
  int result;
  unsigned char cmd[10];
  Scsi_Cmnd *SCpnt;
  Scsi_Tape *STp = &(scsi_tapes[dev]);

#if ST_WRITE_THRESHOLD_BLOCKS < ST_BUFFER_BLOCKS
  if ((STp->buffer)->writing) {
    write_behind_check(dev);
    if ((STp->buffer)->last_result_fatal) {
#ifdef DEBUG
      printk("st%d: Async write error %x.\n", dev,
	     (STp->buffer)->last_result);
#endif
      if ((STp->buffer)->last_result == INT_MAX)
	return (-ENOSPC);
      return (-EIO);
    }
  }
#endif

  result = 0;
  if (STp->dirty == 1) {
    SCpnt = allocate_device(NULL, (STp->device)->index, 1);

    offset = (STp->buffer)->buffer_bytes;
    transfer = ((offset + STp->block_size - 1) /
		STp->block_size) * STp->block_size;
#ifdef DEBUG
    printk("st%d: Flushing %d bytes.\n", dev, transfer);
#endif
    memset((STp->buffer)->b_data + offset, 0, transfer - offset);

    SCpnt->sense_buffer[0] = 0;
    memset(cmd, 0, 10);
    cmd[0] = WRITE_6;
    cmd[1] = 1;
    blks = transfer / STp->block_size;
    cmd[2] = blks >> 16;
    cmd[3] = blks >> 8;
    cmd[4] = blks;
    SCpnt->request.dev = dev;
    scsi_do_cmd (SCpnt,
		 (void *) cmd, (STp->buffer)->b_data, transfer,
		 st_sleep_done, ST_TIMEOUT, MAX_RETRIES);

    if (SCpnt->request.dev == dev) sleep_on( &(STp->waiting) );

    if ((STp->buffer)->last_result_fatal != 0) {
      printk("st%d: Error on flush.\n", dev);
      if ((SCpnt->sense_buffer[0] & 0x70) == 0x70 &&
	  (SCpnt->sense_buffer[2] & 0x40) &&
	  (SCpnt->sense_buffer[2] & 0x0f) != VOLUME_OVERFLOW) {
	STp->dirty = 0;
	(STp->buffer)->buffer_bytes = 0;
	result = (-ENOSPC);
      }
      else
	result = (-EIO);
    }
    else {
      STp->dirty = 0;
      (STp->buffer)->buffer_bytes = 0;
    }
    SCpnt->request.dev = -1;  /* Mark as not busy */
  }
  return result;
}


/* Flush the tape buffer. The tape will be positioned correctly unless
   seek_next is true. */
	static int
flush_buffer(struct inode * inode, struct file * filp, int seek_next)
{
  int dev;
  int backspace, result;
  Scsi_Tape * STp;
  ST_buffer * STbuffer;

  dev = MINOR(inode->i_rdev) & 127;
  STp = &(scsi_tapes[dev]);
  STbuffer = STp->buffer;

  if (STp->rw == ST_WRITING)  /* Writing */
    return flush_write_buffer(dev);

  if (STp->block_size == 0)
    return 0;

  backspace = ((STp->buffer)->buffer_bytes +
    (STp->buffer)->read_pointer) / STp->block_size -
      ((STp->buffer)->read_pointer + STp->block_size - 1) /
	STp->block_size;
  (STp->buffer)->buffer_bytes = 0;
  (STp->buffer)->read_pointer = 0;
  result = 0;
  if (!seek_next) {
    if ((STp->eof == ST_FM) && !STp->eof_hit) {
      result = st_int_ioctl(inode, filp, MTBSF, 1); /* Back over the EOF hit */
      if (!result) {
	STp->eof = ST_NOEOF;
	STp->eof_hit = 0;
      }
    }
    if (!result && backspace > 0)
      result = st_int_ioctl(inode, filp, MTBSR, backspace);
  }
  return result;

}


/* Open the device */
	static int
scsi_tape_open(struct inode * inode, struct file * filp)
{
    int dev;
    unsigned short flags;
    int i;
    unsigned char cmd[10];
    Scsi_Cmnd * SCpnt;
    Scsi_Tape * STp;

    dev = MINOR(inode->i_rdev) & 127;
    if (dev >= NR_ST)
      return (-ENODEV);
    STp = &(scsi_tapes[dev]);
    if (STp->in_use) {
      printk("st%d: Device already in use.\n", dev);
      return (-EBUSY);
    }

    /* Allocate buffer for this user */
    for (i=0; i < st_nbr_buffers; i++)
      if (!st_buffers[i]->in_use)
	break;
    if (i >= st_nbr_buffers) {
      printk("st%d: No free buffers.\n", dev);
      return (-EBUSY);
    }
    STp->buffer = st_buffers[i];
    (STp->buffer)->in_use = 1;
    (STp->buffer)->writing = 0;
    STp->in_use = 1;

    flags = filp->f_flags;
    STp->write_prot = ((flags & O_ACCMODE) == O_RDONLY);

    STp->dirty = 0;
    STp->rw = ST_IDLE;
    STp->eof = ST_NOEOF;
    STp->eof_hit = 0;
    STp->recover_count = 0;

    SCpnt = allocate_device(NULL, (STp->device)->index, 1);
    if (!SCpnt) {
      printk("st%d: Tape request not allocated", dev);
      return (-EBUSY);
    }

    SCpnt->sense_buffer[0]=0;
    memset ((void *) &cmd[0], 0, 10);
    cmd[0] = TEST_UNIT_READY;
    SCpnt->request.dev = dev;
    scsi_do_cmd(SCpnt,
                (void *) cmd, (void *) (STp->buffer)->b_data,
                ST_BLOCK_SIZE, st_sleep_done, ST_LONG_TIMEOUT,
		MAX_READY_RETRIES);

    if (SCpnt->request.dev == dev) sleep_on( &(STp->waiting) );

    if ((SCpnt->sense_buffer[0] & 0x70) == 0x70 &&
	(SCpnt->sense_buffer[2] & 0x0f) == UNIT_ATTENTION) { /* New media? */
      SCpnt->sense_buffer[0]=0;
      memset ((void *) &cmd[0], 0, 10);
      cmd[0] = TEST_UNIT_READY;
      SCpnt->request.dev = dev;
      scsi_do_cmd(SCpnt,
		  (void *) cmd, (void *) (STp->buffer)->b_data,
		  ST_BLOCK_SIZE, st_sleep_done, ST_LONG_TIMEOUT,
		  MAX_READY_RETRIES);

      if (SCpnt->request.dev == dev) sleep_on( &(STp->waiting) );
    }

    if ((STp->buffer)->last_result_fatal != 0) {
      if ((SCpnt->sense_buffer[0] & 0x70) == 0x70 &&
	  (SCpnt->sense_buffer[2] & 0x0f) == NO_TAPE)
	printk("st%d: No tape.\n", dev);
      else
	printk("st%d: Error %x.\n", dev, SCpnt->result);
      (STp->buffer)->in_use = 0;
      STp->in_use = 0;
      SCpnt->request.dev = -1;  /* Mark as not busy */
      return (-EIO);
    }

    SCpnt->sense_buffer[0]=0;
    memset ((void *) &cmd[0], 0, 10);
    cmd[0] = READ_BLOCK_LIMITS;
    SCpnt->request.dev = dev;
    scsi_do_cmd(SCpnt,
                (void *) cmd, (void *) (STp->buffer)->b_data,
                ST_BLOCK_SIZE, st_sleep_done, ST_TIMEOUT, MAX_READY_RETRIES);

    if (SCpnt->request.dev == dev) sleep_on( &(STp->waiting) );

    if (!SCpnt->result && !SCpnt->sense_buffer[0]) {
      STp->max_block = ((STp->buffer)->b_data[1] << 16) |
	((STp->buffer)->b_data[2] << 8) | (STp->buffer)->b_data[3];
      STp->min_block = ((STp->buffer)->b_data[4] << 8) |
	(STp->buffer)->b_data[5];
#ifdef DEBUG
      printk("st%d: Block limits %d - %d bytes.\n", dev, STp->min_block,
	     STp->max_block);
#endif
    }
    else {
      STp->min_block = STp->max_block = (-1);
#ifdef DEBUG
      printk("st%d: Can't read block limits.\n", dev);
#endif
    }

    SCpnt->sense_buffer[0]=0;
    memset ((void *) &cmd[0], 0, 10);
    cmd[0] = MODE_SENSE;
    cmd[4] = 12;
    SCpnt->request.dev = dev;
    scsi_do_cmd(SCpnt,
                (void *) cmd, (void *) (STp->buffer)->b_data,
                ST_BLOCK_SIZE, st_sleep_done, ST_TIMEOUT, MAX_READY_RETRIES);

    if (SCpnt->request.dev == dev) sleep_on( &(STp->waiting) );

    if ((STp->buffer)->last_result_fatal != 0) {
#ifdef DEBUG
      printk("st%d: No Mode Sense.\n", dev);
#endif
      (STp->buffer)->b_data[2] =
      (STp->buffer)->b_data[3] = 0;
    }
    SCpnt->request.dev = -1;  /* Mark as not busy */

#ifdef DEBUG
    printk("st%d: Mode sense. Length %d, medium %x, WBS %x, BLL %d\n", dev,
	   (STp->buffer)->b_data[0], (STp->buffer)->b_data[1],
	   (STp->buffer)->b_data[2], (STp->buffer)->b_data[3]);
#endif

    if ((STp->buffer)->b_data[3] >= 8) {
      STp->drv_buffer = ((STp->buffer)->b_data[2] >> 4) & 7;
      STp->density = (STp->buffer)->b_data[4];
      STp->block_size = (STp->buffer)->b_data[9] * 65536 +
	(STp->buffer)->b_data[10] * 256 + (STp->buffer)->b_data[11];
#ifdef DEBUG
      printk(
	"st%d: Density %x, tape length: %x, blocksize: %d, drv buffer: %d\n",
	     dev, STp->density, (STp->buffer)->b_data[5] * 65536 +
	     (STp->buffer)->b_data[6] * 256 + (STp->buffer)->b_data[7],
	     STp->block_size, STp->drv_buffer);
#endif
      if (STp->block_size > ST_BUFFER_SIZE) {
	printk("st%d: Blocksize %d too large for buffer.\n", dev,
	       STp->block_size);
	(STp->buffer)->in_use = 0;
	STp->in_use = 0;
	return (-EIO);
      }

    }
    else
      STp->block_size = ST_BLOCK_SIZE;

    if (STp->block_size > 0) {
      (STp->buffer)->buffer_blocks = ST_BUFFER_SIZE / STp->block_size;
      (STp->buffer)->buffer_size =
	(STp->buffer)->buffer_blocks * STp->block_size;
    }
    else {
      (STp->buffer)->buffer_blocks = 1;
      (STp->buffer)->buffer_size = ST_BUFFER_SIZE;
    }
    (STp->buffer)->buffer_bytes = (STp->buffer)->read_pointer = 0;

#ifdef DEBUG
    printk("st%d: Block size: %d, buffer size: %d (%d blocks).\n", dev,
	   STp->block_size, (STp->buffer)->buffer_size,
	   (STp->buffer)->buffer_blocks);
#endif

    if ((STp->buffer)->b_data[2] & 0x80) {
      STp->write_prot = 1;
#ifdef DEBUG
      printk( "st%d: Write protected\n", dev);
#endif
    }

    return 0;
}


/* Close the device*/
	static void
scsi_tape_close(struct inode * inode, struct file * filp)
{
    int dev;
    int result;
    int rewind;
    static unsigned char cmd[10];
    Scsi_Cmnd * SCpnt;
    Scsi_Tape * STp;
   
    dev = MINOR(inode->i_rdev);
    rewind = (dev & 0x80) == 0;
    dev = dev & 127;
    STp = &(scsi_tapes[dev]);

    if ( STp->rw == ST_WRITING) {

      result = flush_write_buffer(dev);

#ifdef DEBUG
      printk("st%d: File length %d bytes.\n", dev, filp->f_pos);
#endif

      if (result == 0 || result == (-ENOSPC)) {
	SCpnt = allocate_device(NULL, (STp->device)->index, 1);

	SCpnt->sense_buffer[0] = 0;
	memset(cmd, 0, 10);
	cmd[0] = WRITE_FILEMARKS;
	cmd[4] = 1;
	SCpnt->request.dev = dev;
	scsi_do_cmd( SCpnt,
		    (void *) cmd, (void *) (STp->buffer)->b_data,
		    ST_BLOCK_SIZE, st_sleep_done, ST_TIMEOUT, MAX_RETRIES);

	if (SCpnt->request.dev == dev) sleep_on( &(STp->waiting) );

	if ((STp->buffer)->last_result_fatal != 0)
	  printk("st%d: Error on write filemark.\n", dev);

	SCpnt->request.dev = -1;  /* Mark as not busy */
      }

#ifdef DEBUG
      printk("st%d: Buffer flushed, EOF written\n", dev);
#endif
    }
    else if (!rewind) {
#ifndef ST_IN_FILE_POS
      if ((STp->eof == ST_FM) && !STp->eof_hit)
	st_int_ioctl(inode, filp, MTBSF, 1); /* Back over the EOF hit */
#else
      flush_buffer(inode, filp, 0);
#endif
    }

    if (rewind)
      st_int_ioctl(inode, filp, MTREW, 1);

    (STp->buffer)->in_use = 0;
    STp->in_use = 0;

    return;
}


/* Write command */
	static int
st_write(struct inode * inode, struct file * filp, char * buf, int count)
{
    int dev;
    int total, do_count, blks, retval, transfer;
    int write_threshold;
    static unsigned char cmd[10];
    char *b_point;
    Scsi_Cmnd * SCpnt;
    Scsi_Tape * STp;

    dev = MINOR(inode->i_rdev) & 127;
    STp = &(scsi_tapes[dev]);
#ifdef DEBUG
    if (!STp->in_use) {
      printk("st%d: Incorrect device.\n", dev);
      return (-EIO);
    }
#endif

    if (STp->write_prot)
      return (-EACCES);

    if (STp->block_size == 0 && count > ST_BUFFER_SIZE)
      return (-EOVERFLOW);

    if (STp->rw == ST_READING) {
      retval = flush_buffer(inode, filp, 0);
      if (retval)
	return retval;
      STp->rw = ST_WRITING;
    }

#if ST_WRITE_THRESHOLD_BLOCKS < ST_BUFFER_BLOCKS
    if ((STp->buffer)->writing) {
      write_behind_check(dev);
      if ((STp->buffer)->last_result_fatal) {
#ifdef DEBUG
	printk("st%d: Async write error %x.\n", dev,
	       (STp->buffer)->last_result);
#endif
	if ((STp->buffer)->last_result == INT_MAX) {
	  retval = (-ENOSPC);  /* All has been written */
	  STp->eof = ST_EOM_OK;
	}
	else
	  retval = (-EIO);
	return retval;
      }
    }
#endif

    if (STp->eof == ST_EOM_OK)
      return (-ENOSPC);
    else if (STp->eof == ST_EOM_ERROR)
      return (-EIO);

#ifdef ST_NO_DELAYED_WRITES
    if (STp->block_size != 0 && (count % STp->block_size) != 0)
      return (-EIO);   /* Write must be integral number of blocks */
    write_threshold = 1;
#else
    write_threshold = (STp->buffer)->buffer_size;
#endif

    SCpnt = allocate_device(NULL, (STp->device)->index, 1);

    total = count;

    memset(cmd, 0, 10);
    cmd[0] = WRITE_6;
    cmd[1] = (STp->block_size != 0);

    STp->rw = ST_WRITING;

    b_point = buf;
    while(
#if ST_WRITE_THRESHOLD_BLOCKS  < ST_BUFFER_BLOCKS
	  STp->block_size != 0 &&
	  ((STp->buffer)->buffer_bytes + count) >
	  write_threshold)
#else
	  (STp->block_size == 0 && count > 0) ||
	  ((STp->buffer)->buffer_bytes + count) >=
	  write_threshold)
#endif
    {
      if (STp->block_size == 0)
	do_count = count;
      else {
	do_count = (STp->buffer)->buffer_size - (STp->buffer)->buffer_bytes;
	if (do_count > count)
	  do_count = count;
      }
      memcpy_fromfs((STp->buffer)->b_data +
		    (STp->buffer)->buffer_bytes, b_point, do_count);

      if (STp->block_size == 0)
        blks = do_count;
      else
	blks = ((STp->buffer)->buffer_bytes + do_count) /
	  STp->block_size;
      cmd[2] = blks >> 16;
      cmd[3] = blks >> 8;
      cmd[4] = blks;
      SCpnt->sense_buffer[0] = 0;
      SCpnt->request.dev = dev;
      scsi_do_cmd (SCpnt,
		   (void *) cmd, (STp->buffer)->b_data,
		   (STp->buffer)->buffer_size,
		   st_sleep_done, ST_TIMEOUT, MAX_RETRIES);

      if (SCpnt->request.dev == dev) sleep_on( &(STp->waiting) );

      if ((STp->buffer)->last_result_fatal != 0) {
#ifdef DEBUG
	printk("st%d: Error on write:\n", dev);
#endif
	if ((SCpnt->sense_buffer[0] & 0x70) == 0x70 &&
	    (SCpnt->sense_buffer[2] & 0x40)) {
	  if (STp->block_size != 0 && (SCpnt->sense_buffer[0] & 0x80) != 0)
	    transfer = (SCpnt->sense_buffer[3] << 24) |
	      (SCpnt->sense_buffer[4] << 16) |
		(SCpnt->sense_buffer[5] << 8) | SCpnt->sense_buffer[6];
	  else if (STp->block_size == 0 &&
		   (SCpnt->sense_buffer[2] & 0x0f) == VOLUME_OVERFLOW)
	    transfer = do_count;
	  else
	    transfer = 0;
	  if (STp->block_size != 0)
	    transfer *= STp->block_size;
	  if (transfer <= do_count) {
	    filp->f_pos += do_count - transfer;
	    count -= do_count - transfer;
	    STp->eof = ST_EOM_OK;
	    retval = (-ENOSPC); /* EOM within current request */
#ifdef DEBUG
	    printk("st%d: EOM with %d bytes unwritten.\n",
		   dev, transfer);
#endif
	  }
	  else {
	    STp->eof = ST_EOM_ERROR;
	    retval = (-EIO); /* EOM for old data */
#ifdef DEBUG
	    printk("st%d: EOM with lost data.\n", dev);
#endif
	  }
	}
	else
	  retval = (-EIO);

	SCpnt->request.dev = -1;  /* Mark as not busy */
	(STp->buffer)->buffer_bytes = 0;
	STp->dirty = 0;
	if (count < total)
	  return total - count;
	else
	  return retval;
      }
      filp->f_pos += do_count;
      b_point += do_count;
      count -= do_count;
      (STp->buffer)->buffer_bytes = 0;
      STp->dirty = 0;
    }
    if (count != 0) {
      STp->dirty = 1;
      memcpy_fromfs((STp->buffer)->b_data +
		    (STp->buffer)->buffer_bytes,b_point,count);
      filp->f_pos += count;
      (STp->buffer)->buffer_bytes += count;
      count = 0;
    }

    if ((STp->buffer)->last_result_fatal != 0) {
      SCpnt->request.dev = -1;
      return (STp->buffer)->last_result_fatal;
    }

#if ST_WRITE_THRESHOLD_BLOCKS < ST_BUFFER_BLOCKS
    if ((STp->buffer)->buffer_bytes >= ST_WRITE_THRESHOLD ||
	STp->block_size == 0) {
      /* Schedule an asynchronous write */
      if (STp->block_size == 0)
	(STp->buffer)->writing = (STp->buffer)->buffer_bytes;
      else
	(STp->buffer)->writing = ((STp->buffer)->buffer_bytes /
	  STp->block_size) * STp->block_size;
      STp->dirty = 0;

      if (STp->block_size == 0)
	blks = (STp->buffer)->writing;
      else
	blks = (STp->buffer)->writing / STp->block_size;
      cmd[2] = blks >> 16;
      cmd[3] = blks >> 8;
      cmd[4] = blks;
      SCpnt->result = (STp->buffer)->last_result = -1;
      SCpnt->sense_buffer[0] = 0;
      SCpnt->request.dev = dev;
      scsi_do_cmd (SCpnt,
		   (void *) cmd, (STp->buffer)->b_data,
		   (STp->buffer)->writing,
		   st_sleep_done, ST_TIMEOUT, MAX_RETRIES);
    }
    else
#endif
      SCpnt->request.dev = -1;  /* Mark as not busy */

    return( total);
}   


/* Read command */
	static int
st_read(struct inode * inode, struct file * filp, char * buf, int count)
{
    int dev;
    int total;
    int transfer, blks, bytes;
    static unsigned char cmd[10];
    Scsi_Cmnd * SCpnt;
    Scsi_Tape * STp;

    dev = MINOR(inode->i_rdev) & 127;
    STp = &(scsi_tapes[dev]);
#ifdef DEBUG
    if (!STp->in_use) {
      printk("st%d: Incorrect device.\n", dev);
      return (-EIO);
    }
#endif

    if (STp->block_size == 0 && count > ST_BUFFER_SIZE)
      return (-EOVERFLOW);

    if (STp->rw == ST_WRITING) {
      transfer = flush_buffer(inode, filp, 0);
      if (transfer)
	return transfer;
      STp->rw = ST_READING;
    }

#ifdef DEBUG
    if (STp->eof != ST_NOEOF)
      printk("st%d: EOF flag up. Bytes %d\n", dev,
	     (STp->buffer)->buffer_bytes);
#endif
    if (((STp->buffer)->buffer_bytes == 0) &&
	STp->eof == ST_EOM_OK)  /* EOM or Blank Check */
      return (-EIO);

    STp->rw = ST_READING;

    SCpnt = allocate_device(NULL, (STp->device)->index, 1);

    for (total = 0; total < count; ) {

      if ((STp->buffer)->buffer_bytes == 0 &&
	  STp->eof == ST_NOEOF) {

	memset(cmd, 0, 10);
	cmd[0] = READ_6;
	cmd[1] = (STp->block_size != 0);
	if (STp->block_size == 0)
	  blks = bytes = count;
	else {
	  blks = (STp->buffer)->buffer_blocks;
	  bytes = blks * STp->block_size;
	}
	cmd[2] = blks >> 16;
	cmd[3] = blks >> 8;
	cmd[4] = blks;

	SCpnt->sense_buffer[0] = 0;
	SCpnt->request.dev = dev;
	scsi_do_cmd (SCpnt,
		     (void *) cmd, (STp->buffer)->b_data,
		     (STp->buffer)->buffer_size,
		     st_sleep_done, ST_TIMEOUT, MAX_RETRIES);

	if (SCpnt->request.dev == dev) sleep_on( &(STp->waiting) );

	(STp->buffer)->read_pointer = 0;
	STp->eof_hit = 0;

	if ((STp->buffer)->last_result_fatal) {
#ifdef DEBUG
	  printk("st%d: Sense: %2x %2x %2x %2x %2x %2x %2x %2x\n", dev,
		 SCpnt->sense_buffer[0], SCpnt->sense_buffer[1],
		 SCpnt->sense_buffer[2], SCpnt->sense_buffer[3],
		 SCpnt->sense_buffer[4], SCpnt->sense_buffer[5],
		 SCpnt->sense_buffer[6], SCpnt->sense_buffer[7]);
#endif
	  if ((SCpnt->sense_buffer[0] & 0x70) == 0x70) { /* extended sense */

	    if ((SCpnt->sense_buffer[2] & 0xe0) != 0) { /* EOF, EOM, or ILI */

	      if ((SCpnt->sense_buffer[0] & 0x80) != 0)
		transfer = (SCpnt->sense_buffer[3] << 24) |
		  (SCpnt->sense_buffer[4] << 16) |
		    (SCpnt->sense_buffer[5] << 8) | SCpnt->sense_buffer[6];
	      else
		transfer = 0;
	      if (STp->block_size == 0 &&
		  (SCpnt->sense_buffer[2] & 0x0f) == MEDIUM_ERROR)
		transfer = bytes;

	      if (SCpnt->sense_buffer[2] & 0x20) {
		if (STp->block_size == 0) {
		  if (transfer <= 0)
		    transfer = 0;
		  (STp->buffer)->buffer_bytes = count - transfer;
		}
		else {
		  printk("st%d: Incorrect block size.\n", dev);
		  SCpnt->request.dev = -1;  /* Mark as not busy */
		  return (-EIO);
		}
	      }
	      else if (SCpnt->sense_buffer[2] & 0x40) {
		STp->eof = ST_EOM_OK;
		if (STp->block_size == 0)
		  (STp->buffer)->buffer_bytes = count - transfer;
		else
		  (STp->buffer)->buffer_bytes =
		    ((STp->buffer)->buffer_blocks - transfer) *
		      STp->block_size;
#ifdef DEBUG
		printk("st%d: EOM detected (%d bytes read).\n", dev,
		       (STp->buffer)->buffer_bytes);
#endif
	      }
	      else if (SCpnt->sense_buffer[2] & 0x80) {
		STp->eof = ST_FM;
		if (STp->block_size == 0)
		  (STp->buffer)->buffer_bytes = 0;
		else
		  (STp->buffer)->buffer_bytes =
		    ((STp->buffer)->buffer_blocks - transfer) *
		      STp->block_size;
#ifdef DEBUG
		printk(
		 "st%d: EOF detected (%d bytes read, transferred %d bytes).\n",
		       dev, (STp->buffer)->buffer_bytes, total);
#endif
	      } /* end of EOF, EOM, ILI test */
	    }
	    else { /* nonzero sense key */
#ifdef DEBUG
	      printk("st%d: Tape error while reading.\n", dev);
#endif
	      SCpnt->request.dev = -1;
	      if (total)
		return total;
	      else
		return -EIO;
	    }
	  }
	  else {
	    transfer = (STp->buffer)->last_result_fatal;
	    SCpnt->request.dev = -1;  /* Mark as not busy */
	    return transfer;
	  }
	}
	else /* Read successful */
	  (STp->buffer)->buffer_bytes = bytes;

      } /* if ((STp->buffer)->buffer_bytes == 0 &&
	   STp->eof == ST_NOEOF) */

      if ((STp->buffer)->buffer_bytes > 0) {
#ifdef DEBUG
	if (STp->eof != ST_NOEOF)
	  printk("st%d: EOF up. Left %d, needed %d.\n", dev,
		 (STp->buffer)->buffer_bytes, count - total);
#endif
	transfer = (STp->buffer)->buffer_bytes < count - total ?
	  (STp->buffer)->buffer_bytes : count - total;
	memcpy_tofs(buf, (STp->buffer)->b_data +
		    (STp->buffer)->read_pointer,transfer);
	filp->f_pos += transfer;
	buf += transfer;
	total += transfer;
	(STp->buffer)->buffer_bytes -= transfer;
	(STp->buffer)->read_pointer += transfer;
      }
      else if (STp->eof != ST_NOEOF) {
	STp->eof_hit = 1;
	SCpnt->request.dev = -1;  /* Mark as not busy */
	if (total == 0 && STp->eof == ST_FM)
	  STp->eof = 0;
	if (total == 0 && STp->eof == ST_EOM_OK)
	  return (-EIO);  /* ST_EOM_ERROR not used in read */
	return total;
      }

      if (STp->block_size == 0)
	count = total;  /* Read only one variable length block */

    } /* for (total = 0; total < count; ) */

    SCpnt->request.dev = -1;  /* Mark as not busy */

    return total;
}


/* Internal ioctl function */
	static int
st_int_ioctl(struct inode * inode,struct file * file,
	     unsigned int cmd_in, unsigned long arg)
{
   int dev = MINOR(inode->i_rdev);
   int timeout = ST_LONG_TIMEOUT;
   long ltmp;
   int ioctl_result;
   unsigned char cmd[10];
   Scsi_Cmnd * SCpnt;
   Scsi_Tape * STp;

   dev = dev & 127;
   STp = &(scsi_tapes[dev]);

   memset(cmd, 0, 10);
   switch (cmd_in) {
     case MTFSF:
     case MTFSFM:
       cmd[0] = SPACE;
       cmd[1] = 0x01; /* Space FileMarks */
       cmd[2] = (arg >> 16);
       cmd[3] = (arg >> 8);
       cmd[4] = arg;
#ifdef DEBUG
       printk("st%d: Spacing tape forward over %d filemarks.\n", dev,
	      cmd[2] * 65536 + cmd[3] * 256 + cmd[4]);
#endif
       break; 
     case MTBSF:
     case MTBSFM:
       cmd[0] = SPACE;
       cmd[1] = 0x01; /* Space FileMarks */
       ltmp = (-arg);
       cmd[2] = (ltmp >> 16);
       cmd[3] = (ltmp >> 8);
       cmd[4] = ltmp;
#ifdef DEBUG
       if (cmd[2] & 0x80)
	 ltmp = 0xff000000;
       ltmp = ltmp | (cmd[2] << 16) | (cmd[3] << 8) | cmd[4];
       printk("st%d: Spacing tape backward over %d filemarks.\n", dev, (-ltmp));
#endif
       break; 
      case MTFSR:
       cmd[0] = SPACE;
       cmd[1] = 0x00; /* Space Blocks */
       cmd[2] = (arg >> 16);
       cmd[3] = (arg >> 8);
       cmd[4] = arg;
#ifdef DEBUG
       printk("st%d: Spacing tape forward %d blocks.\n", dev,
	      cmd[2] * 65536 + cmd[3] * 256 + cmd[4]);
#endif
       break; 
     case MTBSR:
       cmd[0] = SPACE;
       cmd[1] = 0x00; /* Space Blocks */
       ltmp = (-arg);
       cmd[2] = (ltmp >> 16);
       cmd[3] = (ltmp >> 8);
       cmd[4] = ltmp;
#ifdef DEBUG
       if (cmd[2] & 0x80)
	 ltmp = 0xff000000;
       ltmp = ltmp | (cmd[2] << 16) | (cmd[3] << 8) | cmd[4];
       printk("st%d: Spacing tape backward %d blocks.\n", dev, (-ltmp));
#endif
       break; 
     case MTWEOF:
       if (STp->write_prot)
	 return (-EACCES);
       cmd[0] = WRITE_FILEMARKS;
       cmd[2] = (arg >> 16);
       cmd[3] = (arg >> 8);
       cmd[4] = arg;
       timeout = ST_TIMEOUT;
#ifdef DEBUG
       printk("st%d: Writing %d filemarks.\n", dev,
	      cmd[2] * 65536 + cmd[3] * 256 + cmd[4]);
#endif
       break; 
     case MTREW:
       cmd[0] = REZERO_UNIT;
#ifdef ST_NOWAIT
       cmd[1] = 1;  /* Don't wait for completion */
       timeout = ST_TIMEOUT;
#endif
#ifdef DEBUG
       printk("st%d: Rewinding tape.\n", dev);
#endif
       break; 
     case MTOFFL:
       cmd[0] = START_STOP;
#ifdef ST_NOWAIT
       cmd[1] = 1;  /* Don't wait for completion */
       timeout = ST_TIMEOUT;
#endif
#ifdef DEBUG
       printk("st%d: Unloading tape.\n", dev);
#endif
       break; 
     case MTNOP:
#ifdef DEBUG
       printk("st%d: No op on tape.\n", dev);
#endif
       return 0;  /* Should do something ? */
       break;
     case MTRETEN:
       cmd[0] = START_STOP;
#ifdef ST_NOWAIT
       cmd[1] = 1;  /* Don't wait for completion */
       timeout = ST_TIMEOUT;
#endif
       cmd[4] = 3;
#ifdef DEBUG
       printk("st%d: Retensioning tape.\n", dev);
#endif
       break; 
     case MTEOM:
       cmd[0] = SPACE;
       cmd[1] = 3;
#ifdef DEBUG
       printk("st%d: Spacing to end of recorded medium.\n", dev);
#endif
       break; 
     case MTERASE:
       if (STp->write_prot)
	 return (-EACCES);
       cmd[0] = ERASE;
       cmd[1] = 1;  /* To the end of tape */
#ifdef DEBUG
       printk("st%d: Erasing tape.\n", dev);
#endif
       break;
     case MTSEEK:
       if ((STp->device)->scsi_level < SCSI_2) {
	 cmd[0] = QFA_SEEK_BLOCK;
	 cmd[2] = (arg >> 16);
	 cmd[3] = (arg >> 8);
	 cmd[4] = arg;
	 cmd[5] = 0;
       }
       else {
	 cmd[0] = SEEK_10;
	 cmd[1] = 4;
	 cmd[3] = (arg >> 24);
	 cmd[4] = (arg >> 16);
	 cmd[5] = (arg >> 8);
	 cmd[6] = arg;
       }
#ifdef ST_NOWAIT
       cmd[1] |= 1;  /* Don't wait for completion */
       timeout = ST_TIMEOUT;
#endif
#ifdef DEBUG
       printk("st%d: Seeking tape to block %d.\n", dev, arg);
#endif
       break;
     case MTSETBLK:  /* Set block length */
     case MTSETDENSITY: /* Set tape density */
     case MTSETDRVBUFFER: /* Set drive buffering */
       if (STp->dirty || (STp->buffer)->buffer_bytes != 0)
	 return (-EIO);   /* Not allowed if data in buffer */
       if (cmd_in == MTSETBLK &&
	   arg != 0 &&
	   (arg < STp->min_block || arg > STp->max_block ||
	    arg > ST_BUFFER_SIZE)) {
	 printk("st%d: Illegal block size.\n", dev);
	 return (-EINVAL);
       }
       cmd[0] = MODE_SELECT;
       cmd[4] = 12;

       memset((STp->buffer)->b_data, 0, 12);
       if (cmd_in == MTSETDRVBUFFER)
	 (STp->buffer)->b_data[2] = (arg & 7) << 4;
       else
	 (STp->buffer)->b_data[2] = 
	   STp->drv_buffer << 4;
       (STp->buffer)->b_data[3] = 8;     /* block descriptor length */
       if (cmd_in == MTSETDENSITY)
	 (STp->buffer)->b_data[4] = arg;
       else
	 (STp->buffer)->b_data[4] = STp->density;
       if (cmd_in == MTSETBLK)
	 ltmp = arg;
       else
	 ltmp = STp->block_size;
       (STp->buffer)->b_data[9] = (ltmp >> 16);
       (STp->buffer)->b_data[10] = (ltmp >> 8);
       (STp->buffer)->b_data[11] = ltmp;
       timeout = ST_TIMEOUT;
#ifdef DEBUG
       if (cmd_in == MTSETBLK)
	 printk("st%d: Setting block size to %d bytes.\n", dev,
		(STp->buffer)->b_data[9] * 65536 +
		(STp->buffer)->b_data[10] * 256 +
		(STp->buffer)->b_data[11]);
       else if (cmd_in == MTSETDENSITY)
	 printk("st%d: Setting density code to %x.\n", dev,
		(STp->buffer)->b_data[4]);
       else
	 printk("st%d: Setting drive buffer code to %d.\n", dev,
		((STp->buffer)->b_data[2] >> 4) & 7);
#endif
       break;
     default:
       printk("st%d: Unknown st_ioctl command %x.\n", dev, cmd_in);
       return (-ENOSYS);
     }

   SCpnt = allocate_device(NULL, (STp->device)->index, 1);
   SCpnt->sense_buffer[0] = 0;
   SCpnt->request.dev = dev;
   scsi_do_cmd(SCpnt,
	       (void *) cmd, (void *) (STp->buffer)->b_data, ST_BLOCK_SIZE,
	       st_sleep_done, timeout, MAX_RETRIES);

   if (SCpnt->request.dev == dev) sleep_on( &(STp->waiting) );

   ioctl_result = (STp->buffer)->last_result_fatal;

   SCpnt->request.dev = -1;  /* Mark as not busy */

   if (!ioctl_result) {
     if (cmd_in == MTBSFM)
       ioctl_result = st_int_ioctl(inode, file, MTFSF, 1);
     else if (cmd_in == MTFSFM)
       ioctl_result = st_int_ioctl(inode, file, MTBSF, 1);
     else if (cmd_in == MTSETBLK) {
       STp->block_size = arg;
       if (arg != 0) {
	 (STp->buffer)->buffer_blocks =
	   ST_BUFFER_SIZE / STp->block_size;
	 (STp->buffer)->buffer_size =
	   (STp->buffer)->buffer_blocks * STp->block_size;
       }
       else {
	 (STp->buffer)->buffer_blocks = 1;
	 (STp->buffer)->buffer_size = ST_BUFFER_SIZE;
       }
       (STp->buffer)->buffer_bytes =
	 (STp->buffer)->read_pointer = 0;
     }
     else if (cmd_in == MTSETDRVBUFFER)
       STp->drv_buffer = arg;
     else if (cmd_in == MTSETDENSITY)
       STp->density = arg;
     if (cmd_in == MTEOM || cmd_in == MTWEOF) {
       STp->eof = ST_EOM_OK;
       STp->eof_hit = 0;
     }
     else if (cmd_in != MTSETBLK && cmd_in != MTNOP) {
       STp->eof = ST_NOEOF;
       STp->eof_hit = 0;
     }
   }

   return ioctl_result ;
}



/* The ioctl command */
	static int
st_ioctl(struct inode * inode,struct file * file,
	 unsigned int cmd_in, unsigned long arg)
{
   int dev = MINOR(inode->i_rdev);
   int i, cmd, result;
   struct mtop mtc;
   struct mtpos mt_pos;
   unsigned char scmd[10];
   Scsi_Cmnd *SCpnt;
   Scsi_Tape *STp;

   dev = dev & 127;
   STp = &(scsi_tapes[dev]);
#ifdef DEBUG
   if (!STp->in_use) {
     printk("st%d: Incorrect device.\n", dev);
     return (-EIO);
   }
#endif

   cmd = cmd_in & IOCCMD_MASK;
   if (cmd == (MTIOCTOP & IOCCMD_MASK)) {

     if (((cmd_in & IOCSIZE_MASK) >> IOCSIZE_SHIFT) != sizeof(mtc))
       return (-EINVAL);

     i = verify_area(VERIFY_WRITE, (void *)arg, sizeof(mtc));
     if (i)
        return i;

     memcpy_fromfs((char *) &mtc, (char *)arg, sizeof(struct mtop));

     i = flush_buffer(inode, file, mtc.mt_op == MTSEEK ||
		      mtc.mt_op == MTREW || mtc.mt_op == MTOFFL ||
		      mtc.mt_op == MTRETEN || mtc.mt_op == MTEOM);
     if (i < 0)
       return i;

     return st_int_ioctl(inode, file, mtc.mt_op, mtc.mt_count);
   }
   else if (cmd == (MTIOCGET & IOCCMD_MASK)) {

     if (((cmd_in & IOCSIZE_MASK) >> IOCSIZE_SHIFT) != sizeof(struct mtget))
       return (-EINVAL);
     i = verify_area(VERIFY_WRITE, (void *)arg, sizeof(struct mtget));
     if (i)
       return i;

     memcpy_tofs((char *)arg, (char *)(STp->buffer)->mt_status,
		 sizeof(struct mtget));
     return 0;
   }
   else if (cmd == (MTIOCPOS & IOCCMD_MASK)) {
#ifdef DEBUG
     printk("st%d: get tape position.\n", dev);
#endif
     if (((cmd_in & IOCSIZE_MASK) >> IOCSIZE_SHIFT) != sizeof(struct mtpos))
       return (-EINVAL);

     i = flush_buffer(inode, file, 0);
     if (i < 0)
       return i;

     i = verify_area(VERIFY_WRITE, (void *)arg, sizeof(struct mtpos));
     if (i)
       return i;

     SCpnt = allocate_device(NULL, (STp->device)->index, 1);

     SCpnt->sense_buffer[0]=0;
     memset (scmd, 0, 10);
     if ((STp->device)->scsi_level < SCSI_2) {
       scmd[0] = QFA_REQUEST_BLOCK;
       scmd[4] = 3;
     }
     else {
       scmd[0] = READ_POSITION;
       scmd[1] = 1;
     }
     SCpnt->request.dev = dev;
     SCpnt->sense_buffer[0] = 0;
     scsi_do_cmd(SCpnt,
		 (void *) scmd, (void *) (STp->buffer)->b_data,
		 ST_BLOCK_SIZE, st_sleep_done, ST_TIMEOUT, MAX_READY_RETRIES);

     if (SCpnt->request.dev == dev) sleep_on( &(STp->waiting) );
     
     if ((STp->buffer)->last_result_fatal != 0) {
       mt_pos.mt_blkno = (-1);
#ifdef DEBUG
       printk("st%d: Can't read tape position.\n", dev);
#endif
       result = (-EIO);
     }
     else {
       result = 0;
       if ((STp->device)->scsi_level < SCSI_2)
	 mt_pos.mt_blkno = ((STp->buffer)->b_data[0] << 16) 
	   + ((STp->buffer)->b_data[1] << 8) 
	     + (STp->buffer)->b_data[2];
       else
	 mt_pos.mt_blkno = ((STp->buffer)->b_data[4] << 24)
	   + ((STp->buffer)->b_data[5] << 16) 
	     + ((STp->buffer)->b_data[6] << 8) 
	       + (STp->buffer)->b_data[7];

     }

     SCpnt->request.dev = -1;  /* Mark as not busy */

     memcpy_tofs((char *)arg, (char *) (&mt_pos), sizeof(struct mtpos));
     return result;
   }
   else
     return scsi_ioctl(STp->device, cmd_in, (void *) arg);
}



static struct file_operations st_fops = {
   NULL,            /* lseek - default */
   st_read,         /* read - general block-dev read */
   st_write,        /* write - general block-dev write */
   NULL,            /* readdir - bad */
   NULL,            /* select */
   st_ioctl,        /* ioctl */
   NULL,            /* mmap */
   scsi_tape_open,  /* open */
   scsi_tape_close, /* release */
   NULL		    /* fsync */
};

void st_attach(Scsi_Device * SDp){
  scsi_tapes[NR_ST++].device = SDp;
  if(NR_ST > MAX_ST) panic ("scsi_devices corrupt (st)");
};

unsigned long st_init1(unsigned long mem_start, unsigned long mem_end){
  scsi_tapes = (Scsi_Tape *) mem_start;
  mem_start += MAX_ST * sizeof(Scsi_Tape);
  return mem_start;
};

/* Driver initialization */
unsigned long st_init(unsigned long mem_start, unsigned long mem_end)
{
  int i;

  if (register_chrdev(MAJOR_NR,"st",&st_fops)) {
    printk("Unable to get major %d for SCSI tapes\n",MAJOR_NR);
    return mem_start;
  }
  if (NR_ST == 0) return mem_start;

#ifdef DEBUG
  printk("st: Init tape.\n");
#endif

  for (i=0; i < NR_ST; ++i) {
    scsi_tapes[i].capacity = 0xfffff;
    scsi_tapes[i].dirty = 0;
    scsi_tapes[i].rw = ST_IDLE;
    scsi_tapes[i].eof = ST_NOEOF;
    scsi_tapes[i].waiting = NULL;
    scsi_tapes[i].in_use = 0;
    scsi_tapes[i].drv_buffer = 1;  /* Try buffering if no mode sense */
    scsi_tapes[i].density = 0;
  }


  /* Allocate the buffers */
  if (NR_ST == 1)
    st_nbr_buffers = 1;
  else
    st_nbr_buffers = 2;
  for (i=0; i < st_nbr_buffers; i++) {
    st_buffers[i] = (ST_buffer *) mem_start;
#ifdef DEBUG
    printk("st: Buffer address: %p\n", st_buffers[i]);
#endif
    mem_start += sizeof(ST_buffer) - 1 + ST_BUFFER_BLOCKS * ST_BLOCK_SIZE;
    st_buffers[i]->mt_status = (struct mtget *) mem_start;
    mem_start += sizeof(struct mtget);
    st_buffers[i]->in_use = 0;
    st_buffers[i]->writing = 0;

    /* "generic" status */
    memset((void *) st_buffers[i]->mt_status, 0, sizeof(struct mtget));
    st_buffers[i]->mt_status->mt_type = MT_ISSCSI1;
  }

  return mem_start;
}
