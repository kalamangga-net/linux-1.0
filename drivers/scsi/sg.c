/*
   History:
    Started: Aug 9 by Lawrence Foard (entropy@world.std.com), to allow user 
     process control of SCSI devices.
    Development Sponsored by Killy Corp. NY NY
    
    Borrows code from st driver.
*/

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/mtio.h>
#include <linux/ioctl.h>
#include <linux/fcntl.h>
#include <asm/io.h>
#include <asm/segment.h>
#include <asm/system.h>

#include "../block/blk.h"
#include "scsi.h"
#include "scsi_ioctl.h"
#include "sg.h"

int NR_SG=0;
int MAX_SG=0;

#ifdef SG_BIG_BUFF
static char *big_buff;
static struct wait_queue *big_wait;   /* wait for buffer available */
static int big_inuse=0;
#endif

struct scsi_generic
 {
  Scsi_Device *device;
  int users;   /* how many people have it open? */
  struct wait_queue *generic_wait; /* wait for device to be available */
  struct wait_queue *read_wait;    /* wait for response */
  struct wait_queue *write_wait;   /* wait for free buffer */
  int timeout; /* current default value for device */
  int buff_len; /* length of current buffer */
  char *buff;   /* the buffer */
  struct sg_header header; /* header of pending command */
  char exclude; /* opened for exclusive access */
  char pending;  /* don't accept writes now */
  char complete; /* command complete allow a read */
 };

static struct scsi_generic *scsi_generics=NULL;

static int sg_ioctl(struct inode * inode,struct file * file,
	     unsigned int cmd_in, unsigned long arg)
 {
  int dev = MINOR(inode->i_rdev);
  if ((dev<0) || (dev>=NR_SG))
   return -ENODEV;
  switch(cmd_in)
   {
    case SG_SET_TIMEOUT:
     scsi_generics[dev].timeout=get_fs_long((int *) arg);
     return 0;
    case SG_GET_TIMEOUT:
     return scsi_generics[dev].timeout;
    default:
     return scsi_ioctl(scsi_generics[dev].device, cmd_in, (void *) arg);
   }
 }

static int sg_open(struct inode * inode, struct file * filp)
 {
  int dev=MINOR(inode->i_rdev);
  int flags=filp->f_flags;
  if (dev>=NR_SG)
   return -ENODEV;
  if (O_RDWR!=(flags & O_ACCMODE))
   return -EACCES;
  if (flags & O_EXCL)
   {
    while(scsi_generics[dev].users)
     {
      if (flags & O_NONBLOCK)
       return -EBUSY;
      interruptible_sleep_on(&scsi_generics[dev].generic_wait);
      if (current->signal & ~current->blocked)
       return -ERESTARTSYS;
     }
    scsi_generics[dev].exclude=1;
   }
  else
   while(scsi_generics[dev].exclude)
    {
     if (flags & O_NONBLOCK)
      return -EBUSY;
     interruptible_sleep_on(&scsi_generics[dev].generic_wait);
     if (current->signal & ~current->blocked)
      return -ERESTARTSYS;
    }
  if (!scsi_generics[dev].users && scsi_generics[dev].pending && scsi_generics[dev].complete)
   {
    scsi_free(scsi_generics[dev].buff,scsi_generics[dev].buff_len);
    scsi_generics[dev].pending=0;
   }
  if (!scsi_generics[dev].users)
   scsi_generics[dev].timeout=SG_DEFAULT_TIMEOUT;
  scsi_generics[dev].users++;
  return 0;
 }

static void sg_close(struct inode * inode, struct file * filp)
 {
  int dev=MINOR(inode->i_rdev);
  scsi_generics[dev].users--;
  scsi_generics[dev].exclude=0;
  wake_up(&scsi_generics[dev].generic_wait);
 }

static char *sg_malloc(int size)
 {
  if (size<=4096)
   return (char *) scsi_malloc(size);
#ifdef SG_BIG_BUFF
  if (size<SG_BIG_BUFF)
   {
    while(big_inuse)
     {
      interruptible_sleep_on(&big_wait);
      if (current->signal & ~current->blocked)
       return NULL;
     }
    big_inuse=1;
    return big_buff;
   }
#endif   
  return NULL;
 }

static void sg_free(char *buff,int size) 
 {
#ifdef SG_BIG_BUFF
  if (buff==big_buff)
   {
    big_inuse=0;
    wake_up(&big_wait);
    return;
   }
#endif
  scsi_free(buff,size);
 }

static int sg_read(struct inode *inode,struct file *filp,char *buf,int count)
 {
  int dev=MINOR(inode->i_rdev);
  int i;
  struct scsi_generic *device=&scsi_generics[dev];
  if ((i=verify_area(VERIFY_WRITE,buf,count)))
   return i;
  while(!device->pending || !device->complete)
   {
    if (filp->f_flags & O_NONBLOCK)
     return -EWOULDBLOCK;
    interruptible_sleep_on(&device->read_wait);
    if (current->signal & ~current->blocked)
     return -ERESTARTSYS;
   }
  device->header.pack_len=device->header.reply_len;
  device->header.result=0;
  if (count>=sizeof(struct sg_header))
   {
    memcpy_tofs(buf,&device->header,sizeof(struct sg_header));
    buf+=sizeof(struct sg_header);
    if (count>device->header.pack_len)
     count=device->header.pack_len;
    memcpy_tofs(buf,device->buff,count-sizeof(struct sg_header));
   }
  else
   count=0;
  sg_free(device->buff,device->buff_len);
  device->pending=0;
  wake_up(&device->write_wait);
  return count;
 }

static void sg_command_done(Scsi_Cmnd * SCpnt)
 {
  int dev=SCpnt->request.dev;
  struct scsi_generic *device=&scsi_generics[dev];
  if (!device->pending)
   {
    printk("unexpected done for sg %d\n",dev);
    SCpnt->request.dev=-1;
    return;
   }
  if (SCpnt->sense_buffer[0])
   {
    device->header.result=EIO;
   }
  else
   device->header.result=SCpnt->result;
  device->complete=1;
  SCpnt->request.dev=-1;
  wake_up(&scsi_generics[dev].read_wait);
 }

static int sg_write(struct inode *inode,struct file *filp,char *buf,int count)
 {
  int dev=MINOR(inode->i_rdev);
  Scsi_Cmnd *SCpnt;
  int bsize,size,amt,i;
  unsigned char cmnd[MAX_COMMAND_SIZE];
  struct scsi_generic *device=&scsi_generics[dev];
  if ((i=verify_area(VERIFY_READ,buf,count)))
   return i;
  if (count<sizeof(struct sg_header))
   return -EIO;
  /* make sure we can fit */
  while(device->pending)
   {
    if (filp->f_flags & O_NONBLOCK)
     return -EWOULDBLOCK;
#ifdef DEBUG
    printk("sg_write: sleeping on pending request\n");
#endif     
    interruptible_sleep_on(&device->write_wait);
    if (current->signal & ~current->blocked)
     return -ERESTARTSYS;
   }
  device->pending=1;
  device->complete=0;
  memcpy_fromfs(&device->header,buf,sizeof(struct sg_header));
  /* fix input size */
  device->header.pack_len=count;
  buf+=sizeof(struct sg_header);
  bsize=(device->header.pack_len>device->header.reply_len) ? device->header.pack_len : device->header.reply_len;
  bsize-=sizeof(struct sg_header);
  amt=bsize;
  if (!bsize)
   bsize++;
  bsize=(bsize+511) & ~511;
  if ((bsize<0) || !(device->buff=sg_malloc(device->buff_len=bsize)))
   {
    device->pending=0;
    wake_up(&device->write_wait);
    return -ENOMEM;
   }
#ifdef DEBUG
  printk("allocating device\n");
#endif
  if (!(SCpnt=allocate_device(NULL,device->device->index, !(filp->f_flags & O_NONBLOCK))))
   {
    device->pending=0;
    wake_up(&device->write_wait);
    sg_free(device->buff,device->buff_len);
    return -EWOULDBLOCK;
   } 
#ifdef DEBUG
  printk("device allocated\n");
#endif    
  /* now issue command */
  SCpnt->request.dev=dev;
  SCpnt->sense_buffer[0]=0;
  size=COMMAND_SIZE(get_fs_byte(buf));
  memcpy_fromfs(cmnd,buf,size);
  buf+=size;
  memcpy_fromfs(device->buff,buf,device->header.pack_len-size-sizeof(struct sg_header));
  cmnd[1]=(cmnd[1] & 0x1f) | (device->device->lun<<5);
#ifdef DEBUG
  printk("do cmd\n");
#endif
  scsi_do_cmd (SCpnt,(void *) cmnd,
               (void *) device->buff,amt,sg_command_done,device->timeout,SG_DEFAULT_RETRIES);
#ifdef DEBUG
  printk("done cmd\n");
#endif               
  return count;
 }

static struct file_operations sg_fops = {
   NULL,            /* lseek */
   sg_read,         /* read */
   sg_write,        /* write */
   NULL,            /* readdir */
   NULL,            /* select */
   sg_ioctl,        /* ioctl */
   NULL,            /* mmap */
   sg_open,         /* open */
   sg_close,        /* release */
   NULL		    /* fsync */
};


/* Driver initialization */
unsigned long sg_init(unsigned long mem_start, unsigned long mem_end)
 {
  if (register_chrdev(SCSI_GENERIC_MAJOR,"sg",&sg_fops)) 
   {
    printk("Unable to get major %d for generic SCSI device\n",
	   SCSI_GENERIC_MAJOR);
    return mem_start;
   }
  if (NR_SG == 0) return mem_start;

#ifdef DEBUG
  printk("sg: Init generic device.\n");
#endif

#ifdef SG_BIG_BUFF
  big_buff= (char *) mem_start;
  mem_start+=SG_BIG_BUFF;
#endif
  return mem_start;
 }

unsigned long sg_init1(unsigned long mem_start, unsigned long mem_end)
 {
  scsi_generics = (struct scsi_generic *) mem_start;
  mem_start += MAX_SG * sizeof(struct scsi_generic);
  return mem_start;
 };

void sg_attach(Scsi_Device * SDp)
 {
  if(NR_SG >= MAX_SG) 
   panic ("scsi_devices corrupt (sg)");
  scsi_generics[NR_SG].device=SDp;
  scsi_generics[NR_SG].users=0;
  scsi_generics[NR_SG].generic_wait=NULL;
  scsi_generics[NR_SG].read_wait=NULL;
  scsi_generics[NR_SG].write_wait=NULL;
  scsi_generics[NR_SG].exclude=0;
  scsi_generics[NR_SG].pending=0;
  scsi_generics[NR_SG].timeout=SG_DEFAULT_TIMEOUT;
  NR_SG++;
 };
