/*
 *  Linux/fs/xiafs/namei.c
 *
 *  Copyright (C) Q. Frank Xia, 1993.
 *  
 *  Based on Linus' minix/namei.c
 *  Copyright (C) Linus Torvalds, 1991, 1992.
 * 
 *  This software may be redistributed per Linux Copyright.
 */

#include <linux/sched.h>
#include <linux/xia_fs.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/errno.h>
#include <asm/segment.h>

#include "xiafs_mac.h"

#define RNDUP4(x)	((3+(u_long)(x)) & ~3)
/*
 * ok, we cannot use strncmp, as the name is not in our data space.
 * Thus we'll have to use xiafs_match. No big problem. Match also makes
 * some sanity tests.
 *
 * NOTE! unlike strncmp, xiafs_match returns 1 for success, 0 for failure.
 */
static int xiafs_match(int len, const char * name, struct xiafs_direct * dep)
{
    int i;

    if (!dep || !dep->d_ino || len > _XIAFS_NAME_LEN)
        return 0;
    /* "" means "." ---> so paths like "/usr/lib//libc.a" work */
    if (!len && (dep->d_name[0]=='.') && (dep->d_name[1]=='\0'))
        return 1;
    if (len != dep->d_name_len)
        return 0;
    for (i=0; i < len; i++)
        if (*name++ != dep->d_name[i])
	    return 0;
    return 1;
}

/*
 *	xiafs_find_entry()
 *
 * finds an entry in the specified directory with the wanted name. It
 * returns the cache buffer in which the entry was found, and the entry
 * itself (as a parameter - res_dir). It does NOT read the inode of the
 * entry - you'll have to do that yourself if you want to.
 */
static struct buffer_head * 
xiafs_find_entry(struct inode * inode, const char * name, int namelen, 
	       struct xiafs_direct ** res_dir, struct xiafs_direct ** res_pre)
{
    int i, zones, pos;
    struct buffer_head * bh;
    struct xiafs_direct * dep, * dep_pre;

    *res_dir = NULL;
    if (!inode)
        return NULL;
    if (namelen > _XIAFS_NAME_LEN)
        return NULL;

    if (inode->i_size & (XIAFS_ZSIZE(inode->i_sb) - 1)) {
        printk("XIA-FS: bad dir size (%s %d)\n", WHERE_ERR);
	return NULL;
    }
    zones=inode->i_size >> XIAFS_ZSIZE_BITS(inode->i_sb);
    for (i=0; i < zones; i++ ) {
        bh = xiafs_bread(inode, i, 0);
	if (!bh)
	    continue;
	dep_pre=dep=(struct xiafs_direct *)bh->b_data;
	if (!i && (dep->d_rec_len != 12 || !dep->d_ino || 
		   dep->d_name_len != 1 || strcmp(dep->d_name, "."))) {
	    printk("XIA-FS: bad directory (%s %d)\n", WHERE_ERR);
	    brelse(bh);
	    return NULL;
	}
	pos = 0;
	while ( pos < XIAFS_ZSIZE(inode->i_sb) ) {
	    if (dep->d_ino > inode->i_sb->u.xiafs_sb.s_ninodes ||
		dep->d_rec_len < 12 || 
		dep->d_rec_len+(char *)dep > bh->b_data+XIAFS_ZSIZE(inode->i_sb) ||
		dep->d_name_len + 8 > dep->d_rec_len || dep->d_name_len <= 0 ||
		dep->d_name[dep->d_name_len] ) {
	        brelse(bh);
		return NULL;
	    }
	    if (xiafs_match(namelen, name, dep)) {
	        *res_dir=dep;
		if (res_pre) 
		    *res_pre=dep_pre;
		return bh;
	    }
	    pos += dep->d_rec_len;
	    dep_pre=dep;
	    dep=(struct xiafs_direct *)(bh->b_data + pos);
	}
	brelse(bh);
	if (pos > XIAFS_ZSIZE(inode->i_sb)) {
	    printk("XIA-FS: bad directory (%s %d)\n", WHERE_ERR);
	    return NULL;
	}
    }
    return NULL;
}

int xiafs_lookup(struct inode * dir, const char * name, int len,
	       struct inode ** result)
{
    int ino;
    struct xiafs_direct * dep;
    struct buffer_head * bh;

    *result = NULL;
    if (!dir)
        return -ENOENT;
    if (!S_ISDIR(dir->i_mode)) {
        iput(dir);
	return -ENOENT;
    }
    if (!(bh = xiafs_find_entry(dir, name, len, &dep, NULL))) {
        iput(dir);
	return -ENOENT;
    }
    ino = dep->d_ino;
    brelse(bh);
    if (!(*result = iget(dir->i_sb, ino))) {
        iput(dir);
	return -EACCES;
    }
    iput(dir);
    return 0;
}

/*
 *	xiafs_add_entry()
 *
 * adds a file entry to the specified directory, using the same
 * semantics as xiafs_find_entry(). It returns NULL if it failed.
 *
 * NOTE!! The inode part of 'de' is left at 0 - which means you
 * may not sleep between calling this and putting something into
 * the entry, as someone else might have used it while you slept.
 */
static struct buffer_head * xiafs_add_entry(struct inode * dir,
	const char * name, int namelen, struct xiafs_direct ** res_dir, 
	struct xiafs_direct ** res_pre)
{
    int i, pos, offset;
    struct buffer_head * bh;
    struct xiafs_direct * de, * de_pre;

    *res_dir = NULL;
    if (!dir || !namelen || namelen > _XIAFS_NAME_LEN)
        return NULL;

    if (dir->i_size & (XIAFS_ZSIZE(dir->i_sb) - 1)) {
        printk("XIA-FS: bad dir size (%s %d)\n", WHERE_ERR);
	return NULL;
    }
    pos=0;
    for ( ; ; ) {
        bh =  xiafs_bread(dir, pos >> XIAFS_ZSIZE_BITS(dir->i_sb), pos ? 1:0);
	if (!bh)
	    return NULL;
	de_pre=de=(struct xiafs_direct *)bh->b_data;
	if (!pos) {
	    if (de->d_rec_len != 12 || !de->d_ino || de->d_name_len != 1 ||
		strcmp(de->d_name, ".")) {
	        printk("XIA-FS: bad directory (%s %d)\n", WHERE_ERR);
		brelse(bh);
		return NULL;
	    }
	    offset = 12;
	    de_pre=de=(struct xiafs_direct *)(bh->b_data+12);
	} else
	    offset = 0;
	while (offset < XIAFS_ZSIZE(dir->i_sb)) {
	    if (pos >= dir->i_size) {
	        de->d_ino=0;
		de->d_name_len=0;
		de->d_name[0]=0;
		de->d_rec_len=XIAFS_ZSIZE(dir->i_sb);
		dir->i_size += XIAFS_ZSIZE(dir->i_sb);
		dir->i_dirt = 1;
	    } else {
	        if (de->d_ino > dir->i_sb->u.xiafs_sb.s_ninodes ||
		    de->d_rec_len < 12 || 
		    (char *)de+de->d_rec_len > bh->b_data+XIAFS_ZSIZE(dir->i_sb) ||
		    de->d_name_len + 8 > de->d_rec_len ||
		    de->d_name[de->d_name_len]) {
		    printk("XIA-FS: bad directory entry (%s %d)\n", WHERE_ERR);
		    brelse(bh);
		    return NULL;
		}
		if (de->d_ino &&
		    RNDUP4(de->d_name_len)+RNDUP4(namelen)+16<=de->d_rec_len) {
		    i=RNDUP4(de->d_name_len)+8;
		    de_pre=de;
		    de=(struct xiafs_direct *)(i+(u_char *)de_pre);
		    de->d_ino=0;
		    de->d_rec_len=de_pre->d_rec_len-i;
		    de_pre->d_rec_len=i;
		}
	    }
	    if (!de->d_ino && RNDUP4(namelen)+8 <= de->d_rec_len) {
		/*
		 * XXX all times should be set by caller upon successful
		 * completion.
		 */
	        dir->i_ctime = dir->i_mtime = CURRENT_TIME;
		dir->i_dirt = 1;
		memcpy(de->d_name, name, namelen);
		de->d_name[namelen]=0;
		de->d_name_len=namelen;
		bh->b_dirt = 1;
		*res_dir = de;
		if (res_pre)
		    *res_pre = de_pre;
		return bh;
	    }
	    offset+=de->d_rec_len;
	    de_pre=de;
	    de=(struct xiafs_direct *)(bh->b_data+offset);
	}
	brelse(bh);
	if (offset > XIAFS_ZSIZE(dir->i_sb)) {
	    printk("XIA-FS: bad directory (%s %d)\n", WHERE_ERR);
	    return NULL;
	}
	pos+=XIAFS_ZSIZE(dir->i_sb);
    }
    return NULL;
}

int xiafs_create(struct inode * dir, const char * name, int len, int mode,
	struct inode ** result)
{
    struct inode * inode;
    struct buffer_head * bh;
    struct xiafs_direct * de;

    *result = NULL;
    if (!dir)
        return -ENOENT;
    inode = xiafs_new_inode(dir);
    if (!inode) {
        iput(dir);
	return -ENOSPC;
    }
    inode->i_op = &xiafs_file_inode_operations;
    inode->i_mode = mode;
    inode->i_dirt = 1;
    bh = xiafs_add_entry(dir, name, len, &de, NULL);
    if (!bh) {
        inode->i_nlink--;
	inode->i_dirt = 1;
	iput(inode);
	iput(dir);
	return -ENOSPC;
    }
    de->d_ino = inode->i_ino;
    bh->b_dirt = 1;
    brelse(bh);
    iput(dir);
    *result = inode;
    return 0;
}

int xiafs_mknod(struct inode *dir, const char *name, int len, int mode, int rdev)
{
    struct inode * inode;
    struct buffer_head * bh;
    struct xiafs_direct * de;

    if (!dir)
        return -ENOENT;
    bh = xiafs_find_entry(dir,name,len,&de, NULL);
    if (bh) {
        brelse(bh);
	iput(dir);
	return -EEXIST;
    }
    inode = xiafs_new_inode(dir);
    if (!inode) {
        iput(dir);
	return -ENOSPC;
    }
    inode->i_uid = current->euid;
    inode->i_mode = mode;
    inode->i_op = NULL;
    if (S_ISREG(inode->i_mode))
        inode->i_op = &xiafs_file_inode_operations;
    else if (S_ISDIR(inode->i_mode)) {
        inode->i_op = &xiafs_dir_inode_operations;
	if (dir->i_mode & S_ISGID)
	    inode->i_mode |= S_ISGID;
    }
    else if (S_ISLNK(inode->i_mode))
        inode->i_op = &xiafs_symlink_inode_operations;
    else if (S_ISCHR(inode->i_mode))
        inode->i_op = &chrdev_inode_operations;
    else if (S_ISBLK(inode->i_mode))
        inode->i_op = &blkdev_inode_operations;
    else if (S_ISFIFO(inode->i_mode))
    	init_fifo(inode);
    if (S_ISBLK(mode) || S_ISCHR(mode))
        inode->i_rdev = rdev;
    inode->i_atime = inode->i_ctime = inode->i_atime = CURRENT_TIME;
    inode->i_dirt = 1;
    bh = xiafs_add_entry(dir, name, len, &de, NULL);
    if (!bh) {
        inode->i_nlink--;
	inode->i_dirt = 1;
	iput(inode);
	iput(dir);
	return -ENOSPC;
    }
    de->d_ino = inode->i_ino;
    bh->b_dirt = 1;
    brelse(bh);
    iput(dir);
    iput(inode);
    return 0;
}

int xiafs_mkdir(struct inode * dir, const char * name, int len, int mode)
{
    struct inode * inode;
    struct buffer_head * bh, *dir_block;
    struct xiafs_direct * de;
	
    bh = xiafs_find_entry(dir,name,len,&de, NULL);
    if (bh) {
        brelse(bh);
	iput(dir);
	return -EEXIST;
    }
    if (dir->i_nlink > 64000) {
        iput(dir);
        return -EMLINK;
    }
    inode = xiafs_new_inode(dir);
    if (!inode) {
        iput(dir);
	return -ENOSPC;
    }
    inode->i_op = &xiafs_dir_inode_operations;
    inode->i_size = XIAFS_ZSIZE(dir->i_sb);
    inode->i_atime = inode->i_ctime = inode->i_mtime = CURRENT_TIME;
    dir_block = xiafs_bread(inode,0,1);
    if (!dir_block) {
        iput(dir);
	inode->i_nlink--;
	inode->i_dirt = 1;
	iput(inode);
	return -ENOSPC;
    }
    de = (struct xiafs_direct *) dir_block->b_data;
    de->d_ino=inode->i_ino;
    strcpy(de->d_name,".");
    de->d_name_len=1;
    de->d_rec_len=12;
    de =(struct xiafs_direct *)(12 + dir_block->b_data);
    de->d_ino = dir->i_ino;
    strcpy(de->d_name,"..");
    de->d_name_len=2;
    de->d_rec_len=XIAFS_ZSIZE(dir->i_sb)-12;
    inode->i_nlink = 2;
    dir_block->b_dirt = 1;
    brelse(dir_block);
    inode->i_mode = S_IFDIR | (mode & S_IRWXUGO & ~current->umask);
    if (dir->i_mode & S_ISGID)
        inode->i_mode |= S_ISGID;
    inode->i_dirt = 1;
    bh = xiafs_add_entry(dir, name, len, &de, NULL);
    if (!bh) {
        iput(dir);
	inode->i_nlink=0;
	iput(inode);
	return -ENOSPC;
    }
    de->d_ino = inode->i_ino;
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
    int i, zones, offset;
    struct buffer_head * bh;
    struct xiafs_direct * de;

    if (inode->i_size & (XIAFS_ZSIZE(inode->i_sb)-1) ) {
        printk("XIA-FS: bad directory (%s %d)\n", WHERE_ERR);
	return 1;
    }

    zones=inode->i_size >> XIAFS_ZSIZE_BITS(inode->i_sb);
    for (i=0; i < zones; i++) {
        bh =  xiafs_bread(inode, i, 0);
	if (!i) {
	    if (!bh) {
	        printk("XIA-FS: bad directory (%s %d)\n", WHERE_ERR);
		return 1;
	    }
	    de=(struct xiafs_direct *)bh->b_data;
	    if (de->d_ino != inode->i_ino || strcmp(".", de->d_name) ||
		    de->d_rec_len != 12 ) {
	        printk("XIA-FS: bad directory (%s %d)\n", WHERE_ERR);
		brelse(bh);
		return 1;	 
	    }
	    de=(struct xiafs_direct *)(12 + bh->b_data);
	    if (!de->d_ino || strcmp("..", de->d_name)) {
	    	printk("XIA-FS: bad directory (%s %d)\n", WHERE_ERR);
		brelse(bh);
		return 1;
	    }
	    offset=de->d_rec_len+12;
	}
	else
	    offset = 0;
	if (!bh)
	    continue;
	while (offset < XIAFS_ZSIZE(inode->i_sb)) {
	    de=(struct xiafs_direct *)(bh->b_data+offset);
	    if (de->d_ino > inode->i_sb->u.xiafs_sb.s_ninodes ||
		de->d_rec_len < 12 || 
		(char *)de+de->d_rec_len > bh->b_data+XIAFS_ZSIZE(inode->i_sb) ||
		de->d_name_len + 8 > de->d_rec_len ||
		de->d_name[de->d_name_len]) {
	        printk("XIA-FS: bad directory (%s %d)\n", WHERE_ERR);
		brelse(bh);
		return 1;
	    }
	    if (de->d_ino) {
	        brelse(bh);
		return 0;
	    }
	    offset+=de->d_rec_len;
	}
	brelse(bh);
    }
    return 1;
}

static void xiafs_rm_entry(struct xiafs_direct *de, struct xiafs_direct * de_pre)
{
    if (de==de_pre) {
        de->d_ino=0;
	return;
    }
    while (de_pre->d_rec_len+(u_char *)de_pre < (u_char *)de) {
        if (de_pre->d_rec_len < 12) {
	    printk("XIA-FS: bad directory entry (%s %d)\n", WHERE_ERR);
	    return;
	}
        de_pre=(struct xiafs_direct *)(de_pre->d_rec_len+(u_char *)de_pre);
    }
    if (de_pre->d_rec_len+(u_char *)de_pre > (u_char *)de) {
        printk("XIA-FS: bad directory entry (%s %d)\n", WHERE_ERR);
	return;
    }
    de_pre->d_rec_len+=de->d_rec_len;
}

int xiafs_rmdir(struct inode * dir, const char * name, int len)
{
    int retval;
    struct inode * inode;
    struct buffer_head * bh;
    struct xiafs_direct * de, * de_pre;

    inode = NULL;
    bh = xiafs_find_entry(dir, name, len, &de, &de_pre);
    retval = -ENOENT;
    if (!bh)
        goto end_rmdir;
    retval = -EPERM;
    if (!(inode = iget(dir->i_sb, de->d_ino)))
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
    if (inode->i_count > 1) {
        retval = -EBUSY;
	goto end_rmdir;
    }
    if (inode->i_nlink != 2)
        printk("XIA-FS: empty directory has nlink!=2 (%s %d)\n", WHERE_ERR);
    xiafs_rm_entry(de, de_pre);
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

int xiafs_unlink(struct inode * dir, const char * name, int len)
{
    int retval;
    struct inode * inode;
    struct buffer_head * bh;
    struct xiafs_direct * de, * de_pre;

repeat:
    retval = -ENOENT;
    inode = NULL;
    bh = xiafs_find_entry(dir, name, len, &de, &de_pre);
    if (!bh)
        goto end_unlink;
    if (!(inode = iget(dir->i_sb, de->d_ino)))
        goto end_unlink;
    retval = -EPERM;
    if (S_ISDIR(inode->i_mode))
        goto end_unlink;
    if (de->d_ino != inode->i_ino) {
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
    if (!inode->i_nlink) {
        printk("XIA-FS: Deleting nonexistent file (%s %d)\n", WHERE_ERR);
	inode->i_nlink=1;
    }
    xiafs_rm_entry(de, de_pre);
    bh->b_dirt = 1;
    inode->i_ctime = dir->i_ctime = dir->i_mtime = CURRENT_TIME;
    dir->i_dirt = 1;
    inode->i_nlink--;
    inode->i_dirt = 1;
    retval = 0;
end_unlink:
    brelse(bh);
    iput(inode);
    iput(dir);
    return retval;
}

int xiafs_symlink(struct inode * dir, const char * name, 
		int len, const char * symname)
{
    struct xiafs_direct * de;
    struct inode * inode = NULL;
    struct buffer_head * bh = NULL, * name_block = NULL;
    int i;
    char c;

    bh = xiafs_find_entry(dir,name,len, &de, NULL);
    if (bh) {
	brelse(bh);
	iput(dir);
	return -EEXIST;
    }
    if (!(inode = xiafs_new_inode(dir))) {
        iput(dir);
	return -ENOSPC;
    }
    inode->i_mode = S_IFLNK | S_IRWXUGO;
    inode->i_op = &xiafs_symlink_inode_operations;
    name_block = xiafs_bread(inode,0,1);
    if (!name_block) {
        iput(dir);
	inode->i_nlink--;
	inode->i_dirt = 1;
	iput(inode);
	return -ENOSPC;
    }
    for (i = 0; i < BLOCK_SIZE-1 && (c=*symname++); i++)
        name_block->b_data[i] = c;
    name_block->b_data[i] = 0;
    name_block->b_dirt = 1;
    brelse(name_block);
    inode->i_size = i;
    inode->i_dirt = 1;
    bh = xiafs_add_entry(dir, name, len, &de, NULL);
    if (!bh) {
        inode->i_nlink--;
	inode->i_dirt = 1;
	iput(inode);
	iput(dir);
	return -ENOSPC;
    }
    de->d_ino = inode->i_ino;
    bh->b_dirt = 1;
    brelse(bh);
    iput(dir);
    iput(inode);
    return 0;
}

int xiafs_link(struct inode * oldinode, struct inode * dir, 
	     const char * name, int len)
{
    struct xiafs_direct * de;
    struct buffer_head * bh;

    if (S_ISDIR(oldinode->i_mode)) {
        iput(oldinode);
	iput(dir);
	return -EPERM;
    }
    if (oldinode->i_nlink > 64000) {
        iput(oldinode);
	iput(dir);
	return -EMLINK;
    }
    bh = xiafs_find_entry(dir, name, len, &de, NULL);
    if (bh) {
        brelse(bh);
	iput(dir);
	iput(oldinode);
	return -EEXIST;
    }
    bh = xiafs_add_entry(dir, name, len, &de, NULL);
    if (!bh) {
        iput(dir);
	iput(oldinode);
	return -ENOSPC;
    }
    de->d_ino = oldinode->i_ino;
    bh->b_dirt = 1;
    brelse(bh);
    iput(dir);
    oldinode->i_nlink++;
    oldinode->i_ctime = CURRENT_TIME;
    oldinode->i_dirt = 1;
    iput(oldinode);
    return 0;
}

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
	if (xiafs_lookup(new_inode,"..",2,&new_inode))
	    break;
	if (new_inode->i_ino == ino)
	    break;
    }
    iput(new_inode);
    return result;
}

#define PARENT_INO(buffer) \
    (((struct xiafs_direct *) ((u_char *)(buffer) + 12))->d_ino)

/*
 * rename uses retry to avoid race-conditions: at least they should be minimal.
 * it tries to allocate all the blocks, then sanity-checks, and if the sanity-
 * checks fail, it tries to restart itself again. Very practical - no changes
 * are done until we know everything works ok.. and then all the changes can be
 * done in one fell swoop when we have claimed all the buffers needed.
 *
 * Anybody can rename anything with this: the permission checks are left to the
 * higher-level routines.
 */
static int do_xiafs_rename(struct inode * old_dir, const char * old_name, 
			 int old_len, struct inode * new_dir, 
			 const char * new_name, int new_len)
{
    struct inode * old_inode, * new_inode;
    struct buffer_head * old_bh, * new_bh, * dir_bh;
    struct xiafs_direct * old_de, * old_de_pre, * new_de, * new_de_pre;
    int retval;

try_again:
    old_inode = new_inode = NULL;
    old_bh = new_bh = dir_bh = NULL;
    old_bh = xiafs_find_entry(old_dir, old_name, old_len, &old_de, &old_de_pre);
    retval = -ENOENT;
    if (!old_bh)
        goto end_rename;
    old_inode = __iget(old_dir->i_sb, old_de->d_ino, 0); /* don't cross mnt-points */
    if (!old_inode)
        goto end_rename;
    retval = -EPERM;
    if ((old_dir->i_mode & S_ISVTX) && 
	    current->euid != old_inode->i_uid &&
	    current->euid != old_dir->i_uid && !suser())
        goto end_rename;
    new_bh = xiafs_find_entry(new_dir, new_name, new_len, &new_de, NULL);
    if (new_bh) {
        new_inode = __iget(new_dir->i_sb, new_de->d_ino, 0);
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
        retval = -EEXIST;
	goto end_rename;
    }
    retval = -EPERM;
    if (new_inode && (new_dir->i_mode & S_ISVTX) && 
	    current->euid != new_inode->i_uid &&
 	    current->euid != new_dir->i_uid && !suser())
        goto end_rename;
    if (S_ISDIR(old_inode->i_mode)) {
        retval = -EEXIST;
	if (new_bh)
	    goto end_rename;
	retval = -EACCES;
	if (!permission(old_inode, MAY_WRITE))
	    goto end_rename;
	retval = -EINVAL;
	if (subdir(new_dir, old_inode))
	    goto end_rename;
	retval = -EIO;
	dir_bh = xiafs_bread(old_inode,0,0);
	if (!dir_bh)
	    goto end_rename;
	if (PARENT_INO(dir_bh->b_data) != old_dir->i_ino)
	    goto end_rename;
	retval = -EMLINK;
	if (new_dir->i_nlink > 64000)
	    goto end_rename;
    }
    if (!new_bh)
        new_bh = xiafs_add_entry(new_dir, new_name, new_len, &new_de, &new_de_pre);
    retval = -ENOSPC;
    if (!new_bh) 
        goto end_rename;
    /* sanity checking */
    if ( (new_inode && (new_de->d_ino != new_inode->i_ino))
	    || (new_de->d_ino && !new_inode)
	    || (old_de->d_ino != old_inode->i_ino)) {
        xiafs_rm_entry(new_de, new_de_pre);
        brelse(old_bh);
	brelse(new_bh);
	brelse(dir_bh);
	iput(old_inode);
	iput(new_inode);
	current->counter=0;
	schedule();
	goto try_again;
    }
    xiafs_rm_entry(old_de, old_de_pre);
    new_de->d_ino = old_inode->i_ino;
    if (new_inode) {
        new_inode->i_nlink--;
	new_inode->i_dirt = 1;
    }
    old_bh->b_dirt = 1;
    new_bh->b_dirt = 1;
    if (dir_bh) {
        PARENT_INO(dir_bh->b_data) = new_dir->i_ino;
	dir_bh->b_dirt = 1;
	old_dir->i_nlink--;
	new_dir->i_nlink++;
	old_dir->i_dirt = 1;
	new_dir->i_dirt = 1;
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
int xiafs_rename(struct inode * old_dir, const char * old_name, int old_len,
	struct inode * new_dir, const char * new_name, int new_len)
{
    static struct wait_queue * wait = NULL;
    static int lock = 0;
    int result;

    while (lock)
        sleep_on(&wait);
    lock = 1;
    result = do_xiafs_rename(old_dir, old_name, old_len,
			   new_dir, new_name, new_len);
    lock = 0;
    wake_up(&wait);
    return result;
}
