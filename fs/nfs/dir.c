/*
 *  linux/fs/nfs/dir.c
 *
 *  Copyright (C) 1992  Rick Sladkey
 *
 *  nfs directory handling functions
 */

#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/stat.h>
#include <linux/nfs_fs.h>
#include <linux/fcntl.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/malloc.h>
#include <linux/mm.h>

#include <asm/segment.h>	/* for fs functions */

static int nfs_dir_read(struct inode *, struct file *filp, char *buf,
			int count);
static int nfs_readdir(struct inode *, struct file *, struct dirent *, int);
static int nfs_lookup(struct inode *dir, const char *name, int len,
		      struct inode **result);
static int nfs_create(struct inode *dir, const char *name, int len, int mode,
		      struct inode **result);
static int nfs_mkdir(struct inode *dir, const char *name, int len, int mode);
static int nfs_rmdir(struct inode *dir, const char *name, int len);
static int nfs_unlink(struct inode *dir, const char *name, int len);
static int nfs_symlink(struct inode *inode, const char *name, int len,
		       const char *symname);
static int nfs_link(struct inode *oldinode, struct inode *dir,
		    const char *name, int len);
static int nfs_mknod(struct inode *dir, const char *name, int len, int mode,
		     int rdev);
static int nfs_rename(struct inode *old_dir, const char *old_name,
		      int old_len, struct inode *new_dir, const char *new_name,
		      int new_len);

static struct file_operations nfs_dir_operations = {
	NULL,			/* lseek - default */
	nfs_dir_read,		/* read - bad */
	NULL,			/* write - bad */
	nfs_readdir,		/* readdir */
	NULL,			/* select - default */
	NULL,			/* ioctl - default */
	NULL,			/* mmap */
	NULL,			/* no special open code */
	NULL,			/* no special release code */
	NULL			/* fsync */
};

struct inode_operations nfs_dir_inode_operations = {
	&nfs_dir_operations,	/* default directory file-ops */
	nfs_create,		/* create */
	nfs_lookup,		/* lookup */
	nfs_link,		/* link */
	nfs_unlink,		/* unlink */
	nfs_symlink,		/* symlink */
	nfs_mkdir,		/* mkdir */
	nfs_rmdir,		/* rmdir */
	nfs_mknod,		/* mknod */
	nfs_rename,		/* rename */
	NULL,			/* readlink */
	NULL,			/* follow_link */
	NULL,			/* bmap */
	NULL,			/* truncate */
	NULL			/* permission */
};

static int nfs_dir_read(struct inode *inode, struct file *filp, char *buf,
			int count)
{
	return -EISDIR;
}

/*
 * We need to do caching of directory entries to prevent an
 * incredible amount of RPC traffic.  Only the most recent open
 * directory is cached.  This seems sufficient for most purposes.
 * Technically, we ought to flush the cache on close but this is
 * not a problem in practice.
 */

static int nfs_readdir(struct inode *inode, struct file *filp,
		       struct dirent *dirent, int count)
{
	static int c_dev = 0;
	static int c_ino;
	static int c_size;
	static struct nfs_entry *c_entry = NULL;

	int result;
	int i;
	struct nfs_entry *entry;

	if (!inode || !S_ISDIR(inode->i_mode)) {
		printk("nfs_readdir: inode is NULL or not a directory\n");
		return -EBADF;
	}

	/* initialize cache memory if it hasn't been used before */

	if (c_entry == NULL) {
		i = sizeof (struct nfs_entry)*NFS_READDIR_CACHE_SIZE;
		c_entry = (struct nfs_entry *) kmalloc(i, GFP_KERNEL);
		for (i = 0; i < NFS_READDIR_CACHE_SIZE; i++) {
			c_entry[i].name = (char *) kmalloc(NFS_MAXNAMLEN + 1,
				GFP_KERNEL);
		}
	}
	entry = NULL;

	/* try to find it in the cache */

	if (inode->i_dev == c_dev && inode->i_ino == c_ino) {
		for (i = 0; i < c_size; i++) {
			if (filp->f_pos == c_entry[i].cookie) {
				if (i == c_size - 1) {
					if (c_entry[i].eof)
						return 0;
				}
				else
					entry = c_entry + i + 1;
				break;
			}
		}
	}

	/* if we didn't find it in the cache, revert to an nfs call */

	if (!entry) {
		result = nfs_proc_readdir(NFS_SERVER(inode), NFS_FH(inode),
			filp->f_pos, NFS_READDIR_CACHE_SIZE, c_entry);
		if (result < 0) {
			c_dev = 0;
			return result;
		}
		if (result > 0) {
			c_dev = inode->i_dev;
			c_ino = inode->i_ino;
			c_size = result;
			entry = c_entry + 0;
		}
	}

	/* if we found it in the cache or from an nfs call, return results */

	if (entry) {
		i = strlen(entry->name);
		memcpy_tofs(dirent->d_name, entry->name, i + 1);
		put_fs_long(entry->fileid, &dirent->d_ino);
		put_fs_word(i, &dirent->d_reclen);
		filp->f_pos = entry->cookie;
		return i;
	}
	return 0;
}

/*
 * Lookup caching is a big win for performance but this is just
 * a trial to see how well it works on a small scale.
 * For example, bash does a lookup on ".." 13 times for each path
 * element when running pwd.  Yes, hard to believe but true.
 * Try pwd in a filesystem mounted with noac.
 *
 * It trades a little cpu time and memory for a lot of network bandwidth.
 * Since the cache is not hashed yet, it is a good idea not to make it too
 * large because every lookup looks through the entire cache even
 * though most of them will fail.
 */

static struct nfs_lookup_cache_entry {
	int dev;
	int inode;
	char filename[NFS_MAXNAMLEN + 1];
	struct nfs_fh fhandle;
	struct nfs_fattr fattr;
	int expiration_date;
} nfs_lookup_cache[NFS_LOOKUP_CACHE_SIZE];

static struct nfs_lookup_cache_entry *nfs_lookup_cache_index(struct inode *dir,
							     const char *filename)
{
	struct nfs_lookup_cache_entry *entry;
	int i;

	for (i = 0; i < NFS_LOOKUP_CACHE_SIZE; i++) {
		entry = nfs_lookup_cache + i;
		if (entry->dev == dir->i_dev && entry->inode == dir->i_ino
		    && !strncmp(filename, entry->filename, NFS_MAXNAMLEN))
			return entry;
	}
	return NULL;
}

static int nfs_lookup_cache_lookup(struct inode *dir, const char *filename,
				   struct nfs_fh *fhandle,
				   struct nfs_fattr *fattr)
{
	static int nfs_lookup_cache_in_use = 0;

	struct nfs_lookup_cache_entry *entry;

	if (!nfs_lookup_cache_in_use) {
		memset(nfs_lookup_cache, 0, sizeof(nfs_lookup_cache));
		nfs_lookup_cache_in_use = 1;
	}
	if ((entry = nfs_lookup_cache_index(dir, filename))) {
		if (jiffies > entry->expiration_date) {
			entry->dev = 0;
			return 0;
		}
		*fhandle = entry->fhandle;
		*fattr = entry->fattr;
		return 1;
	}
	return 0;
}

static void nfs_lookup_cache_add(struct inode *dir, const char *filename,
				 struct nfs_fh *fhandle,
				 struct nfs_fattr *fattr)
{
	static int nfs_lookup_cache_pos = 0;
	struct nfs_lookup_cache_entry *entry;

	/* compensate for bug in SGI NFS server */
	if (fattr->size == -1 || fattr->uid == -1 || fattr->gid == -1
	    || fattr->atime.seconds == -1 || fattr->mtime.seconds == -1)
		return;
	if (!(entry = nfs_lookup_cache_index(dir, filename))) {
		entry = nfs_lookup_cache + nfs_lookup_cache_pos++;
		if (nfs_lookup_cache_pos == NFS_LOOKUP_CACHE_SIZE)
			nfs_lookup_cache_pos = 0;
	}
	entry->dev = dir->i_dev;
	entry->inode = dir->i_ino;
	strcpy(entry->filename, filename);
	entry->fhandle = *fhandle;
	entry->fattr = *fattr;
	entry->expiration_date = jiffies + (S_ISDIR(fattr->mode)
		? NFS_SERVER(dir)->acdirmax : NFS_SERVER(dir)->acregmax);
}

static void nfs_lookup_cache_remove(struct inode *dir, struct inode *inode,
				    const char *filename)
{
	struct nfs_lookup_cache_entry *entry;
	int dev;
	int fileid;
	int i;

	if (inode) {
		dev = inode->i_dev;
		fileid = inode->i_ino;
	}
	else if ((entry = nfs_lookup_cache_index(dir, filename))) {
		dev = entry->dev;
		fileid = entry->fattr.fileid;
	}
	else
		return;
	for (i = 0; i < NFS_LOOKUP_CACHE_SIZE; i++) {
		entry = nfs_lookup_cache + i;
		if (entry->dev == dev && entry->fattr.fileid == fileid)
			entry->dev = 0;
	}
}

static void nfs_lookup_cache_refresh(struct inode *file,
				     struct nfs_fattr *fattr)
{
	struct nfs_lookup_cache_entry *entry;
	int dev = file->i_dev;
	int fileid = file->i_ino;
	int i;

	for (i = 0; i < NFS_LOOKUP_CACHE_SIZE; i++) {
		entry = nfs_lookup_cache + i;
		if (entry->dev == dev && entry->fattr.fileid == fileid)
			entry->fattr = *fattr;
	}
}

static int nfs_lookup(struct inode *dir, const char *__name, int len,
		      struct inode **result)
{
	struct nfs_fh fhandle;
	struct nfs_fattr fattr;
	char name[len > NFS_MAXNAMLEN? 1 : len+1];
	int error;

	*result = NULL;
	if (!dir || !S_ISDIR(dir->i_mode)) {
		printk("nfs_lookup: inode is NULL or not a directory\n");
		iput(dir);
		return -ENOENT;
	}
	if (len > NFS_MAXNAMLEN) {
		iput(dir);
		return -ENAMETOOLONG;
	}
	memcpy(name,__name,len);
	name[len] = '\0';
	if (len == 1 && name[0] == '.') { /* cheat for "." */
		*result = dir;
		return 0;
	}
	if ((NFS_SERVER(dir)->flags & NFS_MOUNT_NOAC)
	    || !nfs_lookup_cache_lookup(dir, name, &fhandle, &fattr)) {
		if ((error = nfs_proc_lookup(NFS_SERVER(dir), NFS_FH(dir),
		    name, &fhandle, &fattr))) {
			iput(dir);
			return error;
		}
		nfs_lookup_cache_add(dir, name, &fhandle, &fattr);
	}
	if (!(*result = nfs_fhget(dir->i_sb, &fhandle, &fattr))) {
		iput(dir);
		return -EACCES;
	}
	iput(dir);
	return 0;
}

static int nfs_create(struct inode *dir, const char *name, int len, int mode,
		      struct inode **result)
{
	struct nfs_sattr sattr;
	struct nfs_fattr fattr;
	struct nfs_fh fhandle;
	int error;

	*result = NULL;
	if (!dir || !S_ISDIR(dir->i_mode)) {
		printk("nfs_create: inode is NULL or not a directory\n");
		iput(dir);
		return -ENOENT;
	}
	if (len > NFS_MAXNAMLEN) {
		iput(dir);
		return -ENAMETOOLONG;
	}
	sattr.mode = mode;
	sattr.uid = sattr.gid = sattr.size = (unsigned) -1;
	sattr.atime.seconds = sattr.mtime.seconds = (unsigned) -1;
	if ((error = nfs_proc_create(NFS_SERVER(dir), NFS_FH(dir),
		name, &sattr, &fhandle, &fattr))) {
		iput(dir);
		return error;
	}
	if (!(*result = nfs_fhget(dir->i_sb, &fhandle, &fattr))) {
		iput(dir);
		return -EACCES;
	}
	nfs_lookup_cache_add(dir, name, &fhandle, &fattr);
	iput(dir);
	return 0;
}

static int nfs_mknod(struct inode *dir, const char *name, int len,
		     int mode, int rdev)
{
	struct nfs_sattr sattr;
	struct nfs_fattr fattr;
	struct nfs_fh fhandle;
	int error;

	if (!dir || !S_ISDIR(dir->i_mode)) {
		printk("nfs_mknod: inode is NULL or not a directory\n");
		iput(dir);
		return -ENOENT;
	}
	if (len > NFS_MAXNAMLEN) {
		iput(dir);
		return -ENAMETOOLONG;
	}
	sattr.mode = mode;
	sattr.uid = sattr.gid = (unsigned) -1;
	if (S_ISCHR(mode) || S_ISBLK(mode))
		sattr.size = rdev; /* get out your barf bag */
	else
		sattr.size = (unsigned) -1;
	sattr.atime.seconds = sattr.mtime.seconds = (unsigned) -1;
	error = nfs_proc_create(NFS_SERVER(dir), NFS_FH(dir),
		name, &sattr, &fhandle, &fattr);
	if (!error)
		nfs_lookup_cache_add(dir, name, &fhandle, &fattr);
	iput(dir);
	return error;
}

static int nfs_mkdir(struct inode *dir, const char *name, int len, int mode)
{
	struct nfs_sattr sattr;
	struct nfs_fattr fattr;
	struct nfs_fh fhandle;
	int error;

	if (!dir || !S_ISDIR(dir->i_mode)) {
		printk("nfs_mkdir: inode is NULL or not a directory\n");
		iput(dir);
		return -ENOENT;
	}
	if (len > NFS_MAXNAMLEN) {
		iput(dir);
		return -ENAMETOOLONG;
	}
	sattr.mode = mode;
	sattr.uid = sattr.gid = sattr.size = (unsigned) -1;
	sattr.atime.seconds = sattr.mtime.seconds = (unsigned) -1;
	error = nfs_proc_mkdir(NFS_SERVER(dir), NFS_FH(dir),
		name, &sattr, &fhandle, &fattr);
	if (!error)
		nfs_lookup_cache_add(dir, name, &fhandle, &fattr);
	iput(dir);
	return error;
}

static int nfs_rmdir(struct inode *dir, const char *name, int len)
{
	int error;

	if (!dir || !S_ISDIR(dir->i_mode)) {
		printk("nfs_rmdir: inode is NULL or not a directory\n");
		iput(dir);
		return -ENOENT;
	}
	if (len > NFS_MAXNAMLEN) {
		iput(dir);
		return -ENAMETOOLONG;
	}
	error = nfs_proc_rmdir(NFS_SERVER(dir), NFS_FH(dir), name);
	if (!error)
		nfs_lookup_cache_remove(dir, NULL, name);
	iput(dir);
	return error;
}

static int nfs_unlink(struct inode *dir, const char *name, int len)
{
	int error;

	if (!dir || !S_ISDIR(dir->i_mode)) {
		printk("nfs_unlink: inode is NULL or not a directory\n");
		iput(dir);
		return -ENOENT;
	}
	if (len > NFS_MAXNAMLEN) {
		iput(dir);
		return -ENAMETOOLONG;
	}
	error = nfs_proc_remove(NFS_SERVER(dir), NFS_FH(dir), name);
	if (!error)
		nfs_lookup_cache_remove(dir, NULL, name);
	iput(dir);
	return error;
}

static int nfs_symlink(struct inode *dir, const char *name, int len,
		       const char *symname)
{
	struct nfs_sattr sattr;
	int error;

	if (!dir || !S_ISDIR(dir->i_mode)) {
		printk("nfs_symlink: inode is NULL or not a directory\n");
		iput(dir);
		return -ENOENT;
	}
	if (len > NFS_MAXNAMLEN) {
		iput(dir);
		return -ENAMETOOLONG;
	}
	if (strlen(symname) > NFS_MAXPATHLEN) {
		iput(dir);
		return -ENAMETOOLONG;
	}
	sattr.mode = S_IFLNK | S_IRWXUGO; /* SunOS 4.1.2 crashes without this! */
	sattr.uid = sattr.gid = sattr.size = (unsigned) -1;
	sattr.atime.seconds = sattr.mtime.seconds = (unsigned) -1;
	error = nfs_proc_symlink(NFS_SERVER(dir), NFS_FH(dir),
		name, symname, &sattr);
	iput(dir);
	return error;
}

static int nfs_link(struct inode *oldinode, struct inode *dir,
		    const char *name, int len)
{
	int error;

	if (!oldinode) {
		printk("nfs_link: old inode is NULL\n");
		iput(oldinode);
		iput(dir);
		return -ENOENT;
	}
	if (!dir || !S_ISDIR(dir->i_mode)) {
		printk("nfs_link: dir is NULL or not a directory\n");
		iput(oldinode);
		iput(dir);
		return -ENOENT;
	}
	if (len > NFS_MAXNAMLEN) {
		iput(oldinode);
		iput(dir);
		return -ENAMETOOLONG;
	}
	error = nfs_proc_link(NFS_SERVER(oldinode), NFS_FH(oldinode),
		NFS_FH(dir), name);
	if (!error)
		nfs_lookup_cache_remove(dir, oldinode, NULL);
	iput(oldinode);
	iput(dir);
	return error;
}

static int nfs_rename(struct inode *old_dir, const char *old_name, int old_len,
		      struct inode *new_dir, const char *new_name, int new_len)
{
	int error;

	if (!old_dir || !S_ISDIR(old_dir->i_mode)) {
		printk("nfs_rename: old inode is NULL or not a directory\n");
		iput(old_dir);
		iput(new_dir);
		return -ENOENT;
	}
	if (!new_dir || !S_ISDIR(new_dir->i_mode)) {
		printk("nfs_rename: new inode is NULL or not a directory\n");
		iput(old_dir);
		iput(new_dir);
		return -ENOENT;
	}
	if (old_len > NFS_MAXNAMLEN || new_len > NFS_MAXNAMLEN) {
		iput(old_dir);
		iput(new_dir);
		return -ENAMETOOLONG;
	}
	error = nfs_proc_rename(NFS_SERVER(old_dir),
		NFS_FH(old_dir), old_name,
		NFS_FH(new_dir), new_name);
	if (!error) {
		nfs_lookup_cache_remove(old_dir, NULL, old_name);
		nfs_lookup_cache_remove(new_dir, NULL, new_name);
	}
	iput(old_dir);
	iput(new_dir);
	return error;
}

/*
 * Many nfs protocol calls return the new file attributes after
 * an operation.  Here we update the inode to reflect the state
 * of the server's inode.
 */

void nfs_refresh_inode(struct inode *inode, struct nfs_fattr *fattr)
{
	int was_empty;

	if (!inode || !fattr) {
		printk("nfs_refresh_inode: inode or fattr is NULL\n");
		return;
	}
	if (inode->i_ino != fattr->fileid) {
		printk("nfs_refresh_inode: inode number mismatch\n");
		return;
	}
	was_empty = inode->i_mode == 0;
	inode->i_mode = fattr->mode;
	inode->i_nlink = fattr->nlink;
	inode->i_uid = fattr->uid;
	inode->i_gid = fattr->gid;
	inode->i_size = fattr->size;
	inode->i_blksize = fattr->blocksize;
	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
		inode->i_rdev = fattr->rdev;
	else
		inode->i_rdev = 0;
	inode->i_blocks = fattr->blocks;
	inode->i_atime = fattr->atime.seconds;
	inode->i_mtime = fattr->mtime.seconds;
	inode->i_ctime = fattr->ctime.seconds;
	if (was_empty) {
		if (S_ISREG(inode->i_mode))
			inode->i_op = &nfs_file_inode_operations;
		else if (S_ISDIR(inode->i_mode))
			inode->i_op = &nfs_dir_inode_operations;
		else if (S_ISLNK(inode->i_mode))
			inode->i_op = &nfs_symlink_inode_operations;
		else if (S_ISCHR(inode->i_mode))
			inode->i_op = &chrdev_inode_operations;
		else if (S_ISBLK(inode->i_mode))
			inode->i_op = &blkdev_inode_operations;
		else if (S_ISFIFO(inode->i_mode))
			init_fifo(inode);
		else
			inode->i_op = NULL;
	}
	nfs_lookup_cache_refresh(inode, fattr);
}

