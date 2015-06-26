#include <asm/io.h>
#include <asm/segment.h>
#include <asm/system.h>

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>

#include "../block/blk.h"
#include "scsi.h"
#include "hosts.h"
#include "scsi_ioctl.h"

#define MAX_RETRIES 5	
#define MAX_TIMEOUT 200
#define MAX_BUF 4096

#define max(a,b) (((a) > (b)) ? (a) : (b))

/*
 * If we are told to probe a host, we will return 0 if  the host is not
 * present, 1 if the host is present, and will return an identifying
 * string at *arg, if arg is non null, filling to the length stored at
 * (int *) arg
 */

static int ioctl_probe(struct Scsi_Host * host, void *buffer)
{
	int temp;
	unsigned int len,slen;
	const char * string;
	
	if ((temp = host->hostt->present) && buffer) {
		len = get_fs_long ((unsigned long *) buffer);
		string = host->hostt->info();
		slen = strlen(string);
		if (len > slen)
			len = slen + 1;
		verify_area(VERIFY_WRITE, buffer, len);
		memcpy_tofs (buffer, string, len);
	}
	return temp;
}

/*
 * 
 * The SCSI_IOCTL_SEND_COMMAND ioctl sends a command out to the SCSI host.
 * The MAX_TIMEOUT and MAX_RETRIES  variables are used.  
 * 
 * dev is the SCSI device struct ptr, *(int *) arg is the length of the
 * input data, if any, not including the command string & counts, 
 * *((int *)arg + 1) is the output buffer size in bytes.
 * 
 * *(char *) ((int *) arg)[2] the actual command byte.   
 * 
 * Note that no more than MAX_BUF data bytes will be transfered.  Since
 * SCSI block device size is 512 bytes, I figured 1K was good.
 * but (WDE) changed it to 8192 to handle large bad track buffers.
 * ERY: I changed this to a dynamic allocation using scsi_malloc - we were
 * getting a kernel stack overflow which was crashing the system when we
 * were using 8192 bytes.
 * 
 * This size *does not* include the initial lengths that were passed.
 * 
 * The SCSI command is read from the memory location immediately after the
 * length words, and the input data is right after the command.  The SCSI
 * routines know the command size based on the opcode decode.  
 * 
 * The output area is then filled in starting from the command byte. 
 */

static void scsi_ioctl_done (Scsi_Cmnd * SCpnt)
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

static int ioctl_internal_command(Scsi_Device *dev, char * cmd)
{
	int result;
	Scsi_Cmnd * SCpnt;

	SCpnt = allocate_device(NULL, dev->index, 1);
	scsi_do_cmd(SCpnt,  cmd, NULL,  0,
			scsi_ioctl_done,  MAX_TIMEOUT,
			MAX_RETRIES);

	if (SCpnt->request.dev != 0xfffe){
	  SCpnt->request.waiting = current;
	  current->state = TASK_UNINTERRUPTIBLE;
	  while (SCpnt->request.dev != 0xfffe) schedule();
	};

	if(driver_byte(SCpnt->result) != 0)
	  switch(SCpnt->sense_buffer[2] & 0xf) {
	  case ILLEGAL_REQUEST:
	    if(cmd[0] == ALLOW_MEDIUM_REMOVAL) dev->lockable = 0;
	    else printk("SCSI device (ioctl) reports ILLEGAL REQUEST.\n");
	    break;
	  case NOT_READY: /* This happens if there is no disc in drive */
	    if(dev->removable){
	      printk("Device not ready.  Make sure there is a disc in the drive.\n");
	      break;
	    };
	  case UNIT_ATTENTION:
	    if (dev->removable){
	      dev->changed = 1;
	      SCpnt->result = 0; /* This is no longer considered an error */
	      printk("Disc change detected.\n");
	      break;
	    };
	  default: /* Fall through for non-removable media */
	    printk("SCSI CD error: host %d id %d lun %d return code = %x\n",
		   dev->host->host_no,
		   dev->id,
		   dev->lun,
		   SCpnt->result);
	    printk("\tSense class %x, sense error %x, extended sense %x\n",
		   sense_class(SCpnt->sense_buffer[0]),
		   sense_error(SCpnt->sense_buffer[0]),
		   SCpnt->sense_buffer[2] & 0xf);

	  };

	result = SCpnt->result;
	SCpnt->request.dev = -1;  /* Mark as not busy */
	wake_up(&scsi_devices[SCpnt->index].device_wait);
	return result;
}

static int ioctl_command(Scsi_Device *dev, void *buffer)
{
	char * buf;
	char cmd[12];
	char * cmd_in;
	Scsi_Cmnd * SCpnt;
	unsigned char opcode;
	int inlen, outlen, cmdlen;
	int needed;
	int result;

	if (!buffer)
		return -EINVAL;
	
	inlen = get_fs_long((unsigned long *) buffer);
	outlen = get_fs_long( ((unsigned long *) buffer) + 1);

	cmd_in = (char *) ( ((int *)buffer) + 2);
	opcode = get_fs_byte(cmd_in); 

	needed = (inlen > outlen ? inlen : outlen);
	if(needed){
	  needed = (needed + 511) & ~511;
	  if (needed > MAX_BUF) needed = MAX_BUF;
	  buf = (char *) scsi_malloc(needed);
	  if (!buf) return -ENOMEM;
	} else
	  buf = NULL;

	memcpy_fromfs ((void *) cmd,  cmd_in,  cmdlen = COMMAND_SIZE (opcode));
	memcpy_fromfs ((void *) buf,  (void *) (cmd_in + cmdlen), inlen > MAX_BUF ? MAX_BUF : inlen);

	cmd[1] = ( cmd[1] & 0x1f ) | (dev->lun << 5);

#ifndef DEBUG_NO_CMD
	
	SCpnt = allocate_device(NULL, dev->index, 1);

	scsi_do_cmd(SCpnt,  cmd,  buf, needed,  scsi_ioctl_done,  MAX_TIMEOUT, 
			MAX_RETRIES);

	if (SCpnt->request.dev != 0xfffe){
	  SCpnt->request.waiting = current;
	  current->state = TASK_UNINTERRUPTIBLE;
	  while (SCpnt->request.dev != 0xfffe) schedule();
	};


	/* If there was an error condition, pass the info back to the user. */
	if(SCpnt->result) {
	  result = verify_area(VERIFY_WRITE, cmd_in, sizeof(SCpnt->sense_buffer));
	  if (result)
	    return result;
	  memcpy_tofs((void *) cmd_in,  SCpnt->sense_buffer, sizeof(SCpnt->sense_buffer));
	} else {

	  result = verify_area(VERIFY_WRITE, cmd_in, (outlen > MAX_BUF) ? MAX_BUF  : outlen);
	  if (result)
	    return result;
	  memcpy_tofs ((void *) cmd_in,  buf,  (outlen > MAX_BUF) ? MAX_BUF  : outlen);
	};
	result = SCpnt->result;
	SCpnt->request.dev = -1;  /* Mark as not busy */
	if (buf) scsi_free(buf, needed);
	wake_up(&scsi_devices[SCpnt->index].device_wait);
	return result;
#else
	{
	int i;
	printk("scsi_ioctl : device %d.  command = ", dev->id);
	for (i = 0; i < 12; ++i)
		printk("%02x ", cmd[i]);
	printk("\nbuffer =");
	for (i = 0; i < 20; ++i)
		printk("%02x ", buf[i]);
	printk("\n");
	printk("inlen = %d, outlen = %d, cmdlen = %d\n",
		inlen, outlen, cmdlen);
	printk("buffer = %d, cmd_in = %d\n", buffer, cmd_in);
	}
	return 0;
#endif
}

	

/*
	the scsi_ioctl() function differs from most ioctls in that it does
	not take a major/minor number as the dev filed.  Rather, it takes
	a pointer to a scsi_devices[] element, a structure. 
*/
int scsi_ioctl (Scsi_Device *dev, int cmd, void *arg)
{
        char scsi_cmd[12];

	if ((cmd != 0 && dev->index > NR_SCSI_DEVICES))
		return -ENODEV;
	
	switch (cmd) {
	        case SCSI_IOCTL_GET_IDLUN:
	                verify_area(VERIFY_WRITE, (void *) arg, sizeof(int));
			put_fs_long(dev->id + (dev->lun << 8) + 
				    (dev->host->host_no << 16), (unsigned long *) arg);
			return 0;
		case SCSI_IOCTL_TAGGED_ENABLE:
			if(!suser())  return -EACCES;
			if(!dev->tagged_supported) return -EINVAL;
			dev->tagged_queue = 1;
			dev->current_tag = 1;
			break;
		case SCSI_IOCTL_TAGGED_DISABLE:
			if(!suser())  return -EACCES;
			if(!dev->tagged_supported) return -EINVAL;
			dev->tagged_queue = 0;
			dev->current_tag = 0;
			break;
		case SCSI_IOCTL_PROBE_HOST:
			return ioctl_probe(dev->host, arg);
		case SCSI_IOCTL_SEND_COMMAND:
			if(!suser())  return -EACCES;
			return ioctl_command((Scsi_Device *) dev, arg);
		case SCSI_IOCTL_DOORLOCK:
			if (!dev->removable || !dev->lockable) return 0;
		        scsi_cmd[0] = ALLOW_MEDIUM_REMOVAL;
			scsi_cmd[1] = dev->lun << 5;
			scsi_cmd[2] = scsi_cmd[3] = scsi_cmd[5] = 0;
			scsi_cmd[4] = SCSI_REMOVAL_PREVENT;
			return ioctl_internal_command((Scsi_Device *) dev, scsi_cmd);
			break;
		case SCSI_IOCTL_DOORUNLOCK:
			if (!dev->removable || !dev->lockable) return 0;
		        scsi_cmd[0] = ALLOW_MEDIUM_REMOVAL;
			scsi_cmd[1] = dev->lun << 5;
			scsi_cmd[2] = scsi_cmd[3] = scsi_cmd[5] = 0;
			scsi_cmd[4] = SCSI_REMOVAL_ALLOW;
			return ioctl_internal_command((Scsi_Device *) dev, scsi_cmd);
		case SCSI_IOCTL_TEST_UNIT_READY:
		        scsi_cmd[0] = TEST_UNIT_READY;
			scsi_cmd[1] = dev->lun << 5;
			scsi_cmd[2] = scsi_cmd[3] = scsi_cmd[5] = 0;
			scsi_cmd[4] = 0;
			return ioctl_internal_command((Scsi_Device *) dev, scsi_cmd);
			break;
		default :			
			return -EINVAL;
	}
	return -EINVAL;
}

/*
 * Just like scsi_ioctl, only callable from kernel space with no 
 * fs segment fiddling.
 */

int kernel_scsi_ioctl (Scsi_Device *dev, int cmd, void *arg) {
  unsigned long oldfs;
  int tmp;
  oldfs = get_fs();
  set_fs(get_ds());
  tmp = scsi_ioctl (dev, cmd, arg);
  set_fs(oldfs);
  return tmp;
}

