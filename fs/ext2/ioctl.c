/*
 * linux/fs/ext2/ioctl.c
 *
 * Copyright (C) 1993, 1994  Remy Card (card@masi.ibp.fr)
 *                           Laboratoire MASI - Institut Blaise Pascal
 *                           Universite Pierre et Marie Curie (Paris VI)
 */

#include <asm/segment.h>

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/ext2_fs.h>
#include <linux/ioctl.h>
#include <linux/sched.h>

int ext2_ioctl (struct inode * inode, struct file * filp, unsigned int cmd,
		unsigned long arg)
{

	ext2_debug ("cmd = %u, arg = %lu\n", cmd, arg);

	switch (cmd) {
	case EXT2_IOC_GETFLAGS:
		put_fs_long (inode->u.ext2_i.i_flags, (long *) arg);
		return 0;
	case EXT2_IOC_SETFLAGS:
		if ((current->euid != inode->i_uid) && !suser())
			return -EPERM;
		if (IS_RDONLY(inode))
			return -EROFS;
		inode->u.ext2_i.i_flags = get_fs_long ((long *) arg);
		inode->i_ctime = CURRENT_TIME;
		inode->i_dirt = 1;
		return 0;
	case EXT2_IOC_GETVERSION:
		put_fs_long (inode->u.ext2_i.i_version, (long *) arg);
		return 0;
	case EXT2_IOC_SETVERSION:
		if ((current->euid != inode->i_uid) && !suser())
			return -EPERM;
		if (IS_RDONLY(inode))
			return -EROFS;
		inode->u.ext2_i.i_version = get_fs_long ((long *) arg);
		inode->i_ctime = CURRENT_TIME;
		inode->i_dirt = 1;
		return 0;
	default:
		return -EINVAL;
	}
}
