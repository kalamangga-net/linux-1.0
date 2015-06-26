#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <asm/segment.h>
#include <linux/errno.h>

#include "../block/blk.h"
#include "scsi.h"
#include "hosts.h"
#include "sr.h"
#include "scsi_ioctl.h"

#include <linux/cdrom.h>

#define IOCTL_RETRIES 3
/* The CDROM is fairly slow, so we need a little extra time */
#define IOCTL_TIMEOUT 200

extern int scsi_ioctl (Scsi_Device *dev, int cmd, void *arg);

static void sr_ioctl_done(Scsi_Cmnd * SCpnt)
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

/* We do our own retries because we want to know what the specific
   error code is.  Normally the UNIT_ATTENTION code will automatically
   clear after one error */

static int do_ioctl(int target, unsigned char * sr_cmd, void * buffer, unsigned buflength)
{
	Scsi_Cmnd * SCpnt;
	int result;

	SCpnt = allocate_device(NULL, scsi_CDs[target].device->index, 1);
	scsi_do_cmd(SCpnt,
		    (void *) sr_cmd, buffer, buflength, sr_ioctl_done, 
		    IOCTL_TIMEOUT, IOCTL_RETRIES);


	if (SCpnt->request.dev != 0xfffe){
	  SCpnt->request.waiting = current;
	  current->state = TASK_UNINTERRUPTIBLE;
	  while (SCpnt->request.dev != 0xfffe) schedule();
	};

	result = SCpnt->result;

/* Minimal error checking.  Ignore cases we know about, and report the rest. */
	if(driver_byte(result) != 0)
	  switch(SCpnt->sense_buffer[2] & 0xf) {
	  case UNIT_ATTENTION:
	    scsi_CDs[target].device->changed = 1;
	    printk("Disc change detected.\n");
	    break;
	  case NOT_READY: /* This happens if there is no disc in drive */
	    printk("CDROM not ready.  Make sure there is a disc in the drive.\n");
	    break;
	  case ILLEGAL_REQUEST:
	    printk("CDROM (ioctl) reports ILLEGAL REQUEST.\n");
	    break;
	  default:
	    printk("SCSI CD error: host %d id %d lun %d return code = %03x\n", 
		   scsi_CDs[target].device->host->host_no, 
		   scsi_CDs[target].device->id,
		   scsi_CDs[target].device->lun,
		   result);
	    printk("\tSense class %x, sense error %x, extended sense %x\n",
		   sense_class(SCpnt->sense_buffer[0]), 
		   sense_error(SCpnt->sense_buffer[0]),
		   SCpnt->sense_buffer[2] & 0xf);
	    
	};

	result = SCpnt->result;
	SCpnt->request.dev = -1; /* Deallocate */
	wake_up(&scsi_devices[SCpnt->index].device_wait);
	/* Wake up a process waiting for device*/
      	return result;
}

int sr_ioctl(struct inode * inode, struct file * file, unsigned int cmd, unsigned long arg)
{
        u_char 	sr_cmd[10];

	int dev = inode->i_rdev;
	int result, target;

	target = MINOR(dev);
	if (target >= NR_SR) return -ENODEV;

	switch (cmd) 
		{
		/* Sun-compatible */
		case CDROMPAUSE:

			sr_cmd[0] = SCMD_PAUSE_RESUME;
			sr_cmd[1] = scsi_CDs[target].device->lun << 5;
			sr_cmd[2] = sr_cmd[3] = sr_cmd[4] = 0;
			sr_cmd[5] = sr_cmd[6] = sr_cmd[7] = 0;
			sr_cmd[8] = 0;
			sr_cmd[9] = 0;

			result = do_ioctl(target, sr_cmd, NULL, 255);
			return result;

		case CDROMRESUME:

			sr_cmd[0] = SCMD_PAUSE_RESUME;
			sr_cmd[1] = scsi_CDs[target].device->lun << 5;
			sr_cmd[2] = sr_cmd[3] = sr_cmd[4] = 0;
			sr_cmd[5] = sr_cmd[6] = sr_cmd[7] = 0;
			sr_cmd[8] = 1;
			sr_cmd[9] = 0;

			result = do_ioctl(target, sr_cmd, NULL, 255);

			return result;

		case CDROMPLAYMSF:
			{
			struct cdrom_msf msf;
			memcpy_fromfs(&msf, (void *) arg, sizeof(msf));

			sr_cmd[0] = SCMD_PLAYAUDIO_MSF;
			sr_cmd[1] = scsi_CDs[target].device->lun << 5;
			sr_cmd[2] = 0;
			sr_cmd[3] = msf.cdmsf_min0;
			sr_cmd[4] = msf.cdmsf_sec0;
			sr_cmd[5] = msf.cdmsf_frame0;
			sr_cmd[6] = msf.cdmsf_min1;
			sr_cmd[7] = msf.cdmsf_sec1;
			sr_cmd[8] = msf.cdmsf_frame1;
			sr_cmd[9] = 0;

			result = do_ioctl(target, sr_cmd, NULL, 255);
			return result;
			}

		case CDROMPLAYTRKIND:
			{
			struct cdrom_ti ti;
			memcpy_fromfs(&ti, (void *) arg, sizeof(ti));

			sr_cmd[0] = SCMD_PLAYAUDIO_TI;
			sr_cmd[1] = scsi_CDs[target].device->lun << 5;
			sr_cmd[2] = 0;
			sr_cmd[3] = 0;
			sr_cmd[4] = ti.cdti_trk0;
			sr_cmd[5] = ti.cdti_ind0;
			sr_cmd[6] = 0;
			sr_cmd[7] = ti.cdti_trk1;
			sr_cmd[8] = ti.cdti_ind1;
			sr_cmd[9] = 0;

			result = do_ioctl(target, sr_cmd, NULL, 255);

			return result;
			}

		case CDROMREADTOCHDR:
			{
			struct cdrom_tochdr tochdr;
			char * buffer;

			sr_cmd[0] = SCMD_READ_TOC;
			sr_cmd[1] = ((scsi_CDs[target].device->lun) << 5) | 0x02;    /* MSF format */
			sr_cmd[2] = sr_cmd[3] = sr_cmd[4] = sr_cmd[5] = 0;
			sr_cmd[6] = 0;
			sr_cmd[7] = 0;              /* MSB of length (12) */
			sr_cmd[8] = 12;             /* LSB of length */
			sr_cmd[9] = 0;

			buffer = (unsigned char *) scsi_malloc(512);
			if(!buffer) return -ENOMEM;

			result = do_ioctl(target, sr_cmd, buffer, 12);

			tochdr.cdth_trk0 = buffer[2];
			tochdr.cdth_trk1 = buffer[3];

			scsi_free(buffer, 512);

			verify_area (VERIFY_WRITE, (void *) arg, sizeof (struct cdrom_tochdr));
			memcpy_tofs ((void *) arg, &tochdr, sizeof (struct cdrom_tochdr));
			
			return result;
		        }

		case CDROMREADTOCENTRY:
			{
			struct cdrom_tocentry tocentry;
			char * buffer;

			verify_area (VERIFY_READ, (void *) arg, sizeof (struct cdrom_tocentry));
			memcpy_fromfs (&tocentry, (void *) arg, sizeof (struct cdrom_tocentry));

			sr_cmd[0] = SCMD_READ_TOC;
			sr_cmd[1] = ((scsi_CDs[target].device->lun) << 5) | 0x02;    /* MSF format */
			sr_cmd[2] = sr_cmd[3] = sr_cmd[4] = sr_cmd[5] = 0;
			sr_cmd[6] = tocentry.cdte_track;
			sr_cmd[7] = 0;             /* MSB of length (12)  */
			sr_cmd[8] = 12;            /* LSB of length */
			sr_cmd[9] = 0;

			buffer = (unsigned char *) scsi_malloc(512);
			if(!buffer) return -ENOMEM;

			result = do_ioctl (target, sr_cmd, buffer, 12);

			if (tocentry.cdte_format == CDROM_MSF) {
			  tocentry.cdte_addr.msf.minute = buffer[9];
			  tocentry.cdte_addr.msf.second = buffer[10];
			  tocentry.cdte_addr.msf.frame = buffer[11];
			  tocentry.cdte_ctrl = buffer[5] & 0xf;
			}
			else
			  tocentry.cdte_addr.lba = (int) buffer[0];

			scsi_free(buffer, 512);

			verify_area (VERIFY_WRITE, (void *) arg, sizeof (struct cdrom_tocentry));
			memcpy_tofs ((void *) arg, &tocentry, sizeof (struct cdrom_tocentry));

			return result;
		        }

		case CDROMSTOP:
		        sr_cmd[0] = START_STOP;
			sr_cmd[1] = ((scsi_CDs[target].device->lun) << 5) | 1;
			sr_cmd[2] = sr_cmd[3] = sr_cmd[5] = 0;
			sr_cmd[4] = 0;

			result = do_ioctl(target, sr_cmd, NULL, 255);
			return result;
			
		case CDROMSTART:
		        sr_cmd[0] = START_STOP;
			sr_cmd[1] = ((scsi_CDs[target].device->lun) << 5) | 1;
			sr_cmd[2] = sr_cmd[3] = sr_cmd[5] = 0;
			sr_cmd[4] = 1;

			result = do_ioctl(target, sr_cmd, NULL, 255);
			return result;

		case CDROMEJECT:
			if (scsi_CDs[target].device -> access_count == 1)
			  sr_ioctl (inode, NULL, SCSI_IOCTL_DOORUNLOCK, 0);

		        sr_cmd[0] = START_STOP;
			sr_cmd[1] = ((scsi_CDs[target].device -> lun) << 5) | 1;
			sr_cmd[2] = sr_cmd[3] = sr_cmd[5] = 0;
			sr_cmd[4] = 0x02;

			if (!(result = do_ioctl(target, sr_cmd, NULL, 255)))
			  scsi_CDs[target].device -> changed = 1;

			return result;

		case CDROMVOLCTRL:
			{
			  char * buffer, * mask;
			  struct cdrom_volctrl volctrl;

			  verify_area (VERIFY_READ, (void *) arg, sizeof (struct cdrom_volctrl));
			  memcpy_fromfs (&volctrl, (void *) arg, sizeof (struct cdrom_volctrl));

			  /* First we get the current params so we can just twiddle the volume */

			  sr_cmd[0] = MODE_SENSE;
			  sr_cmd[1] = (scsi_CDs[target].device -> lun) << 5;
			  sr_cmd[2] = 0xe;    /* Want mode page 0xe, CDROM audio params */
			  sr_cmd[3] = 0;
			  sr_cmd[4] = 28;
			  sr_cmd[5] = 0;

			  buffer = (unsigned char *) scsi_malloc(512);
			  if(!buffer) return -ENOMEM;

			  if ((result = do_ioctl (target, sr_cmd, buffer, 28))) {
			    printk ("Hosed while obtaining audio mode page\n");
			    scsi_free(buffer, 512);
			    return result;
			  }

			  sr_cmd[0] = MODE_SENSE;
			  sr_cmd[1] = (scsi_CDs[target].device -> lun) << 5;
			  sr_cmd[2] = 0x4e;   /* Want the mask for mode page 0xe */
			  sr_cmd[3] = 0;
			  sr_cmd[4] = 28;
			  sr_cmd[5] = 0;

			  mask = (unsigned char *) scsi_malloc(512);
			  if(!mask) {
			    scsi_free(buffer, 512);
			    return -ENOMEM;
			  };

			  if ((result = do_ioctl (target, sr_cmd, mask, 28))) {
			    printk ("Hosed while obtaining mask for audio mode page\n");
			    scsi_free(buffer, 512);
			    scsi_free(mask, 512);
			    return result;
			  }

			  /* Now mask and substitute our own volume and reuse the rest */
			  buffer[0] = 0;  /* Clear reserved field */

			  buffer[21] = volctrl.channel0 & mask[21];
			  buffer[23] = volctrl.channel1 & mask[23];
			  buffer[25] = volctrl.channel2 & mask[25];
			  buffer[27] = volctrl.channel3 & mask[27];

			  sr_cmd[0] = MODE_SELECT;
			  sr_cmd[1] = ((scsi_CDs[target].device -> lun) << 5) | 0x10;    /* Params are SCSI-2 */
			  sr_cmd[2] = sr_cmd[3] = 0;
			  sr_cmd[4] = 28;
			  sr_cmd[5] = 0;

			  result = do_ioctl (target, sr_cmd, buffer, 28);
			  scsi_free(buffer, 512);
			  scsi_free(mask, 512);
			  return result;
			}

		case CDROMSUBCHNL:
			{
			  struct cdrom_subchnl subchnl;
			  char * buffer;
			  
			  sr_cmd[0] = SCMD_READ_SUBCHANNEL;
			  sr_cmd[1] = ((scsi_CDs[target].device->lun) << 5) | 0x02;    /* MSF format */
			  sr_cmd[2] = 0x40;    /* I do want the subchannel info */
			  sr_cmd[3] = 0x01;    /* Give me current position info */
			  sr_cmd[4] = sr_cmd[5] = 0;
			  sr_cmd[6] = 0;
			  sr_cmd[7] = 0;
			  sr_cmd[8] = 16;
			  sr_cmd[9] = 0;

			  buffer = (unsigned char*) scsi_malloc(512);
			  if(!buffer) return -ENOMEM;

			  result = do_ioctl(target, sr_cmd, buffer, 16);

			  subchnl.cdsc_audiostatus = buffer[1];
			  subchnl.cdsc_format = CDROM_MSF;
			  subchnl.cdsc_ctrl = buffer[5] & 0xf;
			  subchnl.cdsc_trk = buffer[6];
			  subchnl.cdsc_ind = buffer[7];

			  subchnl.cdsc_reladdr.msf.minute = buffer[13];
			  subchnl.cdsc_reladdr.msf.second = buffer[14];
			  subchnl.cdsc_reladdr.msf.frame = buffer[15];
			  subchnl.cdsc_absaddr.msf.minute = buffer[9];
			  subchnl.cdsc_absaddr.msf.second = buffer[10];
			  subchnl.cdsc_absaddr.msf.frame = buffer[11];

			  scsi_free(buffer, 512);

			  verify_area (VERIFY_WRITE, (void *) arg, sizeof (struct cdrom_subchnl));
			  memcpy_tofs ((void *) arg, &subchnl, sizeof (struct cdrom_subchnl));
			  return result;
			}

		case CDROMREADMODE2:
			return -EINVAL;
		case CDROMREADMODE1:
			return -EINVAL;

		RO_IOCTLS(dev,arg);
		default:
			return scsi_ioctl(scsi_CDs[target].device,cmd,(void *) arg);
		}
}
