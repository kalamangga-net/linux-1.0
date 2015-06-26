/*
 *  linux/fs/sysv/file.c
 *
 *  minix/file.c
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  coh/file.c
 *  Copyright (C) 1993  Pascal Haible, Bruno Haible
 *
 *  sysv/file.c
 *  Copyright (C) 1993  Bruno Haible
 *
 *  SystemV/Coherent regular file handling primitives
 */

#include <asm/segment.h>

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/sysv_fs.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/locks.h>

#define	NBUF	32

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

#include <linux/fs.h>
#include <linux/sysv_fs.h>

static int sysv_file_read(struct inode *, struct file *, char *, int);
static int sysv_file_write(struct inode *, struct file *, char *, int);

/*
 * We have mostly NULL's here: the current defaults are ok for
 * the coh filesystem.
 */
static struct file_operations sysv_file_operations = {
	NULL,			/* lseek - default */
	sysv_file_read,		/* read */
	sysv_file_write,	/* write */
	NULL,			/* readdir - bad */
	NULL,			/* select - default */
	NULL,			/* ioctl - default */
	NULL,			/* mmap */
	NULL,			/* no special open is needed */
	NULL,			/* release */
	sysv_sync_file		/* fsync */
};

static struct file_operations sysv_file_operations_with_bmap = {
	NULL,			/* lseek - default */
	sysv_file_read,		/* read */
	sysv_file_write,	/* write */
	NULL,			/* readdir - bad */
	NULL,			/* select - default */
	NULL,			/* ioctl - default */
	generic_mmap,		/* mmap */
	NULL,			/* no special open is needed */
	NULL,			/* release */
	sysv_sync_file		/* fsync */
};

struct inode_operations sysv_file_inode_operations = {
	&sysv_file_operations,	/* default file operations */
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
	NULL,			/* bmap */
	sysv_truncate,		/* truncate */
	NULL			/* permission */
};

struct inode_operations sysv_file_inode_operations_with_bmap = {
	&sysv_file_operations_with_bmap, /* default file operations */
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
	sysv_bmap,		/* bmap */
	sysv_truncate,		/* truncate */
	NULL			/* permission */
};

struct sysv_buffer {
	struct buffer_head * bh;
	char * bh_data;
};

static int sysv_file_read(struct inode * inode, struct file * filp, char * buf, int count)
{
	struct super_block * sb = inode->i_sb;
	int read,left,chars;
	unsigned int block;
	int blocks, offset;
	int bhrequest, bhreqi, uptodate;
	struct sysv_buffer * bhb, * bhe;
	struct buffer_head * bhreq[NBUF];
	struct sysv_buffer buflist[NBUF];
	unsigned int size;

	if (!inode) {
		printk("sysv_file_read: inode = NULL\n");
		return -EINVAL;
	}
	if (!S_ISREG(inode->i_mode)) {
		printk("sysv_file_read: mode = %07o\n",inode->i_mode);
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
	block = offset >> sb->sv_block_size_bits;
	offset &= sb->sv_block_size_1;
	size = (size + sb->sv_block_size_1) >> sb->sv_block_size_bits;
	blocks = (left + offset + sb->sv_block_size_1) >> sb->sv_block_size_bits;
	bhb = bhe = buflist;
	if (filp->f_reada) {
		blocks += read_ahead[MAJOR(inode->i_dev)] >> (sb->sv_block_size_bits - 9);
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
	   buffers and caches.

	   We must remove duplicates from the bhreq array as ll_rw_block
	   doesn't like duplicate requests (it hangs in wait_on_buffer...).
	 */

	do {
		bhrequest = 0;
		uptodate = 1;
		while (blocks) {
			--blocks;
			bhb->bh = sysv_getblk(inode, block++, 0, &bhb->bh_data);
			if (bhb->bh && !bhb->bh->b_uptodate) {
				uptodate = 0;
				if (sb->sv_block_size_ratio_bits > 0) /* block_size < BLOCK_SIZE ? */
					for (bhreqi = 0; bhreqi < bhrequest; bhreqi++)
						if (bhreq[bhreqi] == bhb->bh)
							goto notreq;
				bhreq[bhrequest++] = bhb->bh;
				notreq: ;
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
			if (bhe->bh) {
				wait_on_buffer(bhe->bh);
				if (!bhe->bh->b_uptodate) {	/* read error? */
					brelse(bhe->bh);
					if (++bhe == &buflist[NBUF])
						bhe = buflist;
					left = 0;
					break;
				}
			}
			if (left < sb->sv_block_size - offset)
				chars = left;
			else
				chars = sb->sv_block_size - offset;
			filp->f_pos += chars;
			left -= chars;
			read += chars;
			if (bhe->bh) {
				memcpy_tofs(buf,offset+bhe->bh_data,chars);
				brelse(bhe->bh);
				buf += chars;
			} else {
				while (chars-- > 0)
					put_fs_byte(0,buf++);
			}
			offset = 0;
			if (++bhe == &buflist[NBUF])
				bhe = buflist;
		} while (left > 0 && bhe != bhb && (!bhe->bh || !bhe->bh->b_lock));
	} while (left > 0);

/* Release the read-ahead blocks */
	while (bhe != bhb) {
		brelse(bhe->bh);
		if (++bhe == &buflist[NBUF])
			bhe = buflist;
	};
	if (!read)
		return -EIO;
	filp->f_reada = 1;
	if (!IS_RDONLY(inode))
		inode->i_atime = CURRENT_TIME;
	return read;
}

static int sysv_file_write(struct inode * inode, struct file * filp, char * buf, int count)
{
	struct super_block * sb = inode->i_sb;
	off_t pos;
	int written,c;
	struct buffer_head * bh;
	char * bh_data;
	char * p;

	if (!inode) {
		printk("sysv_file_write: inode = NULL\n");
		return -EINVAL;
	}
	if (!S_ISREG(inode->i_mode)) {
		printk("sysv_file_write: mode = %07o\n",inode->i_mode);
		return -EINVAL;
	}
/*
 * ok, append may not work when many processes are writing at the same time
 * but so what. That way leads to madness anyway.
 * But we need to protect against simultaneous truncate as we may end up
 * writing our data into blocks that have meanwhile been incorporated into
 * the freelist, thereby trashing the freelist.
 */
	if (sb->sv_block_size_ratio_bits > 0) /* block_size < BLOCK_SIZE ? */
		coh_lock_inode(inode);
	if (filp->f_flags & O_APPEND)
		pos = inode->i_size;
	else
		pos = filp->f_pos;
	written = 0;
	while (written<count) {
		bh = sysv_getblk (inode, pos >> sb->sv_block_size_bits, 1, &bh_data);
		if (!bh) {
			if (!written)
				written = -ENOSPC;
			break;
		}
		c = sb->sv_block_size - (pos & sb->sv_block_size_1);
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
		/* now either c==BLOCK_SIZE or bh->b_uptodate */
		p = (pos & sb->sv_block_size_1) + bh_data;
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
	if (sb->sv_block_size_ratio_bits > 0) /* block_size < BLOCK_SIZE ? */
		coh_unlock_inode(inode);
	return written;
}
