/*
 *      sr.c by David Giller
 *
 *      adapted from:
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

#define MAJOR_NR SCSI_CDROM_MAJOR
#include "../block/blk.h"
#include "scsi.h"
#include "hosts.h"
#include "sr.h"
#include "scsi_ioctl.h"   /* For the door lock/unlock commands */
#include "constants.h"

#define MAX_RETRIES 1
#define SR_TIMEOUT 500

int NR_SR=0;
int MAX_SR=0;
Scsi_CD * scsi_CDs;
static int * sr_sizes;

static int * sr_blocksizes;

static int sr_open(struct inode *, struct file *);
static void get_sectorsize(int);

extern int sr_ioctl(struct inode *, struct file *, unsigned int, unsigned long);

void requeue_sr_request (Scsi_Cmnd * SCpnt);

static void sr_release(struct inode * inode, struct file * file)
{
	sync_dev(inode->i_rdev);
	if(! --scsi_CDs[MINOR(inode->i_rdev)].device->access_count)
	  sr_ioctl(inode, NULL, SCSI_IOCTL_DOORUNLOCK, 0);
}

static struct file_operations sr_fops = 
{
	NULL,			/* lseek - default */
	block_read,		/* read - general block-dev read */
	block_write,		/* write - general block-dev write */
	NULL,			/* readdir - bad */
	NULL,			/* select */
	sr_ioctl,		/* ioctl */
	NULL,			/* mmap */
	sr_open,       		/* no special open code */
	sr_release,		/* release */
	NULL			/* fsync */
};

/*
 * This function checks to see if the media has been changed in the
 * CDROM drive.  It is possible that we have already sensed a change,
 * or the drive may have sensed one and not yet reported it.  We must
 * be ready for either case. This function always reports the current
 * value of the changed bit.  If flag is 0, then the changed bit is reset.
 * This function could be done as an ioctl, but we would need to have
 * an inode for that to work, and we do not always have one.
 */

int check_cdrom_media_change(int full_dev, int flag){
	int retval, target;
	struct inode inode;

	target =  MINOR(full_dev);

	if (target >= NR_SR) {
		printk("CD-ROM request error: invalid device.\n");
		return 0;
	};

	inode.i_rdev = full_dev;  /* This is all we really need here */
	retval = sr_ioctl(&inode, NULL, SCSI_IOCTL_TEST_UNIT_READY, 0);

	if(retval){ /* Unable to test, unit probably not ready.  This usually
		     means there is no disc in the drive.  Mark as changed,
		     and we will figure it out later once the drive is
		     available again.  */

	  scsi_CDs[target].device->changed = 1;
	  return 1; /* This will force a flush, if called from
		       check_disk_change */
	};

	retval = scsi_CDs[target].device->changed;
	if(!flag) {
	  scsi_CDs[target].device->changed = 0;
	  /* If the disk changed, the capacity will now be different,
	     so we force a re-read of this information */
	  if (retval) scsi_CDs[target].needs_sector_size = 1;
	};
	return retval;
}

/*
 * rw_intr is the interrupt routine for the device driver.  It will be notified on the 
 * end of a SCSI read / write, and will take on of several actions based on success or failure.
 */

static void rw_intr (Scsi_Cmnd * SCpnt)
{
	int result = SCpnt->result;
	int this_count = SCpnt->this_count;

#ifdef DEBUG
	printk("sr.c done: %x %x\n",result, SCpnt->request.bh->b_data);
#endif
	if (!result)
		{ /* No error */
		  if (SCpnt->use_sg == 0) {
		    if (SCpnt->buffer != SCpnt->request.buffer)
		      {
			int offset;
			offset = (SCpnt->request.sector % 4) << 9;
			memcpy((char *)SCpnt->request.buffer, 
			       (char *)SCpnt->buffer + offset, 
			       this_count << 9);
			/* Even though we are not using scatter-gather, we look
			   ahead and see if there is a linked request for the
			   other half of this buffer.  If there is, then satisfy
			   it. */
			if((offset == 0) && this_count == 2 &&
			   SCpnt->request.nr_sectors > this_count && 
			   SCpnt->request.bh &&
			   SCpnt->request.bh->b_reqnext &&
			   SCpnt->request.bh->b_reqnext->b_size == 1024) {
			  memcpy((char *)SCpnt->request.bh->b_reqnext->b_data, 
				 (char *)SCpnt->buffer + 1024, 
				 1024);
			  this_count += 2;
			};
			
			scsi_free(SCpnt->buffer, 2048);
		      }
		  } else {
		    struct scatterlist * sgpnt;
		    int i;
		    sgpnt = (struct scatterlist *) SCpnt->buffer;
		    for(i=0; i<SCpnt->use_sg; i++) {
		      if (sgpnt[i].alt_address) {
			if (sgpnt[i].alt_address != sgpnt[i].address) {
			  memcpy(sgpnt[i].alt_address, sgpnt[i].address, sgpnt[i].length);
			};
			scsi_free(sgpnt[i].address, sgpnt[i].length);
		      };
		    };
		    scsi_free(SCpnt->buffer, SCpnt->sglist_len);  /* Free list of scatter-gather pointers */
		    if(SCpnt->request.sector % 4) this_count -= 2;
/* See   if there is a padding record at the end that needs to be removed */
		    if(this_count > SCpnt->request.nr_sectors)
		      this_count -= 2;
		  };

#ifdef DEBUG
		printk("(%x %x %x) ",SCpnt->request.bh, SCpnt->request.nr_sectors, 
		       this_count);
#endif
		if (SCpnt->request.nr_sectors > this_count)
			{	 
			SCpnt->request.errors = 0;
			if (!SCpnt->request.bh)
			    panic("sr.c: linked page request (%lx %x)",
				  SCpnt->request.sector, this_count);
			}

		  end_scsi_request(SCpnt, 1, this_count);  /* All done */
		  requeue_sr_request(SCpnt);
		  return;
		} /* Normal completion */

	/* We only come through here if we have an error of some kind */

/* Free up any indirection buffers we allocated for DMA purposes. */
	if (SCpnt->use_sg) {
	  struct scatterlist * sgpnt;
	  int i;
	  sgpnt = (struct scatterlist *) SCpnt->buffer;
	  for(i=0; i<SCpnt->use_sg; i++) {
	    if (sgpnt[i].alt_address) {
	      scsi_free(sgpnt[i].address, sgpnt[i].length);
	    };
	  };
	  scsi_free(SCpnt->buffer, SCpnt->sglist_len);  /* Free list of scatter-gather pointers */
	} else {
	  if (SCpnt->buffer != SCpnt->request.buffer)
	    scsi_free(SCpnt->buffer, SCpnt->bufflen);
	};

	if (driver_byte(result) != 0) {
		if ((SCpnt->sense_buffer[0] & 0x7f) == 0x70) {
			if ((SCpnt->sense_buffer[2] & 0xf) == UNIT_ATTENTION) {
				/* detected disc change.  set a bit and quietly refuse	*/
				/* further access.					*/
		    
				scsi_CDs[DEVICE_NR(SCpnt->request.dev)].device->changed = 1;
				end_scsi_request(SCpnt, 0, this_count);
			        requeue_sr_request(SCpnt);
				return;
			}
		}
	    
		if (SCpnt->sense_buffer[2] == ILLEGAL_REQUEST) {
			printk("CD-ROM error: Drive reports ILLEGAL REQUEST.\n");
			if (scsi_CDs[DEVICE_NR(SCpnt->request.dev)].ten) {
				scsi_CDs[DEVICE_NR(SCpnt->request.dev)].ten = 0;
				requeue_sr_request(SCpnt);
				result = 0;
				return;
			} else {
			  printk("CD-ROM error: Drive reports %d.\n", SCpnt->sense_buffer[2]);				
			  end_scsi_request(SCpnt, 0, this_count);
			  requeue_sr_request(SCpnt); /* Do next request */
			  return;
			}

		}

		if (SCpnt->sense_buffer[2] == NOT_READY) {
			printk("CDROM not ready.  Make sure you have a disc in the drive.\n");
			end_scsi_request(SCpnt, 0, this_count);
			requeue_sr_request(SCpnt); /* Do next request */
			return;
		};
	      }
	
	/* We only get this far if we have an error we have not recognized */
	if(result) {
	  printk("SCSI CD error : host %d id %d lun %d return code = %03x\n", 
		 scsi_CDs[DEVICE_NR(SCpnt->request.dev)].device->host->host_no, 
		 scsi_CDs[DEVICE_NR(SCpnt->request.dev)].device->id,
		 scsi_CDs[DEVICE_NR(SCpnt->request.dev)].device->lun,
		 result);
	    
	  if (status_byte(result) == CHECK_CONDITION)
		  print_sense("sr", SCpnt);
	  
	  end_scsi_request(SCpnt, 0, SCpnt->request.current_nr_sectors);
	  requeue_sr_request(SCpnt);
  }
}

static int sr_open(struct inode * inode, struct file * filp)
{
	if(MINOR(inode->i_rdev) >= NR_SR || 
	   !scsi_CDs[MINOR(inode->i_rdev)].device) return -ENODEV;   /* No such device */

        check_disk_change(inode->i_rdev);

	if(!scsi_CDs[MINOR(inode->i_rdev)].device->access_count++)
	  sr_ioctl(inode, NULL, SCSI_IOCTL_DOORLOCK, 0);

	/* If this device did not have media in the drive at boot time, then
	   we would have been unable to get the sector size.  Check to see if
	   this is the case, and try again.
	   */

	if(scsi_CDs[MINOR(inode->i_rdev)].needs_sector_size)
	  get_sectorsize(MINOR(inode->i_rdev));

	return 0;
}


/*
 * do_sr_request() is the request handler function for the sr driver.  Its function in life 
 * is to take block device requests, and translate them to SCSI commands.
 */
	
static void do_sr_request (void)
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

    if (flag++ == 0)
      SCpnt = allocate_device(&CURRENT,
			      scsi_CDs[DEVICE_NR(MINOR(CURRENT->dev))].device->index, 0); 
    else SCpnt = NULL;
    sti();

/* This is a performance enhancement.  We dig down into the request list and
   try and find a queueable request (i.e. device not busy, and host able to
   accept another command.  If we find one, then we queue it. This can
   make a big difference on systems with more than one disk drive.  We want
   to have the interrupts off when monkeying with the request list, because
   otherwise the kernel might try and slip in a request inbetween somewhere. */

    if (!SCpnt && NR_SR > 1){
      struct request *req1;
      req1 = NULL;
      cli();
      req = CURRENT;
      while(req){
	SCpnt = request_queueable(req,
				  scsi_CDs[DEVICE_NR(MINOR(req->dev))].device->index);
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
    
    if (!SCpnt)
      return; /* Could not find anything to do */
    
  wake_up(&wait_for_request);

/* Queue command */
  requeue_sr_request(SCpnt);
  };  /* While */
}    

void requeue_sr_request (Scsi_Cmnd * SCpnt)
{
	unsigned int dev, block, realcount;
	unsigned char cmd[10], *buffer, tries;
	int this_count, start, end_rec;

	tries = 2;

      repeat:
	if(SCpnt->request.dev <= 0) {
	  do_sr_request();
	  return;
	}

	dev =  MINOR(SCpnt->request.dev);
	block = SCpnt->request.sector;	
	buffer = NULL;
	this_count = 0;

	if (dev >= NR_SR)
		{
		/* printk("CD-ROM request error: invalid device.\n");			*/
		end_scsi_request(SCpnt, 0, SCpnt->request.nr_sectors);
		tries = 2;
		goto repeat;
		}

	if (!scsi_CDs[dev].use)
		{
		/* printk("CD-ROM request error: device marked not in use.\n");		*/
		end_scsi_request(SCpnt, 0, SCpnt->request.nr_sectors);
		tries = 2;
		goto repeat;
		}

	if (scsi_CDs[dev].device->changed)
	        {
/* 
 * quietly refuse to do anything to a changed disc until the changed bit has been reset
 */
		/* printk("CD-ROM has been changed.  Prohibiting further I/O.\n");	*/
		end_scsi_request(SCpnt, 0, SCpnt->request.nr_sectors);
		tries = 2;
		goto repeat;
		}
	
	switch (SCpnt->request.cmd)
		{
		case WRITE: 		
			end_scsi_request(SCpnt, 0, SCpnt->request.nr_sectors);
			goto repeat;
			break;
		case READ : 
		        cmd[0] = READ_6;
			break;
		default : 
			panic ("Unknown sr command %d\n", SCpnt->request.cmd);
		}
	
	cmd[1] = (SCpnt->lun << 5) & 0xe0;

/*
           Now do the grungy work of figuring out which sectors we need, and
	   where in memory we are going to put them.

	   The variables we need are:

	   this_count= number of 512 byte sectors being read 
	   block     = starting cdrom sector to read.
	   realcount = # of cdrom sectors to read

	   The major difference between a scsi disk and a scsi cdrom
is that we will always use scatter-gather if we can, because we can
work around the fact that the buffer cache has a block size of 1024,
and we have 2048 byte sectors.  This code should work for buffers that
are any multiple of 512 bytes long.  */

	SCpnt->use_sg = 0;

	if (SCpnt->host->sg_tablesize > 0 &&
	    (!need_isa_buffer ||
	    dma_free_sectors >= 10)) {
	  struct buffer_head * bh;
	  struct scatterlist * sgpnt;
	  int count, this_count_max;
	  bh = SCpnt->request.bh;
	  this_count = 0;
	  count = 0;
	  this_count_max = (scsi_CDs[dev].ten ? 0xffff : 0xff) << 4;
	  /* Calculate how many links we can use.  First see if we need
	   a padding record at the start */
	  this_count = SCpnt->request.sector % 4;
	  if(this_count) count++;
	  while(bh && count < SCpnt->host->sg_tablesize) {
	    if ((this_count + (bh->b_size >> 9)) > this_count_max) break;
	    this_count += (bh->b_size >> 9);
	    count++;
	    bh = bh->b_reqnext;
	  };
	  /* Fix up in case of an odd record at the end */
	  end_rec = 0;
	  if(this_count % 4) {
	    if (count < SCpnt->host->sg_tablesize) {
	      count++;
	      end_rec = (4 - (this_count % 4)) << 9;
	      this_count += 4 - (this_count % 4);
	    } else {
	      count--;
	      this_count -= (this_count % 4);
	    };
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
	  } else {
	    buffer = (unsigned char *) sgpnt;
	    count = 0;
	    bh = SCpnt->request.bh;
	    if(SCpnt->request.sector % 4) {
	      sgpnt[count].length = (SCpnt->request.sector % 4) << 9;
	      sgpnt[count].address = (char *) scsi_malloc(sgpnt[count].length);
	      if(!sgpnt[count].address) panic("SCSI DMA pool exhausted.");
	      sgpnt[count].alt_address = sgpnt[count].address; /* Flag to delete
								  if needed */
	      count++;
	    };
	    for(bh = SCpnt->request.bh; count < SCpnt->use_sg; 
		count++, bh = bh->b_reqnext) {
	      if (bh) { /* Need a placeholder at the end of the record? */
		sgpnt[count].address = bh->b_data;
		sgpnt[count].length = bh->b_size;
		sgpnt[count].alt_address = NULL;
	      } else {
		sgpnt[count].address = (char *) scsi_malloc(end_rec);
		if(!sgpnt[count].address) panic("SCSI DMA pool exhausted.");
		sgpnt[count].length = end_rec;
		sgpnt[count].alt_address = sgpnt[count].address;
		if (count+1 != SCpnt->use_sg) panic("Bad sr request list");
		break;
	      };
	      if (((int) sgpnt[count].address) + sgpnt[count].length > 
		  ISA_DMA_THRESHOLD & (SCpnt->host->unchecked_isa_dma)) {
		sgpnt[count].alt_address = sgpnt[count].address;
		/* We try and avoid exhausting the DMA pool, since it is easier
		   to control usage here.  In other places we might have a more
		   pressing need, and we would be screwed if we ran out */
		if(dma_free_sectors < (sgpnt[count].length >> 9) + 5) {
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
		  SCpnt->use_sg = 0;
		  scsi_free(buffer, SCpnt->sglist_len);
		  break;
		}; /* if address == NULL */
	      };  /* if need DMA fixup */
	    };  /* for loop to fill list */
#ifdef DEBUG
	    printk("SG: %d %d %d %d %d *** ",SCpnt->use_sg, SCpnt->request.sector,
		   this_count, 
		   SCpnt->request.current_nr_sectors,
		   SCpnt->request.nr_sectors);
	    for(count=0; count<SCpnt->use_sg; count++)
	      printk("SGlist: %d %x %x %x\n", count,
		     sgpnt[count].address, 
		     sgpnt[count].alt_address, 
		     sgpnt[count].length);
#endif
	  };  /* Able to allocate scatter-gather list */
	};
	
	if (SCpnt->use_sg == 0){
	  /* We cannot use scatter-gather.  Do this the old fashion way */
	  if (!SCpnt->request.bh)  	
	    this_count = SCpnt->request.nr_sectors;
	  else
	    this_count = (SCpnt->request.bh->b_size >> 9);
	  
	  start = block % 4;
	  if (start)
	    {				  
	      this_count = ((this_count > 4 - start) ? 
			    (4 - start) : (this_count));
	      buffer = (unsigned char *) scsi_malloc(2048);
	    } 
	  else if (this_count < 4)
	    {
	      buffer = (unsigned char *) scsi_malloc(2048);
	    }
	  else
	    {
	      this_count -= this_count % 4;
	      buffer = (unsigned char *) SCpnt->request.buffer;
	      if (((int) buffer) + (this_count << 9) > ISA_DMA_THRESHOLD & 
		  (SCpnt->host->unchecked_isa_dma))
		buffer = (unsigned char *) scsi_malloc(this_count << 9);
	    }
	};

	if (scsi_CDs[dev].sector_size == 2048)
	  block = block >> 2; /* These are the sectors that the cdrom uses */
	else
	  block = block & 0xfffffffc;

	realcount = (this_count + 3) / 4;

	if (scsi_CDs[dev].sector_size == 512) realcount = realcount << 2;

	if (((realcount > 0xff) || (block > 0x1fffff)) && scsi_CDs[dev].ten) 
		{
		if (realcount > 0xffff)
		        {
			realcount = 0xffff;
			this_count = realcount * (scsi_CDs[dev].sector_size >> 9);
			}

		cmd[0] += READ_10 - READ_6 ;
		cmd[2] = (unsigned char) (block >> 24) & 0xff;
		cmd[3] = (unsigned char) (block >> 16) & 0xff;
		cmd[4] = (unsigned char) (block >> 8) & 0xff;
		cmd[5] = (unsigned char) block & 0xff;
		cmd[6] = cmd[9] = 0;
		cmd[7] = (unsigned char) (realcount >> 8) & 0xff;
		cmd[8] = (unsigned char) realcount & 0xff;
		}
	else
		{
		if (realcount > 0xff)
		        {
			realcount = 0xff;
			this_count = realcount * (scsi_CDs[dev].sector_size >> 9);
		        }
	
		cmd[1] |= (unsigned char) ((block >> 16) & 0x1f);
		cmd[2] = (unsigned char) ((block >> 8) & 0xff);
		cmd[3] = (unsigned char) block & 0xff;
		cmd[4] = (unsigned char) realcount;
		cmd[5] = 0;
		}   

#ifdef DEBUG
{ 
	int i;
	printk("ReadCD: %d %d %d %d\n",block, realcount, buffer, this_count);
	printk("Use sg: %d\n", SCpnt->use_sg);
	printk("Dumping command: ");
	for(i=0; i<12; i++) printk("%2.2x ", cmd[i]);
	printk("\n");
};
#endif

	SCpnt->this_count = this_count;
	scsi_do_cmd (SCpnt, (void *) cmd, buffer, 
		     realcount * scsi_CDs[dev].sector_size, 
		     rw_intr, SR_TIMEOUT, MAX_RETRIES);
}

unsigned long sr_init1(unsigned long mem_start, unsigned long mem_end){
  scsi_CDs = (Scsi_CD *) mem_start;
  mem_start += MAX_SR * sizeof(Scsi_CD);
  return mem_start;
};

void sr_attach(Scsi_Device * SDp){
  scsi_CDs[NR_SR++].device = SDp;
  if(NR_SR > MAX_SR) panic ("scsi_devices corrupt (sr)");
};

static void sr_init_done (Scsi_Cmnd * SCpnt)
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

static void get_sectorsize(int i){
  unsigned char cmd[10];
  unsigned char buffer[513];
  int the_result, retries;
  Scsi_Cmnd * SCpnt;
  
  SCpnt = allocate_device(NULL, scsi_CDs[i].device->index, 1);

  retries = 3;
  do {
    cmd[0] = READ_CAPACITY;
    cmd[1] = (scsi_CDs[i].device->lun << 5) & 0xe0;
    memset ((void *) &cmd[2], 0, 8);
    SCpnt->request.dev = 0xffff;  /* Mark as really busy */
    
    memset(buffer, 0, 8);

    scsi_do_cmd (SCpnt,
		 (void *) cmd, (void *) buffer,
		 512, sr_init_done,  SR_TIMEOUT,
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

  if (the_result) {
    scsi_CDs[i].capacity = 0x1fffff;
    scsi_CDs[i].sector_size = 2048;  /* A guess, just in case */
    scsi_CDs[i].needs_sector_size = 1;
  } else {
    scsi_CDs[i].capacity = (buffer[0] << 24) |
      (buffer[1] << 16) | (buffer[2] << 8) | buffer[3];
    scsi_CDs[i].sector_size = (buffer[4] << 24) |
      (buffer[5] << 16) | (buffer[6] << 8) | buffer[7];
    if(scsi_CDs[i].sector_size == 0) scsi_CDs[i].sector_size = 2048;
    if(scsi_CDs[i].sector_size != 2048 && 
       scsi_CDs[i].sector_size != 512) {
      printk ("scd%d : unsupported sector size %d.\n",
	      i, scsi_CDs[i].sector_size);
      scsi_CDs[i].capacity = 0;
      scsi_CDs[i].needs_sector_size = 1;
    };
    if(scsi_CDs[i].sector_size == 2048)
      scsi_CDs[i].capacity *= 4;
    scsi_CDs[i].needs_sector_size = 0;
  };
}

unsigned long sr_init(unsigned long memory_start, unsigned long memory_end)
{
	int i;

	if (register_blkdev(MAJOR_NR,"sr",&sr_fops)) {
		printk("Unable to get major %d for SCSI-CD\n",MAJOR_NR);
		return memory_start;
	}
	if(MAX_SR == 0) return memory_start;

	sr_sizes = (int *) memory_start;
	memory_start += MAX_SR * sizeof(int);
	memset(sr_sizes, 0, MAX_SR * sizeof(int));

	sr_blocksizes = (int *) memory_start;
	memory_start += MAX_SR * sizeof(int);
	for(i=0;i<MAX_SR;i++) sr_blocksizes[i] = 2048;
	blksize_size[MAJOR_NR] = sr_blocksizes;

	for (i = 0; i < NR_SR; ++i)
		{
		  get_sectorsize(i);
		  printk("Scd sectorsize = %d bytes\n", scsi_CDs[i].sector_size);
		  scsi_CDs[i].use = 1;
		  scsi_CDs[i].ten = 1;
		  scsi_CDs[i].remap = 1;
		  sr_sizes[i] = scsi_CDs[i].capacity;
		}

	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;
	blk_size[MAJOR_NR] = sr_sizes;	

	/* If our host adapter is capable of scatter-gather, then we increase
	   the read-ahead to 16 blocks (32 sectors).  If not, we use
	   a two block (4 sector) read ahead. */
	if(scsi_CDs[0].device->host->sg_tablesize)
	  read_ahead[MAJOR_NR] = 32;  /* 32 sector read-ahead.  Always removable. */
	else
	  read_ahead[MAJOR_NR] = 4;  /* 4 sector read-ahead */

	return memory_start;
}	
