/*
 *  linux/fs/ext/file.c
 *
 *  Copyright (C) 1992 Remy Card (card@masi.ibp.fr)
 *
 *  from
 *
 *  linux/fs/minix/file.c
 *
 *  Copyright (C) 1991, 1992 Linus Torvalds
 *
 *  ext regular file handling primitives
 */

#include <asm/segment.h>
#include <asm/system.h>

#include <linux/sched.h>
#include <linux/ext_fs.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/stat.h>
#include <linux/locks.h>

#define	NBUF	32

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

#include <linux/fs.h>
#include <linux/ext_fs.h>

static int ext_file_read(struct inode *, struct file *, char *, int);
static int ext_file_write(struct inode *, struct file *, char *, int);

/*
 * We have mostly NULL's here: the current defaults are ok for
 * the ext filesystem.
 */
static struct file_operations ext_file_operations = {
	NULL,			/* lseek - default */
	ext_file_read,	/* read */
	ext_file_write,	/* write */
	NULL,			/* readdir - bad */
	NULL,			/* select - default */
	NULL,			/* ioctl - default */
	generic_mmap,  		/* mmap */
	NULL,			/* no special open is needed */
	NULL,			/* release */
	ext_sync_file			/* fsync */
};

struct inode_operations ext_file_inode_operations = {
	&ext_file_operations,	/* default file operations */
	NULL,			/* create */
	NULL,			/* lookup */
	NULL,			/* link */
	NULL,			/* unlink */
	NULL,			/* symlink */
	NULL,			/* mkdir */
	NULL,			/* rmdir */
	NULL,			/* mknod */
	NULL,			/* rename */
	NULL,			/* readlink */
	NULL,			/* follow_link */
	ext_bmap,		/* bmap */
	ext_truncate,		/* truncate */
	NULL			/* permission */
};

static int ext_file_read(struct inode * inode, struct file * filp, char * buf, int count)
{
	int read,left,chars;
	int block, blocks, offset;
	int bhrequest, uptodate;
	struct buffer_head ** bhb, ** bhe;
	struct buffer_head * bhreq[NBUF];
	struct buffer_head * buflist[NBUF];
	unsigned int size;

	if (!inode) {
		printk("ext_file_read: inode = NULL\n");
		return -EINVAL;
	}
	if (!S_ISREG(inode->i_mode)) {
		printk("ext_file_read: mode = %07o\n",inode->i_mode);
		return -EINVAL;
	}
	offset = filp->f_pos;
	size = inode->i_size;
	if (offset > size)
		left = 0;
	else
		left = size - offset;
	if (left > count)
		left = count;
	if (left <= 0)
		return 0;
	read = 0;
	block = offset >> BLOCK_SIZE_BITS;
	offset &= BLOCK_SIZE-1;
	size = (size + (BLOCK_SIZE-1)) >> BLOCK_SIZE_BITS;
	blocks = (left + offset + BLOCK_SIZE - 1) >> BLOCK_SIZE_BITS;
	bhb = bhe = buflist;
	if (filp->f_reada) {
		blocks += read_ahead[MAJOR(inode->i_dev)] / (BLOCK_SIZE >> 9);
		if (block + blocks > size)
			blocks = size - block;
	}

	/* We do this in a two stage process.  We first try and request
	   as many blocks as we can, then we wait for the first one to
	   complete, and then we try and wrap up as many as are actually
	   done.  This routine is rather generic, in that it can be used
	   in a filesystem by substituting the appropriate function in
	   for getblk.

	   This routine is optimized to make maximum use of the various
	   buffers and caches. */

	do {
		bhrequest = 0;
		uptodate = 1;
		while (blocks) {
			--blocks;
			*bhb = ext_getblk(inode, block++, 0);
			if (*bhb && !(*bhb)->b_uptodate) {
				uptodate = 0;
				bhreq[bhrequest++] = *bhb;
			}

			if (++bhb == &buflist[NBUF])
				bhb = buflist;

			/* If the block we have on hand is uptodate, go ahead
			   and complete processing. */
			if (uptodate)
				break;
			if (bhb == bhe)
				break;
		}

		/* Now request them all */
		if (bhrequest)
			ll_rw_block(READ, bhrequest, bhreq);

		do { /* Finish off all I/O that has actually completed */
			if (*bhe) {
				wait_on_buffer(*bhe);
				if (!(*bhe)->b_uptodate) {	/* read error? */
				        brelse(*bhe);
					if (++bhe == &buflist[NBUF])
					  bhe = buflist;
					left = 0;
					break;
				}
			}
			if (left < BLOCK_SIZE - offset)
				chars = left;
			else
				chars = BLOCK_SIZE - offset;
			filp->f_pos += chars;
			left -= chars;
			read += chars;
			if (*bhe) {
				memcpy_tofs(buf,offset+(*bhe)->b_data,chars);
				brelse(*bhe);
				buf += chars;
			} else {
				while (chars-->0)
					put_fs_byte(0,buf++);
			}
			offset = 0;
			if (++bhe == &buflist[NBUF])
				bhe = buflist;
		} while (left > 0 && bhe != bhb && (!*bhe || !(*bhe)->b_lock));
	} while (left > 0);

/* Release the read-ahead blocks */
	while (bhe != bhb) {
		brelse(*bhe);
		if (++bhe == &buflist[NBUF])
			bhe = buflist;
	};
	if (!read)
		return -EIO;
	filp->f_reada = 1;
	if (!IS_RDONLY(inode)) {
		inode->i_atime = CURRENT_TIME;
		inode->i_dirt = 1;
	}
	return read;
}

static int ext_file_write(struct inode * inode, struct file * filp, char * buf, int count)
{
	off_t pos;
	int written,c;
	struct buffer_head * bh;
	char * p;

	if (!inode) {
		printk("ext_file_write: inode = NULL\n");
		return -EINVAL;
	}
	if (!S_ISREG(inode->i_mode)) {
		printk("ext_file_write: mode = %07o\n",inode->i_mode);
		return -EINVAL;
	}
/*
 * ok, append may not work when many processes are writing at the same time
 * but so what. That way leads to madness anyway.
 */
	if (filp->f_flags & O_APPEND)
		pos = inode->i_size;
	else
		pos = filp->f_pos;
	written = 0;
	while (written<count) {
		bh = ext_getblk(inode,pos/BLOCK_SIZE,1);
		if (!bh) {
			if (!written)
				written = -ENOSPC;
			break;
		}
		c = BLOCK_SIZE - (pos % BLOCK_SIZE);
		if (c > count-written)
			c = count-written;
		if (c != BLOCK_SIZE && !bh->b_uptodate) {
			ll_rw_block(READ, 1, &bh);
			wait_on_buffer(bh);
			if (!bh->b_uptodate) {
				brelse(bh);
				if (!written)
					written = -EIO;
				break;
			}
		}
		p = (pos % BLOCK_SIZE) + bh->b_data;
		pos += c;
		if (pos > inode->i_size) {
			inode->i_size = pos;
			inode->i_dirt = 1;
		}
		written += c;
		memcpy_fromfs(p,buf,c);
		buf += c;
		bh->b_uptodate = 1;
		bh->b_dirt = 1;
		brelse(bh);
	}
	inode->i_mtime = inode->i_ctime = CURRENT_TIME;
	filp->f_pos = pos;
	inode->i_dirt = 1;
	return written;
}
