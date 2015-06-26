/*
 *  linux/fs/proc/kmsg.c
 *
 *  Copyright (C) 1992  by Linus Torvalds
 *
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>

#include <asm/segment.h>
#include <asm/io.h>

extern unsigned long log_size;
extern struct wait_queue * log_wait;

asmlinkage int sys_syslog(int type, char * bug, int count);

static int kmsg_open(struct inode * inode, struct file * file)
{
	return sys_syslog(1,NULL,0);
}

static void kmsg_release(struct inode * inode, struct file * file)
{
	(void) sys_syslog(0,NULL,0);
}

static int kmsg_read(struct inode * inode, struct file * file,char * buf, int count)
{
	return sys_syslog(2,buf,count);
}

static int kmsg_select(struct inode *inode, struct file *file, int sel_type, select_table * wait)
{
	if (sel_type != SEL_IN)
		return 0;
	if (log_size)
		return 1;
	select_wait(&log_wait, wait);
	return 0;
}


static struct file_operations proc_kmsg_operations = {
	NULL,		/* kmsg_lseek */
	kmsg_read,
	NULL,		/* kmsg_write */
	NULL,		/* kmsg_readdir */
	kmsg_select,	/* kmsg_select */
	NULL,		/* kmsg_ioctl */
	NULL,		/* mmap */
	kmsg_open,
	kmsg_release,
	NULL		/* can't fsync */
};

struct inode_operations proc_kmsg_inode_operations = {
	&proc_kmsg_operations,	/* default base directory file-ops */
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
	NULL,			/* truncate */
	NULL			/* permission */
};
