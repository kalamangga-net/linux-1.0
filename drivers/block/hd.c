/*
 *  linux/kernel/hd.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * This is the low-level hd interrupt support. It traverses the
 * request-list, using interrupts to jump between functions. As
 * all the functions are called within interrupts, we may not
 * sleep. Special care is recommended.
 * 
 *  modified by Drew Eckhardt to check nr of hd's from the CMOS.
 *
 *  Thanks to Branko Lankester, lankeste@fwi.uva.nl, who found a bug
 *  in the early extended-partition checks and added DM partitions
 */


#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/hdreg.h>
#include <linux/genhd.h>
#include <linux/config.h>

#define REALLY_SLOW_IO
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#define MAJOR_NR HD_MAJOR
#include "blk.h"

#define HD_IRQ 14

static int revalidate_hddisk(int, int);

static inline unsigned char CMOS_READ(unsigned char addr)
{
	outb_p(addr,0x70);
	return inb_p(0x71);
}

#define	HD_DELAY	0

#define MAX_ERRORS     16	/* Max read/write errors/sector */
#define RESET_FREQ      8	/* Reset controller every 8th retry */
#define RECAL_FREQ      4	/* Recalibrate every 4th retry */
#define MAX_HD		2

static void recal_intr(void);
static void bad_rw_intr(void);

static char recalibrate[ MAX_HD ] = { 0, };
static int access_count[MAX_HD] = {0, };
static char busy[MAX_HD] = {0, };
static struct wait_queue * busy_wait = NULL;

static int reset = 0;
static int hd_error = 0;

#if (HD_DELAY > 0)
unsigned long last_req, read_timer();
#endif

/*
 *  This struct defines the HD's and their types.
 */
struct hd_i_struct {
	unsigned int head,sect,cyl,wpcom,lzone,ctl;
	};
#ifdef HD_TYPE
struct hd_i_struct hd_info[] = { HD_TYPE };
static int NR_HD = ((sizeof (hd_info))/(sizeof (struct hd_i_struct)));
#else
struct hd_i_struct hd_info[] = { {0,0,0,0,0,0},{0,0,0,0,0,0} };
static int NR_HD = 0;
#endif

static struct hd_struct hd[MAX_HD<<6]={{0,0},};
static int hd_sizes[MAX_HD<<6] = {0, };
static int hd_blocksizes[MAX_HD<<6] = {0, };

#if (HD_DELAY > 0)
unsigned long read_timer(void)
{
	unsigned long t;
	int i;

	cli();
	t = jiffies * 11932;
    	outb_p(0, 0x43);
	i = inb_p(0x40);
	i |= inb(0x40) << 8;
	sti();
	return(t - i);
}
#endif

void hd_setup(char *str, int *ints)
{
	int hdind = 0;

	if (ints[0] != 3)
		return;
	if (hd_info[0].head != 0)
		hdind=1;
	hd_info[hdind].head = ints[2];
	hd_info[hdind].sect = ints[3];
	hd_info[hdind].cyl = ints[1];
	hd_info[hdind].wpcom = 0;
	hd_info[hdind].lzone = ints[1];
	hd_info[hdind].ctl = (ints[2] > 8 ? 8 : 0);
	NR_HD = hdind+1;
}

static int win_result(void)
{
	int i=inb_p(HD_STATUS);

	if ((i & (BUSY_STAT | READY_STAT | WRERR_STAT | SEEK_STAT | ERR_STAT))
		== (READY_STAT | SEEK_STAT)) {
	        hd_error = 0;
		return 0; /* ok */
	}
	printk("HD: win_result: status = 0x%02x\n",i);
	if (i&1) {
		hd_error = inb(HD_ERROR);
		printk("HD: win_result: error = 0x%02x\n",hd_error);
	}	
	return 1;
}

static int controller_busy(void);
static int status_ok(void);

static int controller_ready(unsigned int drive, unsigned int head)
{
	int retry = 100;

	do {
		if (controller_busy() & BUSY_STAT)
			return 0;
		outb_p(0xA0 | (drive<<4) | head, HD_CURRENT);
		if (status_ok())
			return 1;
	} while (--retry);
	return 0;
}

static int status_ok(void)
{
	unsigned char status = inb_p(HD_STATUS);

	if (status & BUSY_STAT)
		return 1;
	if (status & WRERR_STAT)
		return 0;
	if (!(status & READY_STAT))
		return 0;
	if (!(status & SEEK_STAT))
		return 0;
	return 1;
}

static int controller_busy(void)
{
	int retries = 100000;
	unsigned char status;

	do {
		status = inb_p(HD_STATUS);
	} while ((status & BUSY_STAT) && --retries);
	return status;
}

static void hd_out(unsigned int drive,unsigned int nsect,unsigned int sect,
		unsigned int head,unsigned int cyl,unsigned int cmd,
		void (*intr_addr)(void))
{
	unsigned short port;

	if (drive>1 || head>15)
		panic("Trying to write bad sector");
#if (HD_DELAY > 0)
	while (read_timer() - last_req < HD_DELAY)
		/* nothing */;
#endif
	if (reset)
		return;
	if (!controller_ready(drive, head)) {
		reset = 1;
		return;
	}
	SET_INTR(intr_addr);
	outb_p(hd_info[drive].ctl,HD_CMD);
	port=HD_DATA;
	outb_p(hd_info[drive].wpcom>>2,++port);
	outb_p(nsect,++port);
	outb_p(sect,++port);
	outb_p(cyl,++port);
	outb_p(cyl>>8,++port);
	outb_p(0xA0|(drive<<4)|head,++port);
	outb_p(cmd,++port);
}

static int drive_busy(void)
{
	unsigned int i;
	unsigned char c;

	for (i = 0; i < 500000 ; i++) {
		c = inb_p(HD_STATUS);
		c &= (BUSY_STAT | READY_STAT | SEEK_STAT);
		if (c == (READY_STAT | SEEK_STAT))
			return 0;
	}
	printk("HD controller times out, status = 0x%02x\n",c);
	return 1;
}

static void reset_controller(void)
{
	int	i;

	printk(KERN_DEBUG "HD-controller reset\n");
	outb_p(4,HD_CMD);
	for(i = 0; i < 1000; i++) nop();
	outb(hd_info[0].ctl & 0x0f ,HD_CMD);
	if (drive_busy())
		printk("HD-controller still busy\n");
	if ((hd_error = inb(HD_ERROR)) != 1)
		printk("HD-controller reset failed: %02x\n",hd_error);
}

static void reset_hd(void)
{
	static int i;

repeat:
	if (reset) {
		reset = 0;
		i = -1;
		reset_controller();
	} else if (win_result()) {
		bad_rw_intr();
		if (reset)
			goto repeat;
	}
	i++;
	if (i < NR_HD) {
		hd_out(i,hd_info[i].sect,hd_info[i].sect,hd_info[i].head-1,
			hd_info[i].cyl,WIN_SPECIFY,&reset_hd);
		if (reset)
			goto repeat;
	} else
		do_hd_request();
}

/*
 * Ok, don't know what to do with the unexpected interrupts: on some machines
 * doing a reset and a retry seems to result in an eternal loop. Right now I
 * ignore it, and just set the timeout.
 */
void unexpected_hd_interrupt(void)
{
	sti();
	printk(KERN_DEBUG "Unexpected HD interrupt\n");
	SET_TIMER;
}

/*
 * bad_rw_intr() now tries to be a bit smarter and does things
 * according to the error returned by the controller.
 * -Mika Liljeberg (liljeber@cs.Helsinki.FI)
 */
static void bad_rw_intr(void)
{
	int dev;

	if (!CURRENT)
		return;
	dev = MINOR(CURRENT->dev) >> 6;
	if (++CURRENT->errors >= MAX_ERRORS || (hd_error & BBD_ERR)) {
		end_request(0);
		recalibrate[dev] = 1;
	} else if (CURRENT->errors % RESET_FREQ == 0)
		reset = 1;
	else if ((hd_error & TRK0_ERR) || CURRENT->errors % RECAL_FREQ == 0)
		recalibrate[dev] = 1;
	/* Otherwise just retry */
}

static inline int wait_DRQ(void)
{
	int retries = 100000;

	while (--retries > 0)
		if (inb_p(HD_STATUS) & DRQ_STAT)
			return 0;
	return -1;
}

#define STAT_MASK (BUSY_STAT | READY_STAT | WRERR_STAT | SEEK_STAT | ERR_STAT)
#define STAT_OK (READY_STAT | SEEK_STAT)

static void read_intr(void)
{
	int i;
	int retries = 100000;

	do {
		i = (unsigned) inb_p(HD_STATUS);
		if (i & BUSY_STAT)
			continue;
		if ((i & STAT_MASK) != STAT_OK)
			break;
		if (i & DRQ_STAT)
			goto ok_to_read;
	} while (--retries > 0);
	sti();
	printk("HD: read_intr: status = 0x%02x\n",i);
	if (i & ERR_STAT) {
		hd_error = (unsigned) inb(HD_ERROR);
		printk("HD: read_intr: error = 0x%02x\n",hd_error);
	}
	bad_rw_intr();
	cli();
	do_hd_request();
	return;
ok_to_read:
	insw(HD_DATA,CURRENT->buffer,256);
	CURRENT->errors = 0;
	CURRENT->buffer += 512;
	CURRENT->sector++;
	i = --CURRENT->nr_sectors;
	--CURRENT->current_nr_sectors;
#ifdef DEBUG
	printk("hd%d : sector = %d, %d remaining to buffer = %08x\n",
		MINOR(CURRENT->dev), CURRENT->sector, i, CURRENT-> 
		buffer);
#endif
	if (!i || (CURRENT->bh && !SUBSECTOR(i)))
		end_request(1);
	if (i > 0) {
		SET_INTR(&read_intr);
		sti();
		return;
	}
	(void) inb_p(HD_STATUS);
#if (HD_DELAY > 0)
	last_req = read_timer();
#endif
	do_hd_request();
	return;
}

static void write_intr(void)
{
	int i;
	int retries = 100000;

	do {
		i = (unsigned) inb_p(HD_STATUS);
		if (i & BUSY_STAT)
			continue;
		if ((i & STAT_MASK) != STAT_OK)
			break;
		if ((CURRENT->nr_sectors <= 1) || (i & DRQ_STAT))
			goto ok_to_write;
	} while (--retries > 0);
	sti();
	printk("HD: write_intr: status = 0x%02x\n",i);
	if (i & ERR_STAT) {
		hd_error = (unsigned) inb(HD_ERROR);
		printk("HD: write_intr: error = 0x%02x\n",hd_error);
	}
	bad_rw_intr();
	cli();
	do_hd_request();
	return;
ok_to_write:
	CURRENT->sector++;
	i = --CURRENT->nr_sectors;
	--CURRENT->current_nr_sectors;
	CURRENT->buffer += 512;
	if (!i || (CURRENT->bh && !SUBSECTOR(i)))
		end_request(1);
	if (i > 0) {
		SET_INTR(&write_intr);
		outsw(HD_DATA,CURRENT->buffer,256);
		sti();
	} else {
#if (HD_DELAY > 0)
		last_req = read_timer();
#endif
		do_hd_request();
	}
	return;
}

static void recal_intr(void)
{
	if (win_result())
		bad_rw_intr();
	do_hd_request();
}

/*
 * This is another of the error-routines I don't know what to do with. The
 * best idea seems to just set reset, and start all over again.
 */
static void hd_times_out(void)
{
	DEVICE_INTR = NULL;
	sti();
	reset = 1;
	if (!CURRENT)
		return;
	printk(KERN_DEBUG "HD timeout\n");
	cli();
	if (++CURRENT->errors >= MAX_ERRORS) {
#ifdef DEBUG
		printk("hd : too many errors.\n");
#endif
		end_request(0);
	}

	do_hd_request();
}

/*
 * The driver has been modified to enable interrupts a bit more: in order to
 * do this we first (a) disable the timeout-interrupt and (b) clear the
 * device-interrupt. This way the interrupts won't mess with out code (the
 * worst that can happen is that an unexpected HD-interrupt comes in and
 * sets the "reset" variable and starts the timer)
 */
static void do_hd_request(void)
{
	unsigned int block,dev;
	unsigned int sec,head,cyl,track;
	unsigned int nsect;

	if (CURRENT && CURRENT->dev < 0) return;

	if (DEVICE_INTR)
		return;
repeat:
	timer_active &= ~(1<<HD_TIMER);
	sti();
	INIT_REQUEST;
	dev = MINOR(CURRENT->dev);
	block = CURRENT->sector;
	nsect = CURRENT->nr_sectors;
	if (dev >= (NR_HD<<6) || block >= hd[dev].nr_sects) {
#ifdef DEBUG
		printk("hd%d : attempted read for sector %d past end of device at sector %d.\n",
		   	block, hd[dev].nr_sects);
#endif
		end_request(0);
		goto repeat;
	}
	block += hd[dev].start_sect;
	dev >>= 6;
	sec = block % hd_info[dev].sect + 1;
	track = block / hd_info[dev].sect;
	head = track % hd_info[dev].head;
	cyl = track / hd_info[dev].head;
#ifdef DEBUG
	printk("hd%d : cyl = %d, head = %d, sector = %d, buffer = %08x\n",
		dev, cyl, head, sec, CURRENT->buffer);
#endif
	cli();
	if (reset) {
		int i;

		for (i=0; i < NR_HD; i++)
			recalibrate[i] = 1;
		reset_hd();
		sti();
		return;
	}
	if (recalibrate[dev]) {
		recalibrate[dev] = 0;
		hd_out(dev,hd_info[dev].sect,0,0,0,WIN_RESTORE,&recal_intr);
		if (reset)
			goto repeat;
		sti();
		return;
	}	
	if (CURRENT->cmd == WRITE) {
		hd_out(dev,nsect,sec,head,cyl,WIN_WRITE,&write_intr);
		if (reset)
			goto repeat;
		if (wait_DRQ()) {
			printk("HD: do_hd_request: no DRQ\n");
			bad_rw_intr();
			goto repeat;
		}
		outsw(HD_DATA,CURRENT->buffer,256);
		sti();
		return;
	}
	if (CURRENT->cmd == READ) {
		hd_out(dev,nsect,sec,head,cyl,WIN_READ,&read_intr);
		if (reset)
			goto repeat;
		sti();
		return;
	}
	panic("unknown hd-command");
}

static int hd_ioctl(struct inode * inode, struct file * file,
	unsigned int cmd, unsigned long arg)
{
	struct hd_geometry *loc = (struct hd_geometry *) arg;
	int dev, err;

	if (!inode)
		return -EINVAL;
	dev = MINOR(inode->i_rdev) >> 6;
	if (dev >= NR_HD)
		return -EINVAL;
	switch (cmd) {
		case HDIO_GETGEO:
			if (!loc)  return -EINVAL;
			err = verify_area(VERIFY_WRITE, loc, sizeof(*loc));
			if (err)
				return err;
			put_fs_byte(hd_info[dev].head,
				(char *) &loc->heads);
			put_fs_byte(hd_info[dev].sect,
				(char *) &loc->sectors);
			put_fs_word(hd_info[dev].cyl,
				(short *) &loc->cylinders);
			put_fs_long(hd[MINOR(inode->i_rdev)].start_sect,
				(long *) &loc->start);
			return 0;
         	case BLKGETSIZE:   /* Return device size */
			if (!arg)  return -EINVAL;
			err = verify_area(VERIFY_WRITE, (long *) arg, sizeof(long));
			if (err)
				return err;
			put_fs_long(hd[MINOR(inode->i_rdev)].nr_sects,
				(long *) arg);
			return 0;
		case BLKFLSBUF:
			if(!suser())  return -EACCES;
			if(!inode->i_rdev) return -EINVAL;
			fsync_dev(inode->i_rdev);
			invalidate_buffers(inode->i_rdev);
			return 0;

		case BLKRRPART: /* Re-read partition tables */
			return revalidate_hddisk(inode->i_rdev, 1);
		RO_IOCTLS(inode->i_rdev,arg);
		default:
			return -EINVAL;
	}
}

static int hd_open(struct inode * inode, struct file * filp)
{
	int target;
	target =  DEVICE_NR(MINOR(inode->i_rdev));

	while (busy[target])
		sleep_on(&busy_wait);
	access_count[target]++;
	return 0;
}

/*
 * Releasing a block device means we sync() it, so that it can safely
 * be forgotten about...
 */
static void hd_release(struct inode * inode, struct file * file)
{
        int target;
	sync_dev(inode->i_rdev);

	target =  DEVICE_NR(MINOR(inode->i_rdev));
	access_count[target]--;

}

static void hd_geninit(void);

static struct gendisk hd_gendisk = {
	MAJOR_NR,	/* Major number */	
	"hd",		/* Major name */
	6,		/* Bits to shift to get real from partition */
	1 << 6,		/* Number of partitions per real */
	MAX_HD,		/* maximum number of real */
	hd_geninit,	/* init function */
	hd,		/* hd struct */
	hd_sizes,	/* block sizes */
	0,		/* number */
	(void *) hd_info,	/* internal */
	NULL		/* next */
};
	
static void hd_interrupt(int unused)
{
	void (*handler)(void) = DEVICE_INTR;

	DEVICE_INTR = NULL;
	timer_active &= ~(1<<HD_TIMER);
	if (!handler)
		handler = unexpected_hd_interrupt;
	handler();
	sti();
}

/*
 * This is the harddisk IRQ description. The SA_INTERRUPT in sa_flags
 * means we run the IRQ-handler with interrupts disabled: this is bad for
 * interrupt latency, but anything else has led to problems on some
 * machines...
 *
 * We enable interrupts in some of the routines after making sure it's
 * safe.
 */
static struct sigaction hd_sigaction = {
	hd_interrupt,
	0,
	SA_INTERRUPT,
	NULL
};

static void hd_geninit(void)
{
	int drive, i;
	extern struct drive_info drive_info;
	unsigned char *BIOS = (unsigned char *) &drive_info;
	int cmos_disks;

	if (!NR_HD) {	   
		for (drive=0 ; drive<2 ; drive++) {
			hd_info[drive].cyl = *(unsigned short *) BIOS;
			hd_info[drive].head = *(2+BIOS);
			hd_info[drive].wpcom = *(unsigned short *) (5+BIOS);
			hd_info[drive].ctl = *(8+BIOS);
			hd_info[drive].lzone = *(unsigned short *) (12+BIOS);
			hd_info[drive].sect = *(14+BIOS);
			BIOS += 16;
		}

	/*
		We querry CMOS about hard disks : it could be that 
		we have a SCSI/ESDI/etc controller that is BIOS
		compatable with ST-506, and thus showing up in our
		BIOS table, but not register compatable, and therefore
		not present in CMOS.

		Furthurmore, we will assume that our ST-506 drives
		<if any> are the primary drives in the system, and 
		the ones reflected as drive 1 or 2.

		The first drive is stored in the high nibble of CMOS
		byte 0x12, the second in the low nibble.  This will be
		either a 4 bit drive type or 0xf indicating use byte 0x19 
		for an 8 bit type, drive 1, 0x1a for drive 2 in CMOS.

		Needless to say, a non-zero value means we have 
		an AT controller hard disk for that drive.

		
	*/

		if ((cmos_disks = CMOS_READ(0x12)) & 0xf0)
			if (cmos_disks & 0x0f)
				NR_HD = 2;
			else
				NR_HD = 1;
	}
	i = NR_HD;
	while (i-- > 0) {
		hd[i<<6].nr_sects = 0;
		if (hd_info[i].head > 16) {
			printk("hd.c: ST-506 interface disk with more than 16 heads detected,\n");
			printk("  probably due to non-standard sector translation. Giving up.\n");
			printk("  (disk %d: cyl=%d, sect=%d, head=%d)\n", i,
				hd_info[i].cyl,
				hd_info[i].sect,
				hd_info[i].head);
			if (i+1 == NR_HD)
				NR_HD--;
			continue;
		}
		hd[i<<6].nr_sects = hd_info[i].head*
				hd_info[i].sect*hd_info[i].cyl;
	}
	if (NR_HD) {
		if (irqaction(HD_IRQ,&hd_sigaction)) {
			printk("hd.c: unable to get IRQ%d for the harddisk driver\n",HD_IRQ);
			NR_HD = 0;
		}
	}
	hd_gendisk.nr_real = NR_HD;

	for(i=0;i<(MAX_HD << 6);i++) hd_blocksizes[i] = 1024;
	blksize_size[MAJOR_NR] = hd_blocksizes;
}

static struct file_operations hd_fops = {
	NULL,			/* lseek - default */
	block_read,		/* read - general block-dev read */
	block_write,		/* write - general block-dev write */
	NULL,			/* readdir - bad */
	NULL,			/* select */
	hd_ioctl,		/* ioctl */
	NULL,			/* mmap */
	hd_open,		/* open */
	hd_release,		/* release */
	block_fsync		/* fsync */
};

unsigned long hd_init(unsigned long mem_start, unsigned long mem_end)
{
	if (register_blkdev(MAJOR_NR,"hd",&hd_fops)) {
		printk("Unable to get major %d for harddisk\n",MAJOR_NR);
		return mem_start;
	}
	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;
	read_ahead[MAJOR_NR] = 8;		/* 8 sector (4kB) read-ahead */
	hd_gendisk.next = gendisk_head;
	gendisk_head = &hd_gendisk;
	timer_table[HD_TIMER].fn = hd_times_out;
	return mem_start;
}

#define DEVICE_BUSY busy[target]
#define USAGE access_count[target]
#define CAPACITY (hd_info[target].head*hd_info[target].sect*hd_info[target].cyl)
/* We assume that the the bios parameters do not change, so the disk capacity
   will not change */
#undef MAYBE_REINIT
#define GENDISK_STRUCT hd_gendisk

/*
 * This routine is called to flush all partitions and partition tables
 * for a changed scsi disk, and then re-read the new partition table.
 * If we are revalidating a disk because of a media change, then we
 * enter with usage == 0.  If we are using an ioctl, we automatically have
 * usage == 1 (we need an open channel to use an ioctl :-), so this
 * is our limit.
 */
static int revalidate_hddisk(int dev, int maxusage)
{
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
	wake_up(&busy_wait);
	return 0;
}

