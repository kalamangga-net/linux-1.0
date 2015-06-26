/*
 *  linux/fs/sysv/symlink.c
 *
 *  minix/symlink.c
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  coh/symlink.c
 *  Copyright (C) 1993  Pascal Haible, Bruno Haible
 *
 *  sysv/symlink.c
 *  Copyright (C) 1993  Bruno Haible
 *
 *  SystemV/Coherent symlink handling code
 */

#include <asm/segment.h>

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/sysv_fs.h>
#include <linux/stat.h>

static int sysv_readlink(struct inode *, char *, int);
static int sysv_follow_link(struct inode *, struct inode *, int, int, struct inode **);

/*
 * symlinks can't do much...
 */
struct inode_operations sysv_symlink_inode_operations = {
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
	sysv_readlink,		/* readlink */
	sysv_follow_link,	/* follow_link */
	NULL,			/* bmap */
	NULL,			/* truncate */
	NULL			/* permission */
};

static int sysv_follow_link(struct inode * dir, struct inode * inode,
	int flag, int mode, struct inode ** res_inode)
{
	int error;
	struct buffer_head * bh;
	char * bh_data;

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
	if (current->link_count > 5) {
		iput(inode);
		iput(dir);
		return -ELOOP;
	}
	if (!(bh = sysv_file_bread(inode, 0, 0, &bh_data))) { /* is reading 1 block enough ?? */
		iput(inode);
		iput(dir);
		return -EIO;
	}
	iput(inode);
	current->link_count++;
	error = open_namei(bh_data,flag,mode,res_inode,dir);
	current->link_count--;
	brelse(bh);
	return error;
}

static int sysv_readlink(struct inode * inode, char * buffer, int buflen)
{
	struct buffer_head * bh;
	char * bh_data;
	int i;
	char c;

	if (!S_ISLNK(inode->i_mode)) {
		iput(inode);
		return -EINVAL;
	}
	if (buflen > inode->i_sb->sv_block_size_1)
		buflen = inode->i_sb->sv_block_size_1;
	bh = sysv_file_bread(inode, 0, 0, &bh_data);
	iput(inode);
	if (!bh)
		return 0;
	i = 0;
	while (i<buflen && (c = bh_data[i])) {
		i++;
		put_fs_byte(c,buffer++);
	}
	brelse(bh);
	return i;
}
