/*
 *  linux/kernel/blk_drv/ramdisk.c
 *
 *  Written by Theodore Ts'o, 12/2/91
 *
 * Modifications by Fred N. van Kempen to allow for bootable root
 * disks (which are used in LINUX/Pro).  Also some cleanups.  03/03/93
 */


#include <linux/config.h>
#include <linux/sched.h>
#include <linux/minix_fs.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <asm/system.h>
#include <asm/segment.h>

#define MAJOR_NR  MEM_MAJOR
#include "blk.h"

#define RAMDISK_MINOR	1


char	*rd_start;
int	rd_length = 0;
static int rd_blocksizes[2] = {0, 0};

static void do_rd_request(void)
{
	int	len;
	char	*addr;

repeat:
	INIT_REQUEST;
	addr = rd_start + (CURRENT->sector << 9);
	len = CURRENT->current_nr_sectors << 9;

	if ((MINOR(CURRENT->dev) != RAMDISK_MINOR) ||
	    (addr+len > rd_start+rd_length)) {
		end_request(0);
		goto repeat;
	}
	if (CURRENT-> cmd == WRITE) {
		(void ) memcpy(addr,
			      CURRENT->buffer,
			      len);
	} else if (CURRENT->cmd == READ) {
		(void) memcpy(CURRENT->buffer, 
			      addr,
			      len);
	} else
		panic("RAMDISK: unknown RAM disk command !\n");
	end_request(1);
	goto repeat;
}

static struct file_operations rd_fops = {
	NULL,			/* lseek - default */
	block_read,		/* read - general block-dev read */
	block_write,		/* write - general block-dev write */
	NULL,			/* readdir - bad */
	NULL,			/* select */
	NULL,			/* ioctl */
	NULL,			/* mmap */
	NULL,			/* no special open code */
	NULL,			/* no special release code */
	block_fsync		/* fsync */
};

/*
 * Returns amount of memory which needs to be reserved.
 */
long rd_init(long mem_start, int length)
{
	int	i;
	char	*cp;

	if (register_blkdev(MEM_MAJOR,"rd",&rd_fops)) {
		printk("RAMDISK: Unable to get major %d.\n", MEM_MAJOR);
		return 0;
	}
	blk_dev[MEM_MAJOR].request_fn = DEVICE_REQUEST;
	rd_start = (char *) mem_start;
	rd_length = length;
	cp = rd_start;
	for (i=0; i < length; i++)
		*cp++ = '\0';

	for(i=0;i<2;i++) rd_blocksizes[i] = 1024;
	blksize_size[MAJOR_NR] = rd_blocksizes;

	return(length);
}

/*
 * If the root device is the RAM disk, try to load it.
 * In order to do this, the root device is originally set to the
 * floppy, and we later change it to be RAM disk.
 */
void rd_load(void)
{
	struct buffer_head *bh;
	struct minix_super_block s;
	int		block, tries;
	int		i = 1;
	int		nblocks;
	char		*cp;

	/* If no RAM disk specified, give up early. */
	if (!rd_length) return;
	printk("RAMDISK: %d bytes, starting at 0x%x\n",
					rd_length, (int) rd_start);

	/* If we are doing a diskette boot, we might have to pre-load it. */
	if (MAJOR(ROOT_DEV) != FLOPPY_MAJOR) return;

	/*
	 * Check for a super block on the diskette.
	 * The old-style boot/root diskettes had their RAM image
	 * starting at block 512 of the boot diskette.  LINUX/Pro
	 * uses the enire diskette as a file system, so in that
	 * case, we have to look at block 0.  Be intelligent about
	 * this, and check both... - FvK
	 */
	for (tries = 0; tries < 1000; tries += 512) {
		block = tries;
		bh = breada(ROOT_DEV,block+1,block,block+2,-1);
		if (!bh) {
			printk("RAMDISK: I/O error while looking for super block!\n");
			return;
		}

		/* This is silly- why do we require it to be a MINIX FS? */
		*((struct minix_super_block *) &s) =
			*((struct minix_super_block *) bh->b_data);
		brelse(bh);
		nblocks = s.s_nzones << s.s_log_zone_size;
		if (s.s_magic != MINIX_SUPER_MAGIC &&
		    s.s_magic != MINIX_SUPER_MAGIC2) {
			printk("RAMDISK: trying old-style RAM image.\n");
			continue;
		}

		if (nblocks > (rd_length >> BLOCK_SIZE_BITS)) {
			printk("RAMDISK: image too big! (%d/%d blocks)\n",
					nblocks, rd_length >> BLOCK_SIZE_BITS);
			return;
		}
		printk("RAMDISK: Loading %d blocks into RAM disk", nblocks);

		/* We found an image file system.  Load it into core! */
		cp = rd_start;
		while (nblocks) {
			if (nblocks > 2) 
				bh = breada(ROOT_DEV, block, block+1, block+2, -1);
			else
				bh = bread(ROOT_DEV, block, BLOCK_SIZE);
			if (!bh) {
				printk("RAMDISK: I/O error on block %d, aborting!\n", 
				block);
				return;
			}
			(void) memcpy(cp, bh->b_data, BLOCK_SIZE);
			brelse(bh);
			if (!(nblocks-- & 15)) printk(".");
			cp += BLOCK_SIZE;
			block++;
			i++;
		}
		printk("\ndone\n");

		/* We loaded the file system image.  Prepare for mounting it. */
		ROOT_DEV = ((MEM_MAJOR << 8) | RAMDISK_MINOR);
		return;
	}
}
