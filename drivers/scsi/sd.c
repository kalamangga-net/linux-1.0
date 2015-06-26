/*
 *	sd.c Copyright (C) 1992 Drew Eckhardt 
 *	Linux scsi disk driver by
 *		Drew Eckhardt 
 *
 *	<drew@colorado.edu>
 *
 *       Modified by Eric Youngdale eric@tantalus.nrl.navy.mil to
 *       add scatter-gather, multiple outstanding request, and other
 *       enhancements.
 */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <asm/system.h>

#define MAJOR_NR SCSI_DISK_MAJOR
#include "../block/blk.h"
#include "scsi.h"
#include "hosts.h"
#include "sd.h"
#include "scsi_ioctl.h"
#include "constants.h"

#include <linux/genhd.h>

/*
static const char RCSid[] = "$Header:";
*/

#define MAX_RETRIES 5

/*
 *	Time out in seconds
 */

#define SD_TIMEOUT 300

struct hd_struct * sd;

int NR_SD=0;
int MAX_SD=0;
Scsi_Disk * rscsi_disks;
static int * sd_sizes;
static int * sd_blocksizes;

extern int sd_ioctl(struct inode *, struct file *, unsigned int, unsigned long);

static sd_init_onedisk(int);

static void requeue_sd_request (Scsi_Cmnd * SCpnt);

static int sd_open(struct inode * inode, struct file * filp)
{
        int target;
	target =  DEVICE_NR(MINOR(inode->i_rdev));

	if(target >= NR_SD || !rscsi_disks[target].device)
	  return -ENODEV;   /* No such device */
	
/* Make sure that only one process can do a check_change_disk at one time.
 This is also used to lock out further access when the partition table is being re-read. */

	while (rscsi_disks[target].device->busy);

	if(rscsi_disks[target].device->removable) {
	  check_disk_change(inode->i_rdev);

	  if(!rscsi_disks[target].device->access_count)
	    sd_ioctl(inode, NULL, SCSI_IOCTL_DOORLOCK, 0);
	};
	rscsi_disks[target].device->access_count++;
	return 0;
}

static void sd_release(struct inode * inode, struct file * file)
{
        int target;
	sync_dev(inode->i_rdev);

	target =  DEVICE_NR(MINOR(inode->i_rdev));

	rscsi_disks[target].device->access_count--;

	if(rscsi_disks[target].device->removable) {
	  if(!rscsi_disks[target].device->access_count)
	    sd_ioctl(inode, NULL, SCSI_IOCTL_DOORUNLOCK, 0);
	};
}

static void sd_geninit(void);

static struct file_operations sd_fops = {
	NULL,			/* lseek - default */
	block_read,		/* read - general block-dev read */
	block_write,		/* write - general block-dev write */
	NULL,			/* readdir - bad */
	NULL,			/* select */
	sd_ioctl,		/* ioctl */
	NULL,			/* mmap */
	sd_open,		/* open code */
	sd_release,		/* release */
	block_fsync		/* fsync */
};

static struct gendisk sd_gendisk = {
	MAJOR_NR,		/* Major number */
	"sd",		/* Major name */
	4,		/* Bits to shift to get real from partition */
	1 << 4,		/* Number of partitions per real */
	0,		/* maximum number of real */
	sd_geninit,	/* init function */
	NULL,		/* hd struct */
	NULL,	/* block sizes */
	0,		/* number */
	NULL,	/* internal */
	NULL		/* next */
};

static void sd_geninit (void)
{
	int i;

	for (i = 0; i < NR_SD; ++i)
		sd[i << 4].nr_sects = rscsi_disks[i].capacity;
	sd_gendisk.nr_real = NR_SD;
}

/*
	rw_intr is the interrupt routine for the device driver.  It will
	be notified on the end of a SCSI read / write, and
	will take on of several actions based on success or failure.
*/

static void rw_intr (Scsi_Cmnd *SCpnt)
{
  int result = SCpnt->result;
  int this_count = SCpnt->bufflen >> 9;

#ifdef DEBUG
  printk("sd%d : rw_intr(%d, %d)\n", MINOR(SCpnt->request.dev), SCpnt->host->host_no, result);
#endif

/*
  First case : we assume that the command succeeded.  One of two things will
  happen here.  Either we will be finished, or there will be more
  sectors that we were unable to read last time.
*/

  if (!result) {

#ifdef DEBUG
    printk("sd%d : %d sectors remain.\n", MINOR(SCpnt->request.dev), SCpnt->request.nr_sectors);
    printk("use_sg is %d\n ",SCpnt->use_sg);
#endif
    if (SCpnt->use_sg) {
      struct scatterlist * sgpnt;
      int i;
      sgpnt = (struct scatterlist *) SCpnt->buffer;
      for(i=0; i<SCpnt->use_sg; i++) {
#ifdef DEBUG
	printk(":%x %x %d\n",sgpnt[i].alt_address, sgpnt[i].address, sgpnt[i].length);
#endif
	if (sgpnt[i].alt_address) {
	  if (SCpnt->request.cmd == READ)
	    memcpy(sgpnt[i].alt_address, sgpnt[i].address, sgpnt[i].length);
	  scsi_free(sgpnt[i].address, sgpnt[i].length);
	};
      };
      scsi_free(SCpnt->buffer, SCpnt->sglist_len);  /* Free list of scatter-gather pointers */
    } else {
      if (SCpnt->buffer != SCpnt->request.buffer) {
#ifdef DEBUG
	printk("nosg: %x %x %d\n",SCpnt->request.buffer, SCpnt->buffer,
		   SCpnt->bufflen);
#endif	
	  if (SCpnt->request.cmd == READ)
	    memcpy(SCpnt->request.buffer, SCpnt->buffer,
		   SCpnt->bufflen);
	  scsi_free(SCpnt->buffer, SCpnt->bufflen);
      };
    };
/*
 * 	If multiple sectors are requested in one buffer, then
 *	they will have been finished off by the first command.  If
 *	not, then we have a multi-buffer command.
 */
    if (SCpnt->request.nr_sectors > this_count)
      {
	SCpnt->request.errors = 0;
	
	if (!SCpnt->request.bh)
	  {
#ifdef DEBUG
	    printk("sd%d : handling page request, no buffer\n",
		   MINOR(SCpnt->request.dev));
#endif
/*
  The SCpnt->request.nr_sectors field is always done in 512 byte sectors,
  even if this really isn't the case.
*/
	    panic("sd.c: linked page request (%lx %x)",
		  SCpnt->request.sector, this_count);
	  }
      }
    end_scsi_request(SCpnt, 1, this_count);
    requeue_sd_request(SCpnt);
    return;
  }

/* Free up any indirection buffers we allocated for DMA purposes. */
    if (SCpnt->use_sg) {
      struct scatterlist * sgpnt;
      int i;
      sgpnt = (struct scatterlist *) SCpnt->buffer;
      for(i=0; i<SCpnt->use_sg; i++) {
#ifdef DEBUG
	printk("err: %x %x %d\n",SCpnt->request.buffer, SCpnt->buffer,
		   SCpnt->bufflen);
#endif
	if (sgpnt[i].alt_address) {
	  scsi_free(sgpnt[i].address, sgpnt[i].length);
	};
      };
      scsi_free(SCpnt->buffer, SCpnt->sglist_len);  /* Free list of scatter-gather pointers */
    } else {
#ifdef DEBUG
      printk("nosgerr: %x %x %d\n",SCpnt->request.buffer, SCpnt->buffer,
		   SCpnt->bufflen);
#endif
      if (SCpnt->buffer != SCpnt->request.buffer)
	scsi_free(SCpnt->buffer, SCpnt->bufflen);
    };

/*
	Now, if we were good little boys and girls, Santa left us a request
	sense buffer.  We can extract information from this, so we
	can choose a block to remap, etc.
*/

        if (driver_byte(result) != 0) {
	  if (sugestion(result) == SUGGEST_REMAP) {
#ifdef REMAP
/*
	Not yet implemented.  A read will fail after being remapped,
	a write will call the strategy routine again.
*/
	    if rscsi_disks[DEVICE_NR(SCpnt->request.dev)].remap
	      {
		result = 0;
	      }
	    else
	      
#endif
	    }

	  if ((SCpnt->sense_buffer[0] & 0x7f) == 0x70) {
	    if ((SCpnt->sense_buffer[2] & 0xf) == UNIT_ATTENTION) {
	      if(rscsi_disks[DEVICE_NR(SCpnt->request.dev)].device->removable) {
	      /* detected disc change.  set a bit and quietly refuse	*/
	      /* further access.					*/
	      
		rscsi_disks[DEVICE_NR(SCpnt->request.dev)].device->changed = 1;
		end_scsi_request(SCpnt, 0, this_count);
		requeue_sd_request(SCpnt);
		return;
	      }
	    }
	  }
	  

/* 	If we had an ILLEGAL REQUEST returned, then we may have
performed an unsupported command.  The only thing this should be would
be a ten byte read where only a six byte read was supportted.  Also,
on a system where READ CAPACITY failed, we mave have read past the end
of the 	disk. 
*/

	  if (SCpnt->sense_buffer[2] == ILLEGAL_REQUEST) {
	    if (rscsi_disks[DEVICE_NR(SCpnt->request.dev)].ten) {
	      rscsi_disks[DEVICE_NR(SCpnt->request.dev)].ten = 0;
	      requeue_sd_request(SCpnt);
	      result = 0;
	    } else {
	    }
	  }
	}  /* driver byte != 0 */
	if (result) {
		printk("SCSI disk error : host %d id %d lun %d return code = %x\n",
		       rscsi_disks[DEVICE_NR(SCpnt->request.dev)].device->host->host_no,
		       rscsi_disks[DEVICE_NR(SCpnt->request.dev)].device->id,
		       rscsi_disks[DEVICE_NR(SCpnt->request.dev)].device->lun, result);

		if (driver_byte(result) & DRIVER_SENSE)
			print_sense("sd", SCpnt);
		end_scsi_request(SCpnt, 0, SCpnt->request.current_nr_sectors);
		requeue_sd_request(SCpnt);
		return;
	}
}

/*
	requeue_sd_request() is the request handler function for the sd driver.
	Its function in life is to take block device requests, and translate
	them to SCSI commands.
*/

static void do_sd_request (void)
{
  Scsi_Cmnd * SCpnt = NULL;
  struct request * req = NULL;
  int flag = 0;
  while (1==1){
    cli();
    if (CURRENT != NULL && CURRENT->dev == -1) {
      sti();
      return;
    };

    INIT_SCSI_REQUEST;


/* We have to be careful here.  allocate_device will get a free pointer, but
   there is no guarantee that it is queueable.  In normal usage, we want to
   call this, because other types of devices may have the host all tied up,
   and we want to make sure that we have at least one request pending for this
   type of device.   We can also come through here while servicing an
   interrupt, because of the need to start another command.  If we call
   allocate_device more than once, then the system can wedge if the command
   is not queueable.  The request_queueable function is safe because it checks
   to make sure that the host is able to take another command before it returns
   a pointer.  */

    if (flag++ == 0)
      SCpnt = allocate_device(&CURRENT,
			      rscsi_disks[DEVICE_NR(MINOR(CURRENT->dev))].device->index, 0); 
    else SCpnt = NULL;
    sti();

/* This is a performance enhancement.  We dig down into the request list and
   try and find a queueable request (i.e. device not busy, and host able to
   accept another command.  If we find one, then we queue it. This can
   make a big difference on systems with more than one disk drive.  We want
   to have the interrupts off when monkeying with the request list, because
   otherwise the kernel might try and slip in a request inbetween somewhere. */

    if (!SCpnt && NR_SD > 1){
      struct request *req1;
      req1 = NULL;
      cli();
      req = CURRENT;
      while(req){
	SCpnt = request_queueable(req,
				  rscsi_disks[DEVICE_NR(MINOR(req->dev))].device->index);
	if(SCpnt) break;
	req1 = req;
	req = req->next;
      };
      if (SCpnt && req->dev == -1) {
	if (req == CURRENT) 
	  CURRENT = CURRENT->next;
	else
	  req1->next = req->next;
      };
      sti();
    };
    
    if (!SCpnt) return; /* Could not find anything to do */
    
    wake_up(&wait_for_request);
    
    /* Queue command */
    requeue_sd_request(SCpnt);
  };  /* While */
}    

static void requeue_sd_request (Scsi_Cmnd * SCpnt)
{
	int dev, block, this_count;
	unsigned char cmd[10];
	char * buff;

repeat:

	if(SCpnt->request.dev <= 0) {
	  do_sd_request();
	  return;
	}

	dev =  MINOR(SCpnt->request.dev);
	block = SCpnt->request.sector;
	this_count = 0;

#ifdef DEBUG
	printk("Doing sd request, dev = %d, block = %d\n", dev, block);
#endif

	if (dev >= (NR_SD << 4) || block + SCpnt->request.nr_sectors > sd[dev].nr_sects)
		{
		end_scsi_request(SCpnt, 0, SCpnt->request.nr_sectors);
		goto repeat;
		}

	block += sd[dev].start_sect;
	dev = DEVICE_NR(dev);

	if (rscsi_disks[dev].device->changed)
	        {
/*
 * quietly refuse to do anything to a changed disc until the changed bit has been reset
 */
		/* printk("SCSI disk has been changed.  Prohibiting further I/O.\n");	*/
		end_scsi_request(SCpnt, 0, SCpnt->request.nr_sectors);
		goto repeat;
		}

#ifdef DEBUG
	printk("sd%d : real dev = /dev/sd%d, block = %d\n", MINOR(SCpnt->request.dev), dev, block);
#endif

	switch (SCpnt->request.cmd)
		{
		case WRITE :
			if (!rscsi_disks[dev].device->writeable)
				{
				end_scsi_request(SCpnt, 0, SCpnt->request.nr_sectors);
				goto repeat;
				}
			cmd[0] = WRITE_6;
			break;
		case READ :
			cmd[0] = READ_6;
			break;
		default :
			panic ("Unknown sd command %d\n", SCpnt->request.cmd);
		      }

	SCpnt->this_count = 0;

	if (!SCpnt->request.bh || 
	    (SCpnt->request.nr_sectors == SCpnt->request.current_nr_sectors)) {

	  /* case of page request (i.e. raw device), or unlinked buffer */
	  this_count = SCpnt->request.nr_sectors;
	  buff = SCpnt->request.buffer;
	  SCpnt->use_sg = 0;

	} else if (SCpnt->host->sg_tablesize == 0 ||
		   (need_isa_buffer && 
		    dma_free_sectors < 10)) {

	  /* Case of host adapter that cannot scatter-gather.  We also
	   come here if we are running low on DMA buffer memory.  We set
	   a threshold higher than that we would need for this request so
	   we leave room for other requests.  Even though we would not need
	   it all, we need to be conservative, because if we run low enough
	   we have no choice but to panic. */

	  if (SCpnt->host->sg_tablesize != 0 &&
	      need_isa_buffer && 
	      dma_free_sectors < 10)
	    printk("Warning: SCSI DMA buffer space running low.  Using non scatter-gather I/O.\n");

	  this_count = SCpnt->request.current_nr_sectors;
	  buff = SCpnt->request.buffer;
	  SCpnt->use_sg = 0;

	} else {

	  /* Scatter-gather capable host adapter */
	  struct buffer_head * bh;
	  struct scatterlist * sgpnt;
	  int count, this_count_max;
	  bh = SCpnt->request.bh;
	  this_count = 0;
	  this_count_max = (rscsi_disks[dev].ten ? 0xffff : 0xff);
	  count = 0;
	  while(bh && count < SCpnt->host->sg_tablesize) {
	    if ((this_count + (bh->b_size >> 9)) > this_count_max) break;
	    this_count += (bh->b_size >> 9);
	    count++;
	    bh = bh->b_reqnext;
	  };
	  SCpnt->use_sg = count;  /* Number of chains */
	  count = 512;/* scsi_malloc can only allocate in chunks of 512 bytes*/
	  while( count < (SCpnt->use_sg * sizeof(struct scatterlist))) 
	    count = count << 1;
	  SCpnt->sglist_len = count;
	  sgpnt = (struct scatterlist * ) scsi_malloc(count);
	  if (!sgpnt) {
	    printk("Warning - running *really* short on DMA buffers\n");
	    SCpnt->use_sg = 0;  /* No memory left - bail out */
	    this_count = SCpnt->request.current_nr_sectors;
	    buff = SCpnt->request.buffer;
	  } else {
	    buff = (char *) sgpnt;
	    count = 0;
	    bh = SCpnt->request.bh;
	    for(count = 0, bh = SCpnt->request.bh; count < SCpnt->use_sg; 
		count++, bh = bh->b_reqnext) {
	      sgpnt[count].address = bh->b_data;
	      sgpnt[count].alt_address = NULL;
	      sgpnt[count].length = bh->b_size;
	      if (((int) sgpnt[count].address) + sgpnt[count].length > 
		  ISA_DMA_THRESHOLD & (SCpnt->host->unchecked_isa_dma)) {
		sgpnt[count].alt_address = sgpnt[count].address;
		/* We try and avoid exhausting the DMA pool, since it is easier
		   to control usage here.  In other places we might have a more
		   pressing need, and we would be screwed if we ran out */
		if(dma_free_sectors < (bh->b_size >> 9) + 5) {
		  sgpnt[count].address = NULL;
		} else {
		  sgpnt[count].address = (char *) scsi_malloc(sgpnt[count].length);
		};
/* If we start running low on DMA buffers, we abort the scatter-gather
   operation, and free all of the memory we have allocated.  We want to
   ensure that all scsi operations are able to do at least a non-scatter/gather
   operation */
		if(sgpnt[count].address == NULL){ /* Out of dma memory */
		  printk("Warning: Running low on SCSI DMA buffers");
		  /* Try switching back to a non scatter-gather operation. */
		  while(--count >= 0){
		    if(sgpnt[count].alt_address) 
		      scsi_free(sgpnt[count].address, sgpnt[count].length);
		  };
		  this_count = SCpnt->request.current_nr_sectors;
		  buff = SCpnt->request.buffer;
		  SCpnt->use_sg = 0;
		  scsi_free(buff, SCpnt->sglist_len);
		  break;
		};

		if (SCpnt->request.cmd == WRITE)
		  memcpy(sgpnt[count].address, sgpnt[count].alt_address, 
			 sgpnt[count].length);
	      };
	    }; /* for loop */
	  };  /* Able to malloc sgpnt */
	};  /* Host adapter capable of scatter-gather */

/* Now handle the possibility of DMA to addresses > 16Mb */

	if(SCpnt->use_sg == 0){
	  if (((int) buff) + (this_count << 9) > ISA_DMA_THRESHOLD && 
	    (SCpnt->host->unchecked_isa_dma)) {
	    buff = (char *) scsi_malloc(this_count << 9);
	    if(buff == NULL) panic("Ran out of DMA buffers.");
	    if (SCpnt->request.cmd == WRITE)
	      memcpy(buff, (char *)SCpnt->request.buffer, this_count << 9);
	  };
	};

#ifdef DEBUG
	printk("sd%d : %s %d/%d 512 byte blocks.\n", MINOR(SCpnt->request.dev),
		(SCpnt->request.cmd == WRITE) ? "writing" : "reading",
		this_count, SCpnt->request.nr_sectors);
#endif

	cmd[1] = (SCpnt->lun << 5) & 0xe0;

	if (rscsi_disks[dev].sector_size == 1024){
	  if(block & 1) panic("sd.c:Bad block number requested");
	  if(this_count & 1) panic("sd.c:Bad block number requested");
	  block = block >> 1;
	  this_count = this_count >> 1;
	};

	if (rscsi_disks[dev].sector_size == 256){
	  block = block << 1;
	  this_count = this_count << 1;
	};

	if (((this_count > 0xff) ||  (block > 0x1fffff)) && rscsi_disks[dev].ten)
		{
		if (this_count > 0xffff)
			this_count = 0xffff;

		cmd[0] += READ_10 - READ_6 ;
		cmd[2] = (unsigned char) (block >> 24) & 0xff;
		cmd[3] = (unsigned char) (block >> 16) & 0xff;
		cmd[4] = (unsigned char) (block >> 8) & 0xff;
		cmd[5] = (unsigned char) block & 0xff;
		cmd[6] = cmd[9] = 0;
		cmd[7] = (unsigned char) (this_count >> 8) & 0xff;
		cmd[8] = (unsigned char) this_count & 0xff;
		}
	else
		{
		if (this_count > 0xff)
			this_count = 0xff;

		cmd[1] |= (unsigned char) ((block >> 16) & 0x1f);
		cmd[2] = (unsigned char) ((block >> 8) & 0xff);
		cmd[3] = (unsigned char) block & 0xff;
		cmd[4] = (unsigned char) this_count;
		cmd[5] = 0;
		}

/*
 * We shouldn't disconnect in the middle of a sector, so with a dumb 
 * host adapter, it's safe to assume that we can at least transfer 
 * this many bytes between each connect / disconnect.  
 */

        SCpnt->transfersize = rscsi_disks[dev].sector_size;
        SCpnt->underflow = this_count << 9; 

	scsi_do_cmd (SCpnt, (void *) cmd, buff, 
		     this_count * rscsi_disks[dev].sector_size,
		     rw_intr, SD_TIMEOUT, MAX_RETRIES);
}

int check_scsidisk_media_change(int full_dev, int flag){
        int retval;
	int target;
	struct inode inode;

	target =  DEVICE_NR(MINOR(full_dev));

	if (target >= NR_SD) {
		printk("SCSI disk request error: invalid device.\n");
		return 0;
	};

	if(!rscsi_disks[target].device->removable) return 0;

	inode.i_rdev = full_dev;  /* This is all we really need here */
	retval = sd_ioctl(&inode, NULL, SCSI_IOCTL_TEST_UNIT_READY, 0);

	if(retval){ /* Unable to test, unit probably not ready.  This usually
		     means there is no disc in the drive.  Mark as changed,
		     and we will figure it out later once the drive is
		     available again.  */

	  rscsi_disks[target].device->changed = 1;
	  return 1; /* This will force a flush, if called from
		       check_disk_change */
	};

	retval = rscsi_disks[target].device->changed;
	if(!flag) rscsi_disks[target].device->changed = 0;
	return retval;
}

static void sd_init_done (Scsi_Cmnd * SCpnt)
{
  struct request * req;
  struct task_struct * p;
  
  req = &SCpnt->request;
  req->dev = 0xfffe; /* Busy, but indicate request done */
  
  if ((p = req->waiting) != NULL) {
    req->waiting = NULL;
    p->state = TASK_RUNNING;
    if (p->counter > current->counter)
      need_resched = 1;
  }
}

static int sd_init_onedisk(int i)
{
  int j = 0;
  unsigned char cmd[10];
  unsigned char *buffer;
  char spintime;
  int the_result, retries;
  Scsi_Cmnd * SCpnt;

  /* We need to retry the READ_CAPACITY because a UNIT_ATTENTION is considered
     a fatal error, and many devices report such an error just after a scsi
     bus reset. */

  SCpnt = allocate_device(NULL, rscsi_disks[i].device->index, 1);
  buffer = (unsigned char *) scsi_malloc(512);

  spintime = 0;

  /* Spin up drives, as required.  Only do this at boot time */
  if (current == task[0]){
    do{
      cmd[0] = TEST_UNIT_READY;
      cmd[1] = (rscsi_disks[i].device->lun << 5) & 0xe0;
      memset ((void *) &cmd[2], 0, 8);
      SCpnt->request.dev = 0xffff;  /* Mark as really busy again */
      SCpnt->sense_buffer[0] = 0;
      SCpnt->sense_buffer[2] = 0;
      
      scsi_do_cmd (SCpnt,
		   (void *) cmd, (void *) buffer,
		   512, sd_init_done,  SD_TIMEOUT,
		   MAX_RETRIES);
      
      while(SCpnt->request.dev != 0xfffe);
      
      the_result = SCpnt->result;
      
      /* Look for non-removable devices that return NOT_READY.  Issue command
	 to spin up drive for these cases. */
      if(the_result && !rscsi_disks[i].device->removable && 
	 SCpnt->sense_buffer[2] == NOT_READY) {
	int time1;
	if(!spintime){
	  printk( "sd%d: Spinning up disk...", i );
	  cmd[0] = START_STOP;
	  cmd[1] = (rscsi_disks[i].device->lun << 5) & 0xe0;
	  cmd[1] |= 1;  /* Return immediately */
	  memset ((void *) &cmd[2], 0, 8);
	  cmd[4] = 1; /* Start spin cycle */
	  SCpnt->request.dev = 0xffff;  /* Mark as really busy again */
	  SCpnt->sense_buffer[0] = 0;
	  SCpnt->sense_buffer[2] = 0;
	  
	  scsi_do_cmd (SCpnt,
		       (void *) cmd, (void *) buffer,
		       512, sd_init_done,  SD_TIMEOUT,
		       MAX_RETRIES);
	  
	  while(SCpnt->request.dev != 0xfffe);

	  spintime = jiffies;
	};

	time1 = jiffies;
	while(jiffies < time1 + 100); /* Wait 1 second for next try */
	printk( "." );
      };
    } while(the_result && spintime && spintime+5000 > jiffies);
    if (spintime) {
       if (the_result)
           printk( "not responding...\n" );
       else
           printk( "ready\n" );
    }
  };  /* current == task[0] */


  retries = 3;
  do {
    cmd[0] = READ_CAPACITY;
    cmd[1] = (rscsi_disks[i].device->lun << 5) & 0xe0;
    memset ((void *) &cmd[2], 0, 8);
    memset ((void *) buffer, 0, 8);
    SCpnt->request.dev = 0xffff;  /* Mark as really busy again */
    SCpnt->sense_buffer[0] = 0;
    SCpnt->sense_buffer[2] = 0;
    
    scsi_do_cmd (SCpnt,
		 (void *) cmd, (void *) buffer,
		 8, sd_init_done,  SD_TIMEOUT,
		 MAX_RETRIES);
    
    if (current == task[0])
      while(SCpnt->request.dev != 0xfffe);
    else
      if (SCpnt->request.dev != 0xfffe){
	SCpnt->request.waiting = current;
	current->state = TASK_UNINTERRUPTIBLE;
	while (SCpnt->request.dev != 0xfffe) schedule();
      };
    
    the_result = SCpnt->result;
    retries--;

  } while(the_result && retries);

  SCpnt->request.dev = -1;  /* Mark as not busy */

  wake_up(&scsi_devices[SCpnt->index].device_wait); 

  /* Wake up a process waiting for device*/

  /*
   *	The SCSI standard says "READ CAPACITY is necessary for self confuring software"
   *	While not mandatory, support of READ CAPACITY is strongly encouraged.
   *	We used to die if we couldn't successfully do a READ CAPACITY.
   *	But, now we go on about our way.  The side effects of this are
   *
   *	1.  We can't know block size with certainty.  I have said "512 bytes is it"
   *	   	as this is most common.
   *
   *	2.  Recovery from when some one attempts to read past the end of the raw device will
   *	    be slower.
   */

  if (the_result)
    {
      printk ("sd%d : READ CAPACITY failed.\n"
	      "sd%d : status = %x, message = %02x, host = %d, driver = %02x \n",
	      i,i,
	      status_byte(the_result),
	      msg_byte(the_result),
	      host_byte(the_result),
	      driver_byte(the_result)
	      );
      if (driver_byte(the_result)  & DRIVER_SENSE)
	printk("sd%d : extended sense code = %1x \n", i, SCpnt->sense_buffer[2] & 0xf);
      else
	printk("sd%d : sense not available. \n", i);

      printk("sd%d : block size assumed to be 512 bytes, disk size 1GB.  \n", i);
      rscsi_disks[i].capacity = 0x1fffff;
      rscsi_disks[i].sector_size = 512;

      /* Set dirty bit for removable devices if not ready - sometimes drives
	 will not report this properly. */
      if(rscsi_disks[i].device->removable && 
	 SCpnt->sense_buffer[2] == NOT_READY)
	rscsi_disks[i].device->changed = 1;

    }
  else
    {
      rscsi_disks[i].capacity = (buffer[0] << 24) |
	(buffer[1] << 16) |
	  (buffer[2] << 8) |
	    buffer[3];

      rscsi_disks[i].sector_size = (buffer[4] << 24) |
	(buffer[5] << 16) | (buffer[6] << 8) | buffer[7];

      if (rscsi_disks[i].sector_size != 512 &&
	  rscsi_disks[i].sector_size != 1024 &&
	  rscsi_disks[i].sector_size != 256)
	{
	  printk ("sd%d : unsupported sector size %d.\n",
		  i, rscsi_disks[i].sector_size);
	  if(rscsi_disks[i].device->removable){
	    rscsi_disks[i].capacity = 0;
	  } else {
	    printk ("scsi : deleting disk entry.\n");
	    for  (j=i;  j < NR_SD - 1;)
	      rscsi_disks[j] = rscsi_disks[++j];
	    --i;
	    --NR_SD;
	    scsi_free(buffer, 512);
	    return i;
	  };
	}
      if(rscsi_disks[i].sector_size == 1024)
	rscsi_disks[i].capacity <<= 1;  /* Change this into 512 byte sectors */
      if(rscsi_disks[i].sector_size == 256)
	rscsi_disks[i].capacity >>= 1;  /* Change this into 512 byte sectors */
    }

  rscsi_disks[i].ten = 1;
  rscsi_disks[i].remap = 1;
  scsi_free(buffer, 512);
  return i;
}

/*
	The sd_init() function looks at all SCSI drives present, determines
	their size, and reads partition	table entries for them.
*/

unsigned long sd_init(unsigned long memory_start, unsigned long memory_end)
{
	int i;

	if (register_blkdev(MAJOR_NR,"sd",&sd_fops)) {
		printk("Unable to get major %d for SCSI disk\n",MAJOR_NR);
		return memory_start;
	}
	if (MAX_SD == 0) return memory_start;

	sd_sizes = (int *) memory_start;
	memory_start += (MAX_SD << 4) * sizeof(int);
	memset(sd_sizes, 0, (MAX_SD << 4) * sizeof(int));

	sd_blocksizes = (int *) memory_start;
	memory_start += (MAX_SD << 4) * sizeof(int);
	for(i=0;i<(MAX_SD << 4);i++) sd_blocksizes[i] = 1024;
	blksize_size[MAJOR_NR] = sd_blocksizes;

	sd = (struct hd_struct *) memory_start;
	memory_start += (MAX_SD << 4) * sizeof(struct hd_struct);

	sd_gendisk.max_nr = MAX_SD;
	sd_gendisk.part = sd;
	sd_gendisk.sizes = sd_sizes;
	sd_gendisk.real_devices = (void *) rscsi_disks;

	for (i = 0; i < NR_SD; ++i)
	  i = sd_init_onedisk(i);

	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;

	/* If our host adapter is capable of scatter-gather, then we increase
	   the read-ahead to 16 blocks (32 sectors).  If not, we use
	   a two block (4 sector) read ahead. */
	if(rscsi_disks[0].device->host->sg_tablesize)
	  read_ahead[MAJOR_NR] = 32;
	/* 64 sector read-ahead */
	else
	  read_ahead[MAJOR_NR] = 4;  /* 4 sector read-ahead */
	
	sd_gendisk.next = gendisk_head;
	gendisk_head = &sd_gendisk;
	return memory_start;
}

unsigned long sd_init1(unsigned long mem_start, unsigned long mem_end){
  rscsi_disks = (Scsi_Disk *) mem_start;
  mem_start += MAX_SD * sizeof(Scsi_Disk);
  return mem_start;
};

void sd_attach(Scsi_Device * SDp){
  rscsi_disks[NR_SD++].device = SDp;
  if(NR_SD > MAX_SD) panic ("scsi_devices corrupt (sd)");
};

#define DEVICE_BUSY rscsi_disks[target].device->busy
#define USAGE rscsi_disks[target].device->access_count
#define CAPACITY rscsi_disks[target].capacity
#define MAYBE_REINIT  sd_init_onedisk(target)
#define GENDISK_STRUCT sd_gendisk

/* This routine is called to flush all partitions and partition tables
   for a changed scsi disk, and then re-read the new partition table.
   If we are revalidating a disk because of a media change, then we
   enter with usage == 0.  If we are using an ioctl, we automatically have
   usage == 1 (we need an open channel to use an ioctl :-), so this
   is our limit.
 */
int revalidate_scsidisk(int dev, int maxusage){
	  int target, major;
	  struct gendisk * gdev;
	  int max_p;
	  int start;
	  int i;

	  target =  DEVICE_NR(MINOR(dev));
	  gdev = &GENDISK_STRUCT;

	  cli();
	  if (DEVICE_BUSY || USAGE > maxusage) {
	    sti();
	    printk("Device busy for revalidation (usage=%d)\n", USAGE);
	    return -EBUSY;
	  };
	  DEVICE_BUSY = 1;
	  sti();

	  max_p = gdev->max_p;
	  start = target << gdev->minor_shift;
	  major = MAJOR_NR << 8;

	  for (i=max_p - 1; i >=0 ; i--) {
	    sync_dev(major | start | i);
	    invalidate_inodes(major | start | i);
	    invalidate_buffers(major | start | i);
	    gdev->part[start+i].start_sect = 0;
	    gdev->part[start+i].nr_sects = 0;
	  };

#ifdef MAYBE_REINIT
	  MAYBE_REINIT;
#endif

	  gdev->part[start].nr_sects = CAPACITY;
	  resetup_one_dev(gdev, target);

	  DEVICE_BUSY = 0;
	  return 0;
}
