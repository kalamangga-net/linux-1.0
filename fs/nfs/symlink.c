/*
 *  linux/fs/nfs/symlink.c
 *
 *  Copyright (C) 1992  Rick Sladkey
 *
 *  nfs symlink handling code
 */

#include <asm/segment.h>

#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/nfs_fs.h>
#include <linux/stat.h>
#include <linux/mm.h>
#include <linux/malloc.h>

static int nfs_readlink(struct inode *, char *, int);
static int nfs_follow_link(struct inode *, struct inode *, int, int,
			   struct inode **);

/*
 * symlinks can't do much...
 */
struct inode_operations nfs_symlink_inode_operations = {
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
	nfs_readlink,		/* readlink */
	nfs_follow_link,	/* follow_link */
	NULL,			/* bmap */
	NULL,			/* truncate */
	NULL			/* permission */
};

static int nfs_follow_link(struct inode *dir, struct inode *inode,
			   int flag, int mode, struct inode **res_inode)
{
	int error;
	char *res;

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
	res = (char *) kmalloc(NFS_MAXPATHLEN + 1, GFP_KERNEL);
	error = nfs_proc_readlink(NFS_SERVER(inode), NFS_FH(inode), res);
	if (error) {
		iput(inode);
		iput(dir);
		kfree_s(res, NFS_MAXPATHLEN + 1);
		return error;
	}
	iput(inode);
	current->link_count++;
	error = open_namei(res, flag, mode, res_inode, dir);
	current->link_count--;
	kfree_s(res, NFS_MAXPATHLEN + 1);
	return error;
}

static int nfs_readlink(struct inode *inode, char *buffer, int buflen)
{
	int i;
	char c;
	int error;
	char *res;

	if (!S_ISLNK(inode->i_mode)) {
		iput(inode);
		return -EINVAL;
	}
	if (buflen > NFS_MAXPATHLEN)
		buflen = NFS_MAXPATHLEN;
	res = (char *) kmalloc(buflen + 1, GFP_KERNEL);
	error = nfs_proc_readlink(NFS_SERVER(inode), NFS_FH(inode), res);
	iput(inode);
	if (error) {
		kfree_s(res, buflen + 1);
		return error;
	}
	for (i = 0; i < buflen && (c = res[i]); i++)
		put_fs_byte(c,buffer++);
	kfree_s(res, buflen + 1);
	return i;
}
