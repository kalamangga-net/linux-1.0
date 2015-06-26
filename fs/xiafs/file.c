/*
 *  linux/fs/xiafs/file.c
 *
 *  Copyright (C) Q. Frank Xia, 1993.
 *  
 *  Based on Linus' minix/file.c
 *  Copyright (C) Linus Torvalds, 1991, 1992.
 *
 *  This software may be redistributed per Linux Copyright.
 */

#include <asm/segment.h>
#include <asm/system.h>

#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/xia_fs.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/stat.h>
#include <linux/locks.h>

#include "xiafs_mac.h"

#define	NBUF	32

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

static int xiafs_file_read(struct inode *, struct file *, char *, int);
static int xiafs_file_write(struct inode *, struct file *, char *, int);

/*
 * We have mostly NULL's here: the current defaults are ok for
 * the xiafs filesystem.
 */
static struct file_operations xiafs_file_operations = {
    NULL,			/* lseek - default */
    xiafs_file_read,		/* read */
    xiafs_file_write,		/* write */
    NULL,			/* readdir - bad */
    NULL,			/* select - default */
    NULL,			/* ioctl - default */
    generic_mmap,      		/* mmap */
    NULL,			/* no special open is needed */
    NULL,			/* release */
    xiafs_sync_file		/* fsync */
};

struct inode_operations xiafs_file_inode_operations = {
    &xiafs_file_operations,	/* default file operations */
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
    xiafs_bmap,			/* bmap */
    xiafs_truncate,		/* truncate */
    NULL			/* permission */
};

static int 
xiafs_file_read(struct inode * inode, struct file * filp, char * buf, int count)
{
    int read, left, chars;
    int zone_nr, zones, f_zones, offset;
    int bhrequest, uptodate;
    struct buffer_head ** bhb, ** bhe;
    struct buffer_head * bhreq[NBUF];
    struct buffer_head * buflist[NBUF];

    if (!inode) {
        printk("XIA-FS: inode = NULL (%s %d)\n", WHERE_ERR);
	return -EINVAL;
    }
    if (!S_ISREG(inode->i_mode)) {
        printk("XIA-FS: mode != regular (%s %d)\n", WHERE_ERR);
	return -EINVAL;
    }
    offset = filp->f_pos;
    left = inode->i_size - offset;
    if (left > count)
	left = count;
    if (left <= 0)
	return 0;
    read = 0;
    zone_nr = offset >> XIAFS_ZSIZE_BITS(inode->i_sb);
    offset &= XIAFS_ZSIZE(inode->i_sb) -1 ;
    f_zones =(inode->i_size+XIAFS_ZSIZE(inode->i_sb)-1)>>XIAFS_ZSIZE_BITS(inode->i_sb);
    zones = (left+offset+XIAFS_ZSIZE(inode->i_sb)-1) >> XIAFS_ZSIZE_BITS(inode->i_sb);
    bhb = bhe = buflist;
    if (filp->f_reada) {
        zones += read_ahead[MAJOR(inode->i_dev)] >> (1+XIAFS_ZSHIFT(inode->i_sb));
	if (zone_nr + zones > f_zones)
	    zones = f_zones - zone_nr;
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
	while (zones--) {
	    *bhb = xiafs_getblk(inode, zone_nr++, 0);
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
	    if (left < XIAFS_ZSIZE(inode->i_sb) - offset)
	        chars = left;
	    else
	        chars = XIAFS_ZSIZE(inode->i_sb) - offset;
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
    if (!IS_RDONLY (inode)) {
	inode->i_atime = CURRENT_TIME;
	inode->i_dirt = 1;
    }
    return read;
}

static int 
xiafs_file_write(struct inode * inode, struct file * filp, char * buf, int count)
{
    off_t pos;
    int written, c;
    struct buffer_head * bh;
    char * cp;

    if (!inode) {
        printk("XIA-FS: inode = NULL (%s %d)\n", WHERE_ERR);
	return -EINVAL;
    }
    if (!S_ISREG(inode->i_mode)) {
        printk("XIA-FS: mode != regular (%s %d)\n", WHERE_ERR);
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
        bh = xiafs_getblk(inode, pos >> XIAFS_ZSIZE_BITS(inode->i_sb), 1);
	if (!bh) {
	    if (!written)
	        written = -ENOSPC;
	    break;
	}
	c = XIAFS_ZSIZE(inode->i_sb) - (pos & (XIAFS_ZSIZE(inode->i_sb) - 1));
	if (c > count-written)
	    c = count-written;
	if (c != XIAFS_ZSIZE(inode->i_sb) && !bh->b_uptodate) {
	    ll_rw_block(READ, 1, &bh);
	    wait_on_buffer(bh);
	    if (!bh->b_uptodate) {
	        brelse(bh);
		if (!written)
		    written = -EIO;
		break;
	    }
	}
	cp = (pos & (XIAFS_ZSIZE(inode->i_sb)-1)) + bh->b_data;
	pos += c;
	if (pos > inode->i_size) {
	    inode->i_size = pos;
	    inode->i_dirt = 1;
	}
	written += c;
	memcpy_fromfs(cp,buf,c);
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
