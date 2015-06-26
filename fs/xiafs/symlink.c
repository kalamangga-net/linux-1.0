/*
 *  linux/fs/xiafs/symlink.c
 *
 *  Copyright (C) Q. Frank Xia, 1993.
 *  
 *  Based on Linus' minix/symlink.c
 *  Copyright (C) Linus Torvalds, 1991, 1992.
 *
 *  This software may be redistributed per Linux Copyright.
 */

#include <asm/segment.h>

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/xia_fs.h>
#include <linux/stat.h>

static int 
xiafs_readlink(struct inode *, char *, int);

static int 
xiafs_follow_link(struct inode *, struct inode *, int, int, struct inode **);

/*
 * symlinks can't do much...
 */
struct inode_operations xiafs_symlink_inode_operations = {
	NULL,			/* no file-operations */
	NULL,			/* create */
	NULL,			/* lookup */
	NULL,			/* link */
	NULL,			/* unlink */
	NULL,			/* symlink */
	NULL,			/* mkdir */
	NULL,			/* rmdir */
	NULL,			/* mknod */
	NULL,			/* rename */
	xiafs_readlink,		/* readlink */
	xiafs_follow_link,	/* follow_link */
	NULL,			/* bmap */
	NULL,			/* truncate */
	NULL			/* permission */
};

static int xiafs_readlink(struct inode * inode, char * buffer, int buflen)
{
    struct buffer_head * bh;
    int i;
    char c;

    if (!S_ISLNK(inode->i_mode)) {
        iput(inode);
	return -EINVAL;
    }
    if (buflen > BLOCK_SIZE)
        buflen = BLOCK_SIZE;
    bh = xiafs_bread(inode, 0, 0);
    if (!IS_RDONLY (inode)) {
 	inode->i_atime=CURRENT_TIME;
 	inode->i_dirt=1;
    }
    iput(inode);
    if (!bh)
        return 0;
    for (i=0; i < buflen && (c=bh->b_data[i]); i++)
      put_fs_byte(c, buffer++);
    if (i < buflen-1)
      put_fs_byte((char)0, buffer);
    brelse(bh);
    return i;
}

static int xiafs_follow_link(struct inode * dir, struct inode * inode,
	int flag, int mode, struct inode ** res_inode)
{
    int error;
    struct buffer_head * bh;

    *res_inode = NULL;
    if (!dir) {
        dir = current->root;
	dir->i_count++;
    }
    if (!inode) {
        iput(dir);
	return -ENOENT;
    }
    if (!S_ISLNK(inode->i_mode)) {
        iput(dir);
	*res_inode = inode;
	return 0;
    }
    if (!IS_RDONLY (inode)) {
	inode->i_atime=CURRENT_TIME;
	inode->i_dirt=1;
    }
    if (current->link_count > 5) {
        iput(inode);
	iput(dir);
	return -ELOOP;
    }
    if (!(bh = xiafs_bread(inode, 0, 0))) {
        iput(inode);
        iput(dir);
	return -EIO;
    }
    iput(inode);
    current->link_count++;
    error = open_namei(bh->b_data,flag,mode,res_inode,dir);
    current->link_count--;
    brelse(bh);
    return error;
}



