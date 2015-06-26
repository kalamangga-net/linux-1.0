/*
 *  linux/fs/ext2/file.c
 *
 *  Copyright (C) 1992, 1993, 1994  Remy Card (card@masi.ibp.fr)
 *                                  Laboratoire MASI - Institut Blaise Pascal
 *                                  Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/file.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext2 fs regular file handling primitives
 */

#include <asm/segment.h>
#include <asm/system.h>

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/ext2_fs.h>
#include <linux/fcntl.h>
#include <linux/sched.h>
#include <linux/stat.h>
#include <linux/locks.h>

#define	NBUF	32

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

#include <linux/fs.h>
#include <linux/ext2_fs.h>

static int ext2_file_read (struct inode *, struct file *, char *, int);
static int ext2_file_write (struct inode *, struct file *, char *, int);
static void ext2_release_file (struct inode *, struct file *);

/*
 * We have mostly NULL's here: the current defaults are ok for
 * the ext2 filesystem.
 */
static struct file_operations ext2_file_operations = {
	NULL,			/* lseek - default */
	ext2_file_read,		/* read */
	ext2_file_write,	/* write */
	NULL,			/* readdir - bad */
	NULL,			/* select - default */
	ext2_ioctl,		/* ioctl */
	generic_mmap,  		/* mmap */
	NULL,			/* no special open is needed */
	ext2_release_file,	/* release */
	ext2_sync_file		/* fsync */
};

struct inode_operations ext2_file_inode_operations = {
	&ext2_file_operations,/* default file operations */
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
	ext2_bmap,		/* bmap */
	ext2_truncate,		/* truncate */
	ext2_permission		/* permission */
};

static int ext2_file_read (struct inode * inode, struct file * filp,
		    char * buf, int count)
{
	int read, left, chars;
	int block, blocks, offset;
	int bhrequest, uptodate;
	struct buffer_head ** bhb, ** bhe;
	struct buffer_head * bhreq[NBUF];
	struct buffer_head * buflist[NBUF];
	struct super_block * sb;
	unsigned int size;
	int err;

	if (!inode) {
		printk ("ext2_file_read: inode = NULL\n");
		return -EINVAL;
	}
	sb = inode->i_sb;
	if (!S_ISREG(inode->i_mode)) {
		ext2_warning (sb, "ext2_file_read", "mode = %07o",
			      inode->i_mode);
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
	block = offset >> EXT2_BLOCK_SIZE_BITS(sb);
	offset &= (sb->s_blocksize - 1);
	size = (size + sb->s_blocksize - 1) >> EXT2_BLOCK_SIZE_BITS(sb);
	blocks = (left + offset + sb->s_blocksize - 1) >> EXT2_BLOCK_SIZE_BITS(sb);
	bhb = bhe = buflist;
	if (filp->f_reada) {
		blocks += read_ahead[MAJOR(inode->i_dev)] >>
			(EXT2_BLOCK_SIZE_BITS(sb) - 9);
		if (block + blocks > size)
			blocks = size - block;
	}

	/*
	 * We do this in a two stage process.  We first try and request
	 * as many blocks as we can, then we wait for the first one to
	 * complete, and then we try and wrap up as many as are actually
	 * done.  This routine is rather generic, in that it can be used
	 * in a filesystem by substituting the appropriate function in
	 * for getblk
	 *
	 * This routine is optimized to make maximum use of the various
	 * buffers and caches.
	 */

	do {
		bhrequest = 0;
		uptodate = 1;
		while (blocks) {
			--blocks;
			*bhb = ext2_getblk (inode, block++, 0, &err);
			if (*bhb && !(*bhb)->b_uptodate) {
				uptodate = 0;
				bhreq[bhrequest++] = *bhb;
			}

			if (++bhb == &buflist[NBUF])
				bhb = buflist;

			/*
			 * If the block we have on hand is uptodate, go ahead
			 * and complete processing
			 */
			if (uptodate)
				break;

			if (bhb == bhe)
				break;
		}

		/*
		 * Now request them all
		 */
		if (bhrequest)
			ll_rw_block (READ, bhrequest, bhreq);

		do {
			/*
			 * Finish off all I/O that has actually completed
			 */
			if (*bhe) {
				wait_on_buffer (*bhe);
				if (!(*bhe)->b_uptodate) { /* read error? */
				        brelse(*bhe);
					if (++bhe == &buflist[NBUF])
					  bhe = buflist;
					left = 0;
					break;
				}
			}
			if (left < sb->s_blocksize - offset)
				chars = left;
			else
				chars = sb->s_blocksize - offset;
			filp->f_pos += chars;
			left -= chars;
			read += chars;
			if (*bhe) {
				memcpy_tofs (buf, offset + (*bhe)->b_data,
					     chars);
				brelse (*bhe);
				buf += chars;
			} else {
				while (chars-- > 0)
					put_fs_byte (0, buf++);
			}
			offset = 0;
			if (++bhe == &buflist[NBUF])
				bhe = buflist;
		} while (left > 0 && bhe != bhb && (!*bhe || !(*bhe)->b_lock));
	} while (left > 0);

	/*
	 * Release the read-ahead blocks
	 */
	while (bhe != bhb) {
		brelse (*bhe);
		if (++bhe == &buflist[NBUF])
			bhe = buflist;
	}
	if (!read)
		return -EIO;
	filp->f_reada = 1;
	if (!IS_RDONLY(inode)) {
		inode->i_atime = CURRENT_TIME;
		inode->i_dirt = 1;
	}
	return read;
}

static int ext2_file_write (struct inode * inode, struct file * filp,
			    char * buf, int count)
{
	off_t pos;
	int written, c;
	struct buffer_head * bh;
	char * p;
	struct super_block * sb;
	int err;

	if (!inode) {
		printk("ext2_file_write: inode = NULL\n");
		return -EINVAL;
	}
	sb = inode->i_sb;
	if (sb->s_flags & MS_RDONLY)
		/*
		 * This fs has been automatically remounted ro because of errors
		 */
		return -ENOSPC;

	if (!S_ISREG(inode->i_mode)) {
		ext2_warning (sb, "ext2_file_write", "mode = %07o\n",
			      inode->i_mode);
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
	while (written < count) {
		bh = ext2_getblk (inode, pos / sb->s_blocksize, 1, &err);
		if (!bh) {
			if (!written)
				written = err;
			break;
		}
		c = sb->s_blocksize - (pos % sb->s_blocksize);
		if (c > count-written)
			c = count - written;
		if (c != sb->s_blocksize && !bh->b_uptodate) {
			ll_rw_block (READ, 1, &bh);
			wait_on_buffer (bh);
			if (!bh->b_uptodate) {
				brelse (bh);
				if (!written)
					written = -EIO;
				break;
			}
		}
		p = (pos % sb->s_blocksize) + bh->b_data;
		pos += c;
		if (pos > inode->i_size) {
			inode->i_size = pos;
			inode->i_dirt = 1;
		}
		written += c;
		memcpy_fromfs (p, buf, c);
		buf += c;
		bh->b_uptodate = 1;
		bh->b_dirt = 1;
		brelse (bh);
	}
	inode->i_ctime = inode->i_mtime = CURRENT_TIME;
	filp->f_pos = pos;
	inode->i_dirt = 1;
	return written;
}

/*
 * Called when a inode is released. Note that this is different
 * from ext2_open: open gets called at every open, but release
 * gets called only when /all/ the files are closed.
 */
static void ext2_release_file (struct inode * inode, struct file * filp)
{
	if (filp->f_mode & 2)
		ext2_discard_prealloc (inode);
}
