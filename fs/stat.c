/*
 *  linux/fs/stat.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/errno.h>
#include <linux/stat.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

static void cp_old_stat(struct inode * inode, struct old_stat * statbuf)
{
	struct old_stat tmp;

	printk("VFS: Warning: %s using old stat() call. Recompile your binary.\n",
		current->comm);
	tmp.st_dev = inode->i_dev;
	tmp.st_ino = inode->i_ino;
	tmp.st_mode = inode->i_mode;
	tmp.st_nlink = inode->i_nlink;
	tmp.st_uid = inode->i_uid;
	tmp.st_gid = inode->i_gid;
	tmp.st_rdev = inode->i_rdev;
	tmp.st_size = inode->i_size;
	tmp.st_atime = inode->i_atime;
	tmp.st_mtime = inode->i_mtime;
	tmp.st_ctime = inode->i_ctime;
	memcpy_tofs(statbuf,&tmp,sizeof(tmp));
}

static void cp_new_stat(struct inode * inode, struct new_stat * statbuf)
{
	struct new_stat tmp = {0, };
	unsigned int blocks, indirect;

	tmp.st_dev = inode->i_dev;
	tmp.st_ino = inode->i_ino;
	tmp.st_mode = inode->i_mode;
	tmp.st_nlink = inode->i_nlink;
	tmp.st_uid = inode->i_uid;
	tmp.st_gid = inode->i_gid;
	tmp.st_rdev = inode->i_rdev;
	tmp.st_size = inode->i_size;
	tmp.st_atime = inode->i_atime;
	tmp.st_mtime = inode->i_mtime;
	tmp.st_ctime = inode->i_ctime;
/*
 * st_blocks and st_blksize are approximated with a simple algorithm if
 * they aren't supported directly by the filesystem. The minix and msdos
 * filesystems don't keep track of blocks, so they would either have to
 * be counted explicitly (by delving into the file itself), or by using
 * this simple algorithm to get a reasonable (although not 100% accurate)
 * value.
 */

/*
 * Use minix fs values for the number of direct and indirect blocks.  The
 * count is now exact for the minix fs except that it counts zero blocks.
 * Everything is in BLOCK_SIZE'd units until the assignment to
 * tmp.st_blksize.
 */
#define D_B   7
#define I_B   (BLOCK_SIZE / sizeof(unsigned short))

	if (!inode->i_blksize) {
		blocks = (tmp.st_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
		if (blocks > D_B) {
			indirect = (blocks - D_B + I_B - 1) / I_B;
			blocks += indirect;
			if (indirect > 1) {
				indirect = (indirect - 1 + I_B - 1) / I_B;
				blocks += indirect;
				if (indirect > 1)
					blocks++;
			}
		}
		tmp.st_blocks = (BLOCK_SIZE / 512) * blocks;
		tmp.st_blksize = BLOCK_SIZE;
	} else {
		tmp.st_blocks = inode->i_blocks;
		tmp.st_blksize = inode->i_blksize;
	}
	memcpy_tofs(statbuf,&tmp,sizeof(tmp));
}

asmlinkage int sys_stat(char * filename, struct old_stat * statbuf)
{
	struct inode * inode;
	int error;

	error = verify_area(VERIFY_WRITE,statbuf,sizeof (*statbuf));
	if (error)
		return error;
	error = namei(filename,&inode);
	if (error)
		return error;
	cp_old_stat(inode,statbuf);
	iput(inode);
	return 0;
}

asmlinkage int sys_newstat(char * filename, struct new_stat * statbuf)
{
	struct inode * inode;
	int error;

	error = verify_area(VERIFY_WRITE,statbuf,sizeof (*statbuf));
	if (error)
		return error;
	error = namei(filename,&inode);
	if (error)
		return error;
	cp_new_stat(inode,statbuf);
	iput(inode);
	return 0;
}

asmlinkage int sys_lstat(char * filename, struct old_stat * statbuf)
{
	struct inode * inode;
	int error;

	error = verify_area(VERIFY_WRITE,statbuf,sizeof (*statbuf));
	if (error)
		return error;
	error = lnamei(filename,&inode);
	if (error)
		return error;
	cp_old_stat(inode,statbuf);
	iput(inode);
	return 0;
}

asmlinkage int sys_newlstat(char * filename, struct new_stat * statbuf)
{
	struct inode * inode;
	int error;

	error = verify_area(VERIFY_WRITE,statbuf,sizeof (*statbuf));
	if (error)
		return error;
	error = lnamei(filename,&inode);
	if (error)
		return error;
	cp_new_stat(inode,statbuf);
	iput(inode);
	return 0;
}

asmlinkage int sys_fstat(unsigned int fd, struct old_stat * statbuf)
{
	struct file * f;
	struct inode * inode;
	int error;

	error = verify_area(VERIFY_WRITE,statbuf,sizeof (*statbuf));
	if (error)
		return error;
	if (fd >= NR_OPEN || !(f=current->filp[fd]) || !(inode=f->f_inode))
		return -EBADF;
	cp_old_stat(inode,statbuf);
	return 0;
}

asmlinkage int sys_newfstat(unsigned int fd, struct new_stat * statbuf)
{
	struct file * f;
	struct inode * inode;
	int error;

	error = verify_area(VERIFY_WRITE,statbuf,sizeof (*statbuf));
	if (error)
		return error;
	if (fd >= NR_OPEN || !(f=current->filp[fd]) || !(inode=f->f_inode))
		return -EBADF;
	cp_new_stat(inode,statbuf);
	return 0;
}

asmlinkage int sys_readlink(const char * path, char * buf, int bufsiz)
{
	struct inode * inode;
	int error;

	if (bufsiz <= 0)
		return -EINVAL;
	error = verify_area(VERIFY_WRITE,buf,bufsiz);
	if (error)
		return error;
	error = lnamei(path,&inode);
	if (error)
		return error;
	if (!inode->i_op || !inode->i_op->readlink) {
		iput(inode);
		return -EINVAL;
	}
	return inode->i_op->readlink(inode,buf,bufsiz);
}
