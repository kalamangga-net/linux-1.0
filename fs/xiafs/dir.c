/*
 *  linux/fs/xiafs/dir.c
 *  
 *  Copyright (C) Q. Frank Xia, 1993.
 *  
 *  Based on Linus' minix/dir.c
 *  Copyright (C) Linus Torvalds, 1991, 1992.
 *
 *  This software may be redistributed per Linux Copyright.
 */

#include <asm/segment.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/xia_fs.h>
#include <linux/stat.h>

#include "xiafs_mac.h"

static int xiafs_dir_read(struct inode *, struct file *, char *, int);
static int xiafs_readdir(struct inode *, struct file *, struct dirent *, int);

static struct file_operations xiafs_dir_operations = {
    NULL,		/* lseek - default */
    xiafs_dir_read,	/* read */
    NULL,		/* write - bad */
    xiafs_readdir,	/* readdir */
    NULL,		/* select - default */
    NULL,		/* ioctl - default */
    NULL,		/* mmap */
    NULL,		/* no special open code */
    NULL,		/* no special release code */
    file_fsync		/* default fsync */
};

/*
 * directories can handle most operations...
 */
struct inode_operations xiafs_dir_inode_operations = {
    &xiafs_dir_operations,	/* default directory file-ops */
    xiafs_create,			/* create */
    xiafs_lookup,			/* lookup */
    xiafs_link,			/* link */
    xiafs_unlink,			/* unlink */
    xiafs_symlink,		/* symlink */
    xiafs_mkdir,			/* mkdir */
    xiafs_rmdir,			/* rmdir */
    xiafs_mknod,			/* mknod */
    xiafs_rename,			/* rename */
    NULL,			/* readlink */
    NULL,			/* follow_link */
    NULL,			/* bmap */
    xiafs_truncate,		/* truncate */
    NULL			/* permission */
};

static int xiafs_dir_read(struct inode * inode, 
			struct file * filp, char * buf, int count)
{
  return -EISDIR;
}

static int xiafs_readdir(struct inode * inode, 
		       struct file * filp, struct dirent * dirent, int count)
{
    u_int offset, i;
    struct buffer_head * bh;
    struct xiafs_direct * de;

    if (!inode || !inode->i_sb || !S_ISDIR(inode->i_mode))
        return -EBADF;
    if (inode->i_size & (XIAFS_ZSIZE(inode->i_sb) - 1) )
        return -EBADF;
    while (filp->f_pos < inode->i_size) {
        offset = filp->f_pos & (XIAFS_ZSIZE(inode->i_sb) - 1);
	bh = xiafs_bread(inode, filp->f_pos >> XIAFS_ZSIZE_BITS(inode->i_sb),0);
	if (!bh) {
	    filp->f_pos += XIAFS_ZSIZE(inode->i_sb)-offset;
	    continue;
	}
	de = (struct xiafs_direct *) (offset + bh->b_data);
	while (offset < XIAFS_ZSIZE(inode->i_sb) && filp->f_pos < inode->i_size) {
	    if (de->d_ino > inode->i_sb->u.xiafs_sb.s_ninodes ||
		de->d_rec_len < 12 || 
		(char *)de+de->d_rec_len > XIAFS_ZSIZE(inode->i_sb)+bh->b_data ||
		de->d_name_len < 1 || de->d_name_len + 8 > de->d_rec_len ||
		de->d_name_len > _XIAFS_NAME_LEN ||
		de->d_name[de->d_name_len] ) {
	        printk("XIA-FS: bad directory entry (%s %d)\n", WHERE_ERR);
		brelse(bh);
		return 0;
	    }  
	    offset += de->d_rec_len;
	    filp->f_pos += de->d_rec_len;
	    if (de->d_ino) {
	        for (i = 0; i < de->d_name_len ; i++)
		    put_fs_byte(de->d_name[i],i+dirent->d_name);
		put_fs_byte(0,i+dirent->d_name);
		put_fs_long(de->d_ino,&dirent->d_ino);
		put_fs_word(i,&dirent->d_reclen);
		brelse(bh);
		if (!IS_RDONLY (inode)) {
		    inode->i_atime=CURRENT_TIME;		    
		    inode->i_dirt=1;
		}
		return i;
	    }
	    de = (struct xiafs_direct *) (offset + bh->b_data);
	}
	brelse(bh);
	if (offset > XIAFS_ZSIZE(inode->i_sb)) {
	    printk("XIA-FS: bad directory (%s %d)\n", WHERE_ERR);
	    return 0;
	}
    }
    if (!IS_RDONLY (inode)) {
	inode->i_atime=CURRENT_TIME;		    
	inode->i_dirt=1;
    }
    return 0;
}
