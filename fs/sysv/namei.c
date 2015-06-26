/*
 *  linux/fs/sysv/namei.c
 *
 *  minix/namei.c
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  coh/namei.c
 *  Copyright (C) 1993  Pascal Haible, Bruno Haible
 *
 *  sysv/namei.c
 *  Copyright (C) 1993  Bruno Haible
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/sysv_fs.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>

/* compare strings: name[0..len-1] (not zero-terminated) and
 * buffer[0..] (filled with zeroes up to buffer[0..maxlen-1])
 */
static inline int namecompare(int len, int maxlen,
	const char * name, const char * buffer)
{
	if (len >= maxlen || !buffer[len]) {
		unsigned char same;
		__asm__("repe ; cmpsb ; setz %0"
			:"=q" (same)
			:"S" ((long) name),"D" ((long) buffer),"c" (len)
			:"cx","di","si");
		return same;
	}
	/* if (len<maxlen && buffer[len]) then buffer is longer than name */
	return 0;
}

/*
 * ok, we cannot use strncmp, as the name is not in our data space. [Now it is!]
 * Thus we'll have to use sysv_match. No big problem. Match also makes
 * some sanity tests.
 *
 * NOTE! unlike strncmp, sysv_match returns 1 for success, 0 for failure.
 */
static int sysv_match(int len, const char * name, struct sysv_dir_entry * de)
{
	if (!de->inode || len > SYSV_NAMELEN)
		return 0;
	/* "" means "." ---> so paths like "/usr/lib//libc.a" work */
	if (!len && (de->name[0]=='.') && (de->name[1]=='\0'))
		return 1;
	return namecompare(len,SYSV_NAMELEN,name,de->name);
}

/*
 *	sysv_find_entry()
 *
 * finds an entry in the specified directory with the wanted name. It
 * returns the cache buffer in which the entry was found, and the entry
 * itself (as a parameter - res_dir). It does NOT read the inode of the
 * entry - you'll have to do that yourself if you want to.
 */
static struct buffer_head * sysv_find_entry(struct inode * dir,
	const char * name, int namelen, struct sysv_dir_entry ** res_dir)
{
	struct super_block * sb;
	unsigned long pos, block, offset; /* pos = block * block_size + offset */
	struct buffer_head * bh;
	char * bh_data;

	*res_dir = NULL;
	if (!dir)
		return NULL;
	sb = dir->i_sb;
	if (namelen > SYSV_NAMELEN)
		if (sb->sv_truncate)
			namelen = SYSV_NAMELEN;
		else
			return NULL;
	bh = NULL;
	pos = block = offset = 0;
	while (pos < dir->i_size) {
		if (!bh) {
			bh = sysv_file_bread(dir,block,0,&bh_data);
			if (!bh) {
				/* offset = 0; */ block++;
				pos += sb->sv_block_size;
				continue;
			}
		}
		if (sysv_match(namelen, name,
			       *res_dir = (struct sysv_dir_entry *) (bh_data + offset) ))
			return bh;
		pos += SYSV_DIRSIZE;
		offset += SYSV_DIRSIZE;
		if (offset < sb->sv_block_size)
			continue;
		brelse(bh);
		bh = NULL;
		offset = 0; block++;
	}
	brelse(bh);
	*res_dir = NULL;
	return NULL;
}

int sysv_lookup(struct inode * dir,const char * name, int len,
	struct inode ** result)
{
	int ino;
	struct sysv_dir_entry * de;
	struct buffer_head * bh;

	*result = NULL;
	if (!dir)
		return -ENOENT;
	if (!S_ISDIR(dir->i_mode)) {
		iput(dir);
		return -ENOENT;
	}
	if (!(bh = sysv_find_entry(dir,name,len,&de))) {
		iput(dir);
		return -ENOENT;
	}
	ino = de->inode;
	brelse(bh);
	if (!(*result = iget(dir->i_sb,ino))) {
		iput(dir);
		return -EACCES;
	}
	iput(dir);
	return 0;
}

/*
 *	sysv_add_entry()
 *
 * adds a file entry to the specified directory, returning a possible
 * error value if it fails.
 *
 * NOTE!! The inode part of 'de' is left at 0 - which means you
 * may not sleep between calling this and putting something into
 * the entry, as someone else might have used it while you slept.
 */
static int sysv_add_entry(struct inode * dir,
	const char * name, int namelen,
	struct buffer_head ** res_buf,
	struct sysv_dir_entry ** res_dir)
{
	struct super_block * sb;
	int i;
	unsigned long pos, block, offset; /* pos = block * block_size + offset */
	struct buffer_head * bh;
	char * bh_data;
	struct sysv_dir_entry * de;

	*res_buf = NULL;
	*res_dir = NULL;
	if (!dir)
		return -ENOENT;
	sb = dir->i_sb;
	if (namelen > SYSV_NAMELEN)
		if (sb->sv_truncate)
			namelen = SYSV_NAMELEN;
		else
			return -ENAMETOOLONG;
	if (!namelen)
		return -ENOENT;
	bh = NULL;
	pos = block = offset = 0;
	while (1) {
		if (!bh) {
			bh = sysv_file_bread(dir,block,1,&bh_data);
			if (!bh)
				return -ENOSPC;
		}
		de = (struct sysv_dir_entry *) (bh_data + offset);
		pos += SYSV_DIRSIZE;
		offset += SYSV_DIRSIZE;
		if (pos > dir->i_size) {
			de->inode = 0;
			dir->i_size = pos;
			dir->i_dirt = 1;
		}
		if (de->inode) {
			if (namecompare(namelen, SYSV_NAMELEN, name, de->name)) {
				brelse(bh);
				return -EEXIST;
			}
		} else {
			dir->i_mtime = dir->i_ctime = CURRENT_TIME;
			for (i = 0; i < SYSV_NAMELEN ; i++)
				de->name[i] = (i < namelen) ? name[i] : 0;
			bh->b_dirt = 1;
			*res_dir = de;
			break;
		}
		if (offset < sb->sv_block_size)
			continue;
		brelse(bh);
		bh = NULL;
		offset = 0; block++;
	}
	*res_buf = bh;
	return 0;
}

int sysv_create(struct inode * dir,const char * name, int len, int mode,
	struct inode ** result)
{
	int error;
	struct inode * inode;
	struct buffer_head * bh;
	struct sysv_dir_entry * de;

	*result = NULL;
	if (!dir)
		return -ENOENT;
	inode = sysv_new_inode(dir);
	if (!inode) {
		iput(dir);
		return -ENOSPC;
	}
	if (inode->i_sb->sv_block_size_ratio_bits == 0) /* block_size == BLOCK_SIZE ? */
		inode->i_op = &sysv_file_inode_operations_with_bmap;
	else
		inode->i_op = &sysv_file_inode_operations;
	inode->i_mode = mode;
	inode->i_dirt = 1;
	error = sysv_add_entry(dir,name,len, &bh ,&de);
	if (error) {
		inode->i_nlink--;
		inode->i_dirt = 1;
		iput(inode);
		iput(dir);
		return error;
	}
	de->inode = inode->i_ino;
	bh->b_dirt = 1;
	brelse(bh);
	iput(dir);
	*result = inode;
	return 0;
}

int sysv_mknod(struct inode * dir, const char * name, int len, int mode, int rdev)
{
	int error;
	struct inode * inode;
	struct buffer_head * bh;
	struct sysv_dir_entry * de;

	if (!dir)
		return -ENOENT;
	bh = sysv_find_entry(dir,name,len,&de);
	if (bh) {
		brelse(bh);
		iput(dir);
		return -EEXIST;
	}
	inode = sysv_new_inode(dir);
	if (!inode) {
		iput(dir);
		return -ENOSPC;
	}
	inode->i_uid = current->euid;
	inode->i_mode = mode;
	inode->i_op = NULL;
	if (S_ISREG(inode->i_mode))
		if (inode->i_sb->sv_block_size_ratio_bits == 0) /* block_size == BLOCK_SIZE ? */
			inode->i_op = &sysv_file_inode_operations_with_bmap;
		else
			inode->i_op = &sysv_file_inode_operations;
	else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &sysv_dir_inode_operations;
		if (dir->i_mode & S_ISGID)
			inode->i_mode |= S_ISGID;
	}
	else if (S_ISLNK(inode->i_mode))
		inode->i_op = &sysv_symlink_inode_operations;
	else if (S_ISCHR(inode->i_mode))
		inode->i_op = &chrdev_inode_operations;
	else if (S_ISBLK(inode->i_mode))
		inode->i_op = &blkdev_inode_operations;
	else if (S_ISFIFO(inode->i_mode))
		init_fifo(inode);
	if (S_ISBLK(mode) || S_ISCHR(mode))
		inode->i_rdev = rdev;
	inode->i_dirt = 1;
	error = sysv_add_entry(dir, name, len, &bh, &de);
	if (error) {
		inode->i_nlink--;
		inode->i_dirt = 1;
		iput(inode);
		iput(dir);
		return error;
	}
	de->inode = inode->i_ino;
	bh->b_dirt = 1;
	brelse(bh);
	iput(dir);
	iput(inode);
	return 0;
}

int sysv_mkdir(struct inode * dir, const char * name, int len, int mode)
{
	int error;
	struct inode * inode;
	struct buffer_head * bh, *dir_block;
	char * bh_data;
	struct sysv_dir_entry * de;

	if (!dir) {
		iput(dir);
		return -EINVAL;
	}
	bh = sysv_find_entry(dir,name,len,&de);
	if (bh) {
		brelse(bh);
		iput(dir);
		return -EEXIST;
	}
	if (dir->i_nlink >= dir->i_sb->sv_link_max) {
		iput(dir);
		return -EMLINK;
	}
	inode = sysv_new_inode(dir);
	if (!inode) {
		iput(dir);
		return -ENOSPC;
	}
	inode->i_op = &sysv_dir_inode_operations;
	inode->i_size = 2 * SYSV_DIRSIZE;
	dir_block = sysv_file_bread(inode,0,1,&bh_data);
	if (!dir_block) {
		iput(dir);
		inode->i_nlink--;
		inode->i_dirt = 1;
		iput(inode);
		return -ENOSPC;
	}
	de = (struct sysv_dir_entry *) (bh_data + 0*SYSV_DIRSIZE);
	de->inode = inode->i_ino;
	strcpy(de->name,"."); /* rest of de->name is zero, see sysv_new_block */
	de = (struct sysv_dir_entry *) (bh_data + 1*SYSV_DIRSIZE);
	de->inode = dir->i_ino;
	strcpy(de->name,".."); /* rest of de->name is zero, see sysv_new_block */
	inode->i_nlink = 2;
	dir_block->b_dirt = 1;
	brelse(dir_block);
	inode->i_mode = S_IFDIR | (mode & 0777 & ~current->umask);
	if (dir->i_mode & S_ISGID)
		inode->i_mode |= S_ISGID;
	inode->i_dirt = 1;
	error = sysv_add_entry(dir, name, len, &bh, &de);
	if (error) {
		iput(dir);
		inode->i_nlink=0;
		iput(inode);
		return error;
	}
	de->inode = inode->i_ino;
	bh->b_dirt = 1;
	dir->i_nlink++;
	dir->i_dirt = 1;
	iput(dir);
	iput(inode);
	brelse(bh);
	return 0;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
static int empty_dir(struct inode * inode)
{
	struct super_block * sb;
	unsigned long pos, block, offset; /* pos = block * block_size + offset */
	struct buffer_head * bh;
	char * bh_data;
	struct sysv_dir_entry * de;

	if (!inode)
		return 1;
	block = 0;
	bh = NULL;
	pos = offset = 2*SYSV_DIRSIZE;
	if (inode->i_size % SYSV_DIRSIZE)
		goto bad_dir;
	if (inode->i_size < pos)
		goto bad_dir;
	bh = sysv_file_bread(inode,0,0,&bh_data);
	if (!bh)
		goto bad_dir;
	de = (struct sysv_dir_entry *) (bh_data + 0*SYSV_DIRSIZE);
	if (!de->inode || strcmp(de->name,"."))
		goto bad_dir;
	de = (struct sysv_dir_entry *) (bh_data + 1*SYSV_DIRSIZE);
	if (!de->inode || strcmp(de->name,".."))
		goto bad_dir;
	sb = inode->i_sb;
	while (pos < inode->i_size) {
		if (!bh) {
			bh = sysv_file_bread(inode,block,0,&bh_data);
			if (!bh) {
				/* offset = 0; */ block++;
				pos += sb->sv_block_size;
				continue;
			}
		}
		de = (struct sysv_dir_entry *) (bh_data + offset);
		pos += SYSV_DIRSIZE;
		offset += SYSV_DIRSIZE;
		if (de->inode) {
			brelse(bh);
			return 0;
		}
		if (offset < sb->sv_block_size)
			continue;
		brelse(bh);
		bh = NULL;
		offset = 0; block++;
	}
	brelse(bh);
	return 1;
bad_dir:
	brelse(bh);
	printk("Bad directory on device %04x\n",inode->i_dev);
	return 1;
}

int sysv_rmdir(struct inode * dir, const char * name, int len)
{
	int retval;
	struct inode * inode;
	struct buffer_head * bh;
	struct sysv_dir_entry * de;

	inode = NULL;
	bh = sysv_find_entry(dir,name,len,&de);
	retval = -ENOENT;
	if (!bh)
		goto end_rmdir;
	retval = -EPERM;
	if (!(inode = iget(dir->i_sb, de->inode)))
		goto end_rmdir;
	if ((dir->i_mode & S_ISVTX) && current->euid &&
	    inode->i_uid != current->euid)
		goto end_rmdir;
	if (inode->i_dev != dir->i_dev)
		goto end_rmdir;
	if (inode == dir)	/* we may not delete ".", but "../dir" is ok */
		goto end_rmdir;
	if (!S_ISDIR(inode->i_mode)) {
		retval = -ENOTDIR;
		goto end_rmdir;
	}
	if (!empty_dir(inode)) {
		retval = -ENOTEMPTY;
		goto end_rmdir;
	}
	if (de->inode != inode->i_ino) {
		retval = -ENOENT;
		goto end_rmdir;
	}
	if (inode->i_count > 1) {
		retval = -EBUSY;
		goto end_rmdir;
	}
	if (inode->i_nlink != 2)
		printk("empty directory has nlink!=2 (%d)\n",inode->i_nlink);
	de->inode = 0;
	bh->b_dirt = 1;
	inode->i_nlink=0;
	inode->i_dirt=1;
	dir->i_nlink--;
	inode->i_ctime = dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	dir->i_dirt=1;
	retval = 0;
end_rmdir:
	iput(dir);
	iput(inode);
	brelse(bh);
	return retval;
}

int sysv_unlink(struct inode * dir, const char * name, int len)
{
	int retval;
	struct inode * inode;
	struct buffer_head * bh;
	struct sysv_dir_entry * de;

repeat:
	retval = -ENOENT;
	inode = NULL;
	bh = sysv_find_entry(dir,name,len,&de);
	if (!bh)
		goto end_unlink;
	if (!(inode = iget(dir->i_sb, de->inode)))
		goto end_unlink;
	retval = -EPERM;
	if (S_ISDIR(inode->i_mode))
		goto end_unlink;
	if (de->inode != inode->i_ino) {
		iput(inode);
		brelse(bh);
		current->counter = 0;
		schedule();
		goto repeat;
	}
	if ((dir->i_mode & S_ISVTX) && !suser() &&
	    current->euid != inode->i_uid &&
	    current->euid != dir->i_uid)
		goto end_unlink;
	if (de->inode != inode->i_ino) {
		retval = -ENOENT;
		goto end_unlink;
	}
	if (!inode->i_nlink) {
		printk("Deleting nonexistent file (%04x:%lu), %d\n",
			inode->i_dev,inode->i_ino,inode->i_nlink);
		inode->i_nlink=1;
	}
	de->inode = 0;
	bh->b_dirt = 1;
	dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	dir->i_dirt = 1;
	inode->i_nlink--;
	inode->i_ctime = dir->i_ctime;
	inode->i_dirt = 1;
	retval = 0;
end_unlink:
	brelse(bh);
	iput(inode);
	iput(dir);
	return retval;
}

int sysv_symlink(struct inode * dir, const char * name, int len, const char * symname)
{
	struct sysv_dir_entry * de;
	struct inode * inode;
	struct buffer_head * name_block;
	char * name_block_data;
	struct super_block * sb;
	int i;
	char c;
	struct buffer_head * bh;

	if (!(inode = sysv_new_inode(dir))) {
		iput(dir);
		return -ENOSPC;
	}
	inode->i_mode = S_IFLNK | 0777;
	inode->i_op = &sysv_symlink_inode_operations;
	name_block = sysv_file_bread(inode,0,1,&name_block_data);
	if (!name_block) {
		iput(dir);
		inode->i_nlink--;
		inode->i_dirt = 1;
		iput(inode);
		return -ENOSPC;
	}
	sb = inode->i_sb;
	i = 0;
	while (i < sb->sv_block_size_1 && (c = *(symname++)))
		name_block_data[i++] = c;
	name_block_data[i] = 0;
	name_block->b_dirt = 1;
	brelse(name_block);
	inode->i_size = i;
	inode->i_dirt = 1;
	bh = sysv_find_entry(dir,name,len,&de);
	if (bh) {
		inode->i_nlink--;
		inode->i_dirt = 1;
		iput(inode);
		brelse(bh);
		iput(dir);
		return -EEXIST;
	}
	i = sysv_add_entry(dir, name, len, &bh, &de);
	if (i) {
		inode->i_nlink--;
		inode->i_dirt = 1;
		iput(inode);
		iput(dir);
		return i;
	}
	de->inode = inode->i_ino;
	bh->b_dirt = 1;
	brelse(bh);
	iput(dir);
	iput(inode);
	return 0;
}

int sysv_link(struct inode * oldinode, struct inode * dir, const char * name, int len)
{
	int error;
	struct sysv_dir_entry * de;
	struct buffer_head * bh;

	if (S_ISDIR(oldinode->i_mode)) {
		iput(oldinode);
		iput(dir);
		return -EPERM;
	}
	if (oldinode->i_nlink >= oldinode->i_sb->sv_link_max) {
		iput(oldinode);
		iput(dir);
		return -EMLINK;
	}
	bh = sysv_find_entry(dir,name,len,&de);
	if (bh) {
		brelse(bh);
		iput(dir);
		iput(oldinode);
		return -EEXIST;
	}
	error = sysv_add_entry(dir, name, len, &bh, &de);
	if (error) {
		iput(dir);
		iput(oldinode);
		return error;
	}
	de->inode = oldinode->i_ino;
	bh->b_dirt = 1;
	brelse(bh);
	iput(dir);
	oldinode->i_nlink++;
	oldinode->i_ctime = CURRENT_TIME;
	oldinode->i_dirt = 1;
	iput(oldinode);
	return 0;
}

/* return 1 if `new' is a subdir of `old' on the same device */
static int subdir(struct inode * new_inode, struct inode * old_inode)
{
	int ino;
	int result;

	new_inode->i_count++;
	result = 0;
	for (;;) {
		if (new_inode == old_inode) {
			result = 1;
			break;
		}
		if (new_inode->i_dev != old_inode->i_dev)
			break;
		ino = new_inode->i_ino;
		if (sysv_lookup(new_inode,"..",2,&new_inode))
			break;
		if (new_inode->i_ino == ino) /* root dir reached ? */
			break;
	}
	iput(new_inode);
	return result;
}

#define PARENT_INO(buffer) \
(((struct sysv_dir_entry *) ((buffer) + 1*SYSV_DIRSIZE))->inode)

/*
 * rename uses retrying to avoid race-conditions: at least they should be minimal.
 * it tries to allocate all the blocks, then sanity-checks, and if the sanity-
 * checks fail, it tries to restart itself again. Very practical - no changes
 * are done until we know everything works ok.. and then all the changes can be
 * done in one fell swoop when we have claimed all the buffers needed.
 *
 * Anybody can rename anything with this: the permission checks are left to the
 * higher-level routines.
 */
static int do_sysv_rename(struct inode * old_dir, const char * old_name, int old_len,
	struct inode * new_dir, const char * new_name, int new_len)
{
	struct inode * old_inode, * new_inode;
	struct buffer_head * old_bh, * new_bh, * dir_bh;
	char * dir_bh_data;
	struct sysv_dir_entry * old_de, * new_de;
	int retval;

	goto start_up;
try_again:
	brelse(old_bh);
	brelse(new_bh);
	brelse(dir_bh);
	iput(old_inode);
	iput(new_inode);
	current->counter = 0;
	schedule();
start_up:
	old_inode = new_inode = NULL;
	old_bh = new_bh = dir_bh = NULL;
	old_bh = sysv_find_entry(old_dir,old_name,old_len,&old_de);
	retval = -ENOENT;
	if (!old_bh)
		goto end_rename;
	old_inode = __iget(old_dir->i_sb, old_de->inode, 0); /* don't cross mnt-points */
	if (!old_inode)
		goto end_rename;
	retval = -EPERM;
	if ((old_dir->i_mode & S_ISVTX) && 
	    current->euid != old_inode->i_uid &&
	    current->euid != old_dir->i_uid && !suser())
		goto end_rename;
	new_bh = sysv_find_entry(new_dir,new_name,new_len,&new_de);
	if (new_bh) {
		new_inode = __iget(new_dir->i_sb, new_de->inode, 0);
		if (!new_inode) {
			brelse(new_bh);
			new_bh = NULL;
		}
	}
	if (new_inode == old_inode) {
		retval = 0;
		goto end_rename;
	}
	if (new_inode && S_ISDIR(new_inode->i_mode)) {
		retval = -EISDIR;
		if (!S_ISDIR(old_inode->i_mode))
			goto end_rename;
		retval = -EINVAL;
		if (subdir(new_dir, old_inode))
			goto end_rename;
		retval = -ENOTEMPTY;
		if (!empty_dir(new_inode))
			goto end_rename;
		retval = -EBUSY;
		if (new_inode->i_count > 1)
			goto end_rename;
	}
	retval = -EPERM;
	if (new_inode && (new_dir->i_mode & S_ISVTX) && 
	    current->euid != new_inode->i_uid &&
	    current->euid != new_dir->i_uid && !suser())
		goto end_rename;
	if (S_ISDIR(old_inode->i_mode)) {
		retval = -ENOTDIR;
		if (new_inode && !S_ISDIR(new_inode->i_mode))
			goto end_rename;
		retval = -EINVAL;
		if (subdir(new_dir, old_inode))
			goto end_rename;
		retval = -EIO;
		dir_bh = sysv_file_bread(old_inode,0,0,&dir_bh_data);
		if (!dir_bh)
			goto end_rename;
		if (PARENT_INO(dir_bh_data) != old_dir->i_ino)
			goto end_rename;
		retval = -EMLINK;
		if (!new_inode && new_dir->i_nlink >= new_dir->i_sb->sv_link_max)
			goto end_rename;
	}
	if (!new_bh) {
		retval = sysv_add_entry(new_dir,new_name,new_len,&new_bh,&new_de);
		if (retval)
			goto end_rename;
	}
/* sanity checking before doing the rename - avoid races */
	if (new_inode && (new_de->inode != new_inode->i_ino))
		goto try_again;
	if (new_de->inode && !new_inode)
		goto try_again;
	if (old_de->inode != old_inode->i_ino)
		goto try_again;
/* ok, that's it */
	old_de->inode = 0;
	new_de->inode = old_inode->i_ino;
	old_dir->i_ctime = old_dir->i_mtime = CURRENT_TIME;
	old_dir->i_dirt = 1;
	new_dir->i_ctime = new_dir->i_mtime = CURRENT_TIME;
	new_dir->i_dirt = 1;
	if (new_inode) {
		new_inode->i_nlink--;
		new_inode->i_ctime = CURRENT_TIME;
		new_inode->i_dirt = 1;
	}
	old_bh->b_dirt = 1;
	new_bh->b_dirt = 1;
	if (dir_bh) {
		PARENT_INO(dir_bh_data) = new_dir->i_ino;
		dir_bh->b_dirt = 1;
		old_dir->i_nlink--;
		old_dir->i_dirt = 1;
		if (new_inode) {
			new_inode->i_nlink--;
			new_inode->i_dirt = 1;
		} else {
			new_dir->i_nlink++;
			new_dir->i_dirt = 1;
		}
	}
	retval = 0;
end_rename:
	brelse(dir_bh);
	brelse(old_bh);
	brelse(new_bh);
	iput(old_inode);
	iput(new_inode);
	iput(old_dir);
	iput(new_dir);
	return retval;
}

/*
 * Ok, rename also locks out other renames, as they can change the parent of
 * a directory, and we don't want any races. Other races are checked for by
 * "do_rename()", which restarts if there are inconsistencies.
 *
 * Note that there is no race between different filesystems: it's only within
 * the same device that races occur: many renames can happen at once, as long
 * as they are on different partitions.
 */
int sysv_rename(struct inode * old_dir, const char * old_name, int old_len,
	struct inode * new_dir, const char * new_name, int new_len)
{
	static struct wait_queue * wait = NULL;
	static int lock = 0;
	int result;

	while (lock)
		sleep_on(&wait);
	lock = 1;
	result = do_sysv_rename(old_dir, old_name, old_len,
		new_dir, new_name, new_len);
	lock = 0;
	wake_up(&wait);
	return result;
}
