/*
	linux/kernel/blk_drv/mcd.c - Mitsumi CDROM driver

	Copyright (C) 1992  Martin Harriss

	martin@bdsi.com

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2, or (at your option)
	any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

	HISTORY

	0.1	First attempt - internal use only
	0.2	Cleaned up delays and use of timer - alpha release
	0.3	Audio support added
	0.3.1 Changes for mitsumi CRMC LU005S march version
		   (stud11@cc4.kuleuven.ac.be)
        0.3.2 bug fixes to the ioclts and merged with ALPHA0.99-pl12
		   (Jon Tombs <jon@robots.ox.ac.uk>)
        0.3.3 Added more #defines and mcd_setup()
   		   (Jon Tombs <jon@gtex02.us.es>)
*/


#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/cdrom.h>
#include <linux/ioport.h>

/* #define REALLY_SLOW_IO  */
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#define MAJOR_NR MITSUMI_CDROM_MAJOR
#include "blk.h"
#include <linux/mcd.h>

#if 0
static int mcd_sizes[] = { 0 };
#endif

static int mcdPresent = 0;

static char mcd_buf[2048];	/* buffer for block size conversion */
static int   mcd_bn   = -1;
static short mcd_port = MCD_BASE_ADDR;
static int   mcd_irq  = MCD_INTR_NR;

static int McdTimeout, McdTries;
static struct wait_queue *mcd_waitq = NULL;

static struct mcd_DiskInfo DiskInfo;
static struct mcd_Toc Toc[MAX_TRACKS];
static struct mcd_Play_msf mcd_Play;

static int audioStatus;
static char mcdDiskChanged;
static char tocUpToDate;
static char mcdVersion;

static void mcd_transfer(void);
static void mcd_start(void);
static void mcd_status(void);
static void mcd_read_cmd(void);
static void mcd_data(void);
static void do_mcd_request(void);
static void hsg2msf(long hsg, struct msf *msf);
static void bin2bcd(unsigned char *p);
static int bcd2bin(unsigned char bcd);
static int mcdStatus(void);
static void sendMcdCmd(int cmd, struct mcd_Play_msf *params);
static int getMcdStatus(int timeout);
static int GetQChannelInfo(struct mcd_Toc *qp);
static int updateToc(void);
static int GetDiskInfo(void);
static int GetToc(void);
static int getValue(unsigned char *result);


void mcd_setup(char *str, int *ints)
{
   if (ints[0] > 0)
      mcd_port = ints[1];
   if (ints[0] > 1)      
      mcd_irq  = ints[2];
}

 
int
check_mcd_media_change(int full_dev, int flag)
{
   int retval, target;


#if 1	 /* the below is not reliable */
   return 0;
#endif  
   target = MINOR(full_dev);

   if (target > 0) {
      printk("mcd: Mitsumi CD-ROM request error: invalid device.\n");
      return 0;
   }

   retval = mcdDiskChanged;
   if (!flag)
   {
      mcdDiskChanged = 0;
   }

   return retval;
}


/*
 * Do a 'get status' command and get the result.  Only use from the top half
 * because it calls 'getMcdStatus' which sleeps.
 */

static int
statusCmd(void)
{
	int st, retry;

	for (retry = 0; retry < MCD_RETRY_ATTEMPTS; retry++)
	{

		outb(MCMD_GET_STATUS, MCDPORT(0));	/* send get-status cmd */
		st = getMcdStatus(MCD_STATUS_DELAY);
		if (st != -1)
			break;
	}

	return st;
}


/*
 * Send a 'Play' command and get the status.  Use only from the top half.
 */

static int
mcdPlay(struct mcd_Play_msf *arg)
{
	int retry, st;

	for (retry = 0; retry < MCD_RETRY_ATTEMPTS; retry++)
	{
		sendMcdCmd(MCMD_PLAY_READ, arg);
		st = getMcdStatus(2 * MCD_STATUS_DELAY);
		if (st != -1)
			break;
	}

	return st;
}


long
msf2hsg(struct msf *mp)
{
	return bcd2bin(mp -> frame)
		+ bcd2bin(mp -> sec) * 75
		+ bcd2bin(mp -> min) * 4500
		- 150;
}


static int
mcd_ioctl(struct inode *ip, struct file *fp, unsigned int cmd,
						unsigned long arg)
{
	int i, st;
	struct mcd_Toc qInfo;
	struct cdrom_ti ti;
	struct cdrom_tochdr tocHdr;
	struct cdrom_msf msf;
	struct cdrom_tocentry entry;
	struct mcd_Toc *tocPtr;
	struct cdrom_subchnl subchnl;
#if 0
	struct cdrom_volctrl volctrl;
#endif

	if (!ip)
		return -EINVAL;

	st = statusCmd();
	if (st < 0)
		return -EIO;

	if (!tocUpToDate)
	{
		i = updateToc();
		if (i < 0)
			return i;	/* error reading TOC */
	}

	switch (cmd)
	{
	case CDROMSTART:     /* Spin up the drive */
		/* Don't think we can do this.  Even if we could,
 		 * I think the drive times out and stops after a while
		 * anyway.  For now, ignore it.
		 */

		return 0;

	case CDROMSTOP:      /* Spin down the drive */
		outb(MCMD_STOP, MCDPORT(0));
		i = getMcdStatus(MCD_STATUS_DELAY);

		/* should we do anything if it fails? */

		audioStatus = CDROM_AUDIO_NO_STATUS;
		return 0;

	case CDROMPAUSE:     /* Pause the drive */
		if (audioStatus != CDROM_AUDIO_PLAY)
			return -EINVAL;

		outb(MCMD_STOP, MCDPORT(0));
		i = getMcdStatus(MCD_STATUS_DELAY);

		if (GetQChannelInfo(&qInfo) < 0)
		{
			/* didn't get q channel info */

			audioStatus = CDROM_AUDIO_NO_STATUS;
			return 0;
		}

		mcd_Play.start = qInfo.diskTime;	/* remember restart point */

		audioStatus = CDROM_AUDIO_PAUSED;
		return 0;

	case CDROMRESUME:    /* Play it again, Sam */
		if (audioStatus != CDROM_AUDIO_PAUSED)
			return -EINVAL;

		/* restart the drive at the saved position. */

		i = mcdPlay(&mcd_Play);
		if (i < 0)
		{
			audioStatus = CDROM_AUDIO_ERROR;
			return -EIO;
		}

		audioStatus = CDROM_AUDIO_PLAY;
		return 0;

	case CDROMPLAYTRKIND:     /* Play a track.  This currently ignores index. */

		st = verify_area(VERIFY_READ, (void *) arg, sizeof ti);
		if (st)
			return st;

		memcpy_fromfs(&ti, (void *) arg, sizeof ti);

		if (ti.cdti_trk0 < DiskInfo.first
			|| ti.cdti_trk0 > DiskInfo.last
			|| ti.cdti_trk1 < ti.cdti_trk0)
		{
			return -EINVAL;
		}

		if (ti.cdti_trk1 > DiskInfo.last)
			ti. cdti_trk1 = DiskInfo.last;

		mcd_Play.start = Toc[ti.cdti_trk0].diskTime;
		mcd_Play.end = Toc[ti.cdti_trk1 + 1].diskTime;

#ifdef MCD_DEBUG
printk("play: %02x:%02x.%02x to %02x:%02x.%02x\n",
	mcd_Play.start.min, mcd_Play.start.sec, mcd_Play.start.frame,
	mcd_Play.end.min, mcd_Play.end.sec, mcd_Play.end.frame);
#endif

		i = mcdPlay(&mcd_Play);
		if (i < 0)
		{
			audioStatus = CDROM_AUDIO_ERROR;
			return -EIO;
		}

		audioStatus = CDROM_AUDIO_PLAY;
		return 0;

	case CDROMPLAYMSF:   /* Play starting at the given MSF address. */

		if (audioStatus == CDROM_AUDIO_PLAY) {
		  outb(MCMD_STOP, MCDPORT(0));
		  i = getMcdStatus(MCD_STATUS_DELAY);
		  audioStatus = CDROM_AUDIO_NO_STATUS;
		}

		st = verify_area(VERIFY_READ, (void *) arg, sizeof msf);
		if (st)
			return st;

		memcpy_fromfs(&msf, (void *) arg, sizeof msf);

		/* convert to bcd */

		bin2bcd(&msf.cdmsf_min0);
		bin2bcd(&msf.cdmsf_sec0);
		bin2bcd(&msf.cdmsf_frame0);
		bin2bcd(&msf.cdmsf_min1);
		bin2bcd(&msf.cdmsf_sec1);
		bin2bcd(&msf.cdmsf_frame1);

		mcd_Play.start.min = msf.cdmsf_min0;
		mcd_Play.start.sec = msf.cdmsf_sec0;
		mcd_Play.start.frame = msf.cdmsf_frame0;
		mcd_Play.end.min = msf.cdmsf_min1;
		mcd_Play.end.sec = msf.cdmsf_sec1;
		mcd_Play.end.frame = msf.cdmsf_frame1;

#ifdef MCD_DEBUG
printk("play: %02x:%02x.%02x to %02x:%02x.%02x\n",
mcd_Play.start.min, mcd_Play.start.sec, mcd_Play.start.frame,
mcd_Play.end.min, mcd_Play.end.sec, mcd_Play.end.frame);
#endif

		i = mcdPlay(&mcd_Play);
		if (i < 0)
		{
			audioStatus = CDROM_AUDIO_ERROR;
			return -EIO;
		}

		audioStatus = CDROM_AUDIO_PLAY;
		return 0;

	case CDROMREADTOCHDR:        /* Read the table of contents header */
		st = verify_area(VERIFY_WRITE, (void *) arg, sizeof tocHdr);
		if (st)
			return st;

		tocHdr.cdth_trk0 = DiskInfo.first;
		tocHdr.cdth_trk1 = DiskInfo.last;
		memcpy_tofs((void *) arg, &tocHdr, sizeof tocHdr);
		return 0;

	case CDROMREADTOCENTRY:      /* Read an entry in the table of contents */

		st = verify_area(VERIFY_WRITE, (void *) arg, sizeof entry);
		if (st)
			return st;

		memcpy_fromfs(&entry, (void *) arg, sizeof entry);
		if (entry.cdte_track == CDROM_LEADOUT)
			/* XXX */
			tocPtr = &Toc[DiskInfo.last + 1];

		else if (entry.cdte_track > DiskInfo.last
				|| entry.cdte_track < DiskInfo.first)
			return -EINVAL;

		else
			tocPtr = &Toc[entry.cdte_track];

		entry.cdte_adr = tocPtr -> ctrl_addr;
		entry.cdte_ctrl = tocPtr -> ctrl_addr >> 4;

		if (entry.cdte_format == CDROM_LBA)
			entry.cdte_addr.lba = msf2hsg(&tocPtr -> diskTime);

		else if (entry.cdte_format == CDROM_MSF)
		{
			entry.cdte_addr.msf.minute = bcd2bin(tocPtr -> diskTime.min);
			entry.cdte_addr.msf.second = bcd2bin(tocPtr -> diskTime.sec);
			entry.cdte_addr.msf.frame = bcd2bin(tocPtr -> diskTime.frame);
		}

		else
			return -EINVAL;

		memcpy_tofs((void *) arg, &entry, sizeof entry);
		return 0;

	case CDROMSUBCHNL:   /* Get subchannel info */

		st = verify_area(VERIFY_WRITE, (void *) arg, sizeof subchnl);
		if (st)
			return st;

		memcpy_fromfs(&subchnl, (void *) arg, sizeof subchnl);

		if (GetQChannelInfo(&qInfo) < 0)
			return -EIO;

		subchnl.cdsc_audiostatus = audioStatus;
		subchnl.cdsc_adr = qInfo.ctrl_addr;
		subchnl.cdsc_ctrl = qInfo.ctrl_addr >> 4;
		subchnl.cdsc_trk = bcd2bin(qInfo.track);
		subchnl.cdsc_ind = bcd2bin(qInfo.pointIndex);

		if (subchnl.cdsc_format == CDROM_LBA)
		{
			subchnl.cdsc_absaddr.lba = msf2hsg(&qInfo.diskTime);
			subchnl.cdsc_reladdr.lba = msf2hsg(&qInfo.trackTime);
		}

		else if (subchnl.cdsc_format == CDROM_MSF)
		{
			subchnl.cdsc_absaddr.msf.minute = bcd2bin(qInfo.diskTime.min);
			subchnl.cdsc_absaddr.msf.second = bcd2bin(qInfo.diskTime.sec);
			subchnl.cdsc_absaddr.msf.frame = bcd2bin(qInfo.diskTime.frame);

			subchnl.cdsc_reladdr.msf.minute = bcd2bin(qInfo.trackTime.min);
			subchnl.cdsc_reladdr.msf.second = bcd2bin(qInfo.trackTime.sec);
			subchnl.cdsc_reladdr.msf.frame = bcd2bin(qInfo.trackTime.frame);
		}

		else
			return -EINVAL;

		memcpy_tofs((void *) arg, &subchnl, sizeof subchnl);
		return 0;

	case CDROMVOLCTRL:   /* Volume control */
	/*
	 * This is not working yet.  Setting the volume by itself does
	 * nothing.  Following the 'set' by a 'play' results in zero
	 * volume.  Something to work on for the next release.
	 */
#if 0
		st = verify_area(VERIFY_READ, (void *) arg, sizeof(volctrl));
		if (st)
			return st;

		memcpy_fromfs(&volctrl, (char *) arg, sizeof(volctrl));
printk("VOL %d %d\n", volctrl.channel0 & 0xFF, volctrl.channel1 & 0xFF);
		outb(MCMD_SET_VOLUME, MCDPORT(0));
		outb(volctrl.channel0, MCDPORT(0));
		outb(0, MCDPORT(0));
		outb(volctrl.channel1, MCDPORT(0));
		outb(1, MCDPORT(0));

		i = getMcdStatus(MCD_STATUS_DELAY);
		if (i < 0)
			return -EIO;

		{
			int a, b, c, d;

			getValue(&a);
			getValue(&b);
			getValue(&c);
			getValue(&d);
			printk("%02X %02X %02X %02X\n", a, b, c, d);
		}

		outb(0xF8, MCDPORT(0));
		i = getMcdStatus(MCD_STATUS_DELAY);
		printk("F8 -> %02X\n", i & 0xFF);
#endif
		return 0;

	case CDROMEJECT:     /* Eject the drive - N/A */
		return 0;

	default:
		return -EINVAL;
	}
}


/*
 * Take care of the different block sizes between cdrom and Linux.
 * When Linux gets variable block sizes this will probably go away.
 */

static void
mcd_transfer(void)
{
	long offs;

	while (CURRENT -> nr_sectors > 0 && mcd_bn == CURRENT -> sector / 4)
	{
		offs = (CURRENT -> sector & 3) * 512;
		memcpy(CURRENT -> buffer, mcd_buf + offs, 512);
		CURRENT -> nr_sectors--;
		CURRENT -> sector++;
		CURRENT -> buffer += 512;
	}
}


/*
 * We only seem to get interrupts after an error.
 * Just take the interrupt and clear out the status reg.
 */

static void
mcd_interrupt(int unused)
{
	int st;

	st = inb(MCDPORT(1)) & 0xFF;
	if (st != 0xFF)
	{
		st = inb(MCDPORT(0)) & 0xFF;
#if 0
		printk("<int-%02X>", st);
#endif
	}
}


/*
 * I/O request routine called from Linux kernel.
 */

static void
do_mcd_request(void)
{
	unsigned int block,dev;
	unsigned int nsect;

repeat:
	if (!(CURRENT) || CURRENT->dev < 0) return;
	INIT_REQUEST;
	dev = MINOR(CURRENT->dev);
	block = CURRENT->sector;
	nsect = CURRENT->nr_sectors;

	if (CURRENT == NULL || CURRENT -> sector == -1)
		return;

	if (CURRENT -> cmd != READ)
	{
		printk("mcd: bad cmd %d\n", CURRENT -> cmd);
		end_request(0);
		goto repeat;
	}

	mcd_transfer();

	/* if we satisfied the request from the buffer, we're done. */

	if (CURRENT -> nr_sectors == 0)
	{
		end_request(1);
		goto repeat;
	}

	McdTries = MCD_RETRY_ATTEMPTS;
	mcd_start();
}


/*
 * Start the I/O for the cdrom. Handle retry count.
 */

static void
mcd_start()
{
	if (McdTries == 0)
	{
		printk("mcd: read failed after %d tries\n", MCD_RETRY_ATTEMPTS);
		end_request(0);
		SET_TIMER(do_mcd_request, 1);	/* wait a bit, try again */
		return;
	}

	McdTries--;
	outb(0x40, MCDPORT(0));		/* get status */
	McdTimeout = MCD_STATUS_DELAY;
	SET_TIMER(mcd_status, 1);
}


/*
 * Called from the timer to check the results of the get-status cmd.
 * On success, send the set-mode command.
 */

static void
mcd_status()
{
	int st;

	McdTimeout--;
	st = mcdStatus();
	if (st == -1)
	{
		if (McdTimeout == 0)
		{
			printk("mcd: status timed out\n");
			SET_TIMER(mcd_start, 1);	/* wait a bit, try again */
			return;
		}

		SET_TIMER(mcd_status, 1);
		return;
	}

	if (st & MST_DSK_CHG)
	{
		mcdDiskChanged = 1;
	}
	
	if ((st & MST_READY) == 0)
	{
		printk("mcd: disk removed\n");
		mcdDiskChanged = 1;		
		end_request(0);
		do_mcd_request();
		return;
	}

	outb(0x50, MCDPORT(0));	/* set mode */
	outb(0x01, MCDPORT(0));	/* mode = cooked data */
	McdTimeout = 100;
	SET_TIMER(mcd_read_cmd, 1);
}


/*
 * Check the result of the set-mode command.  On success, send the
 * read-data command.
 */

static void
mcd_read_cmd()
{
	int st;
	long block;
	struct mcd_Play_msf mcdcmd;

	McdTimeout--;
	st = mcdStatus();

	if (st & MST_DSK_CHG)
	{
		mcdDiskChanged = 1;
	}
	
	if (st == -1)
	{
		if (McdTimeout == 0)
		{
			printk("mcd: set mode timed out\n");
			SET_TIMER(mcd_start, 1);	/* wait a bit, try again */
			return;
		}

		SET_TIMER(mcd_read_cmd, 1);
		return;
	}

	mcd_bn = -1;			/* purge our buffer */
	block = CURRENT -> sector / 4;
	hsg2msf(block, &mcdcmd.start);	/* cvt to msf format */

	mcdcmd.end.min = 0;
	mcdcmd.end.sec = 0;
	mcdcmd.end.frame = 1;

	sendMcdCmd(MCMD_PLAY_READ, &mcdcmd);	/* read command */
	McdTimeout = 200;
	SET_TIMER(mcd_data, 1);
}


/*
 * Check the completion of the read-data command.  On success, read
 * the 2048 bytes of data from the disk into our buffer.
 */

static void
mcd_data()
{
	int i;

	McdTimeout--;
	cli();
	i =inb(MCDPORT(1)) & (MFL_STATUS | MFL_DATA);
	if (i == MFL_DATA)
	{
		printk("mcd: read failed\n");
#ifdef MCD_DEBUG
		printk("got 0xB %02X\n", inb(MCDPORT(0)) & 0xFF);
#endif
		SET_TIMER(mcd_start, 1);
		sti();
		return;
	}
	
	if (i == (MFL_STATUS | MFL_DATA))
	{
		if (McdTimeout == 0)
		{
			printk("mcd: data timeout, retrying\n");
			SET_TIMER(mcd_start, 1);
		}
		
		else
			SET_TIMER(mcd_data, 1);
		
		sti();
		return;
	}

	CLEAR_TIMER;
	READ_DATA(MCDPORT(0), &mcd_buf[0], 2048);
	sti();

	mcd_bn = CURRENT -> sector / 4;
	mcd_transfer();
	end_request(1);
	SET_TIMER(do_mcd_request, 1);
}


/*
 * Open the device special file.  Check that a disk is in.
 */

int
mcd_open(struct inode *ip, struct file *fp)
{
	int st;

	if (mcdPresent == 0)
		return -ENXIO;			/* no hardware */

	st = statusCmd();			/* check drive status */
	if (st == -1)
		return -EIO;			/* drive doesn't respond */

	if ((st & MST_READY) == 0)		/* no disk in drive */
	{
		printk("mcd: no disk in drive\n");
		return -EIO;
	}

	if (updateToc() < 0)
		return -EIO;

	return 0;
}


/*
 * On close, we flush all mcd blocks from the buffer cache.
 */

static void
mcd_release(struct inode * inode, struct file * file)
{
	mcd_bn = -1;
	sync_dev(inode->i_rdev);
	invalidate_buffers(inode -> i_rdev);
}


static struct file_operations mcd_fops = {
	NULL,			/* lseek - default */
	block_read,		/* read - general block-dev read */
	block_write,		/* write - general block-dev write */
	NULL,			/* readdir - bad */
	NULL,			/* select */
	mcd_ioctl,		/* ioctl */
	NULL,			/* mmap */
	mcd_open,		/* open */
	mcd_release		/* release */
};


/*
 * MCD interrupt descriptor
 */

static struct sigaction mcd_sigaction = {
	mcd_interrupt,
	0,
	SA_INTERRUPT,
	NULL
};


/*
 * Test for presence of drive and initialize it.  Called at boot time.
 */

unsigned long
mcd_init(unsigned long mem_start, unsigned long mem_end)
{
	int count;
	unsigned char result[3];

	if (register_blkdev(MAJOR_NR, "mcd", &mcd_fops) != 0)
	{
		printk("mcd: Unable to get major %d for Mitsumi CD-ROM\n",
		       MAJOR_NR);
		return mem_start;
	}

        if (check_region(mcd_port, 4)) {
	  printk("mcd: Init failed, I/O port (%X) already in use\n",
		 mcd_port);
	  return mem_start;
	}
	  
	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;
	read_ahead[MAJOR_NR] = 4;

	/* check for card */

	outb(0, MCDPORT(1));			/* send reset */
	for (count = 0; count < 1000000; count++)
		(void) inb(MCDPORT(1));		/* delay a bit */

	outb(0x40, MCDPORT(0));			/* send get-stat cmd */
	for (count = 0; count < 1000000; count++)
		if (!(inb(MCDPORT(1)) & MFL_STATUS))
			break;

	if (count >= 1000000) {
		printk("mcd: Init failed. No mcd device at 0x%x irq %d\n",
		     mcd_port, mcd_irq);
		return mem_start;
	}
	count = inb(MCDPORT(0));		/* pick up the status */
	
	outb(MCMD_GET_VERSION,MCDPORT(0));
	for(count=0;count<3;count++)
		if(getValue(result+count)) {
			printk("mcd: mitsumi get version failed at 0x%d\n",
			       mcd_port);
			return mem_start;
		}	

	if (result[0] == result[1] && result[1] == result[2])
		return mem_start;

	printk("mcd: Mitsumi version : %02X %c %x\n",
	       result[0],result[1],result[2]);


	mcdVersion=result[2];

	if (mcdVersion >=4)
		outb(4,MCDPORT(2)); 	/* magic happens */

	/* don't get the IRQ until we know for sure the drive is there */

	if (irqaction(MCD_INTR_NR,  &mcd_sigaction))
	{
		printk("mcd: Unable to get IRQ%d for Mitsumi CD-ROM\n", MCD_INTR_NR);
		return mem_start;
	}
	snarf_region(mcd_port, 4);
	mcdPresent = 1;
	printk("mcd: Mitsumi CD-ROM Drive present at addr %x, irq %d\n",
	       mcd_port, mcd_irq);
	return mem_start;
}


static void
hsg2msf(long hsg, struct msf *msf)
{
	hsg += 150;
	msf -> min = hsg / 4500;
	hsg %= 4500;
	msf -> sec = hsg / 75;
	msf -> frame = hsg % 75;

	bin2bcd(&msf -> min);		/* convert to BCD */
	bin2bcd(&msf -> sec);
	bin2bcd(&msf -> frame);
}


static void
bin2bcd(unsigned char *p)
{
	int u, t;

	u = *p % 10;
	t = *p / 10;
	*p = u | (t << 4);
}

static int
bcd2bin(unsigned char bcd)
{
	return (bcd >> 4) * 10 + (bcd & 0xF);
}


/*
 * See if a status is ready from the drive and return it
 * if it is ready.
 */

static int
mcdStatus(void)
{
	int i;
	int st;

	st = inb(MCDPORT(1)) & MFL_STATUS;
	if (!st)
	{
		i = inb(MCDPORT(0)) & 0xFF;
		return i;
	}
	else
		return -1;
}


/*
 * Send a play or read command to the drive
 */

static void
sendMcdCmd(int cmd, struct mcd_Play_msf *params)
{
	outb(cmd, MCDPORT(0));
	outb(params -> start.min, MCDPORT(0));
	outb(params -> start.sec, MCDPORT(0));
	outb(params -> start.frame, MCDPORT(0));
	outb(params -> end.min, MCDPORT(0));
	outb(params -> end.sec, MCDPORT(0));
	outb(params -> end.frame, MCDPORT(0));
}


/*
 * Timer interrupt routine to test for status ready from the drive.
 * (see the next routine)
 */

static void
mcdStatTimer(void)
{
	if (!(inb(MCDPORT(1)) & MFL_STATUS))
	{
		wake_up(&mcd_waitq);
		return;
	}

	McdTimeout--;
	if (McdTimeout <= 0)
	{
		wake_up(&mcd_waitq);
		return;
	}

	SET_TIMER(mcdStatTimer, 1);
}


/*
 * Wait for a status to be returned from the drive.  The actual test
 * (see routine above) is done by the timer interrupt to avoid
 * excessive rescheduling.
 */

static int
getMcdStatus(int timeout)
{
	int st;

	McdTimeout = timeout;
	SET_TIMER(mcdStatTimer, 1);
	sleep_on(&mcd_waitq);
	if (McdTimeout <= 0)
		return -1;

	st = inb(MCDPORT(0)) & 0xFF;
	if (st == 0xFF)
		return -1;

	if ((st & MST_BUSY) == 0 && audioStatus == CDROM_AUDIO_PLAY)
		/* XXX might be an error? look at q-channel? */
		audioStatus = CDROM_AUDIO_COMPLETED;

	if (st & MST_DSK_CHG)
	{
		mcdDiskChanged = 1;
		tocUpToDate = 0;
		audioStatus = CDROM_AUDIO_NO_STATUS;
	}

	return st;
}


/*
 * Read a value from the drive.  Should return quickly, so a busy wait
 * is used to avoid excessive rescheduling.
 */

static int
getValue(unsigned char *result)
{
	int count;
	int s;

	for (count = 0; count < 2000; count++)
		if (!(inb(MCDPORT(1)) & MFL_STATUS))
			break;

	if (count >= 2000)
	{
		printk("mcd: getValue timeout\n");
		return -1;
	}

	s = inb(MCDPORT(0)) & 0xFF;
	*result = (unsigned char) s;
	return 0;
}


/*
 * Read the current Q-channel info.  Also used for reading the
 * table of contents.
 */

int
GetQChannelInfo(struct mcd_Toc *qp)
{
	unsigned char notUsed;
	int retry;

	for (retry = 0; retry < MCD_RETRY_ATTEMPTS; retry++)
	{
		outb(MCMD_GET_Q_CHANNEL, MCDPORT(0));
		if (getMcdStatus(MCD_STATUS_DELAY) != -1)
			break;
	}

	if (retry >= MCD_RETRY_ATTEMPTS)
		return -1;

	if (getValue(&qp -> ctrl_addr) < 0) return -1;
	if (getValue(&qp -> track) < 0) return -1;
	if (getValue(&qp -> pointIndex) < 0) return -1;
	if (getValue(&qp -> trackTime.min) < 0) return -1;
	if (getValue(&qp -> trackTime.sec) < 0) return -1;
	if (getValue(&qp -> trackTime.frame) < 0) return -1;
	if (getValue(&notUsed) < 0) return -1;
	if (getValue(&qp -> diskTime.min) < 0) return -1;
	if (getValue(&qp -> diskTime.sec) < 0) return -1;
	if (getValue(&qp -> diskTime.frame) < 0) return -1;

	return 0;
}


/*
 * Read the table of contents (TOC) and TOC header if neccessary
 */

static int
updateToc()
{
	if (tocUpToDate)
		return 0;

	if (GetDiskInfo() < 0)
		return -EIO;

	if (GetToc() < 0)
		return -EIO;

	tocUpToDate = 1;
	return 0;
}


/*
 * Read the table of contents header
 */

static int
GetDiskInfo()
{
	int retry;

	for (retry = 0; retry < MCD_RETRY_ATTEMPTS; retry++)
	{
		outb(MCMD_GET_DISK_INFO, MCDPORT(0));
		if (getMcdStatus(MCD_STATUS_DELAY) != -1)
			break;
	}

	if (retry >= MCD_RETRY_ATTEMPTS)
		return -1;

	if (getValue(&DiskInfo.first) < 0) return -1;
	if (getValue(&DiskInfo.last) < 0) return -1;

	DiskInfo.first = bcd2bin(DiskInfo.first);
	DiskInfo.last = bcd2bin(DiskInfo.last);

	if (getValue(&DiskInfo.diskLength.min) < 0) return -1;
	if (getValue(&DiskInfo.diskLength.sec) < 0) return -1;
	if (getValue(&DiskInfo.diskLength.frame) < 0) return -1;
	if (getValue(&DiskInfo.firstTrack.min) < 0) return -1;
	if (getValue(&DiskInfo.firstTrack.sec) < 0) return -1;
	if (getValue(&DiskInfo.firstTrack.frame) < 0) return -1;

#ifdef MCD_DEBUG
printk("Disk Info: first %d last %d length %02x:%02x.%02x first %02x:%02x.%02x\n",
	DiskInfo.first,
	DiskInfo.last,
	DiskInfo.diskLength.min,
	DiskInfo.diskLength.sec,
	DiskInfo.diskLength.frame,
	DiskInfo.firstTrack.min,
	DiskInfo.firstTrack.sec,
	DiskInfo.firstTrack.frame);
#endif

	return 0;
}


/*
 * Read the table of contents (TOC)
 */

static int
GetToc()
{
	int i, px;
	int limit;
	int retry;
	struct mcd_Toc qInfo;

	for (i = 0; i < MAX_TRACKS; i++)
		Toc[i].pointIndex = 0;

	i = DiskInfo.last + 3;

	for (retry = 0; retry < MCD_RETRY_ATTEMPTS; retry++)
	{
		outb(MCMD_STOP, MCDPORT(0));
		if (getMcdStatus(MCD_STATUS_DELAY) != -1)
			break;
	}

	if (retry >= MCD_RETRY_ATTEMPTS)
		return -1;

	for (retry = 0; retry < MCD_RETRY_ATTEMPTS; retry++)
	{
		outb(MCMD_SET_MODE, MCDPORT(0));
		outb(0x05, MCDPORT(0));			/* mode: toc */
		if (getMcdStatus(MCD_STATUS_DELAY) != -1)
			break;
	}

	if (retry >= MCD_RETRY_ATTEMPTS)
		return -1;

	for (limit = 300; limit > 0; limit--)
	{
		if (GetQChannelInfo(&qInfo) < 0)
			break;

		px = bcd2bin(qInfo.pointIndex);
		if (px > 0 && px < MAX_TRACKS && qInfo.track == 0)
			if (Toc[px].pointIndex == 0)
			{
				Toc[px] = qInfo;
				i--;
			}

		if (i <= 0)
			break;
	}

	Toc[DiskInfo.last + 1].diskTime = DiskInfo.diskLength;

	for (retry = 0; retry < MCD_RETRY_ATTEMPTS; retry++)
	{
                outb(MCMD_SET_MODE, MCDPORT(0));
                outb(0x01, MCDPORT(0));
                if (getMcdStatus(MCD_STATUS_DELAY) != -1)
                        break;
	}

#ifdef MCD_DEBUG
for (i = 1; i <= DiskInfo.last; i++)
printk("i = %2d ctl-adr = %02X track %2d px %02X %02X:%02X.%02X    %02X:%02X.%02X\n",
i, Toc[i].ctrl_addr, Toc[i].track, Toc[i].pointIndex,
Toc[i].trackTime.min, Toc[i].trackTime.sec, Toc[i].trackTime.frame,
Toc[i].diskTime.min, Toc[i].diskTime.sec, Toc[i].diskTime.frame);
for (i = 100; i < 103; i++)
printk("i = %2d ctl-adr = %02X track %2d px %02X %02X:%02X.%02X    %02X:%02X.%02X\n",
i, Toc[i].ctrl_addr, Toc[i].track, Toc[i].pointIndex,
Toc[i].trackTime.min, Toc[i].trackTime.sec, Toc[i].trackTime.frame,
Toc[i].diskTime.min, Toc[i].diskTime.sec, Toc[i].diskTime.frame);
#endif

	return limit > 0 ? 0 : -1;
}

