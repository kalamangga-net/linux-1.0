/*
 *  linux/fs/ext2/dir.c
 *
 *  Copyright (C) 1992, 1993, 1994  Remy Card (card@masi.ibp.fr)
 *                                  Laboratoire MASI - Institut Blaise Pascal
 *                                  Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/dir.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext2 directory handling functions
 */

#include <asm/segment.h>

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/ext2_fs.h>
#include <linux/sched.h>
#include <linux/stat.h>

static int ext2_dir_read (struct inode * inode, struct file * filp,
			    char * buf, int count)
{
	return -EISDIR;
}

static int ext2_readdir (struct inode *, struct file *, struct dirent *, int);

static struct file_operations ext2_dir_operations = {
	NULL,			/* lseek - default */
	ext2_dir_read,		/* read */
	NULL,			/* write - bad */
	ext2_readdir,		/* readdir */
	NULL,			/* select - default */
	ext2_ioctl,		/* ioctl */
	NULL,			/* mmap */
	NULL,			/* no special open code */
	NULL,			/* no special release code */
	file_fsync		/* fsync */
};

/*
 * directories can handle most operations...
 */
struct inode_operations ext2_dir_inode_operations = {
	&ext2_dir_operations,	/* default directory file-ops */
	ext2_create,		/* create */
	ext2_lookup,		/* lookup */
	ext2_link,		/* link */
	ext2_unlink,		/* unlink */
	ext2_symlink,		/* symlink */
	ext2_mkdir,		/* mkdir */
	ext2_rmdir,		/* rmdir */
	ext2_mknod,		/* mknod */
	ext2_rename,		/* rename */
	NULL,			/* readlink */
	NULL,			/* follow_link */
	NULL,			/* bmap */
	ext2_truncate,		/* truncate */
	ext2_permission		/* permission */
};

int ext2_check_dir_entry (char * function, struct inode * dir,
			  struct ext2_dir_entry * de, struct buffer_head * bh,
			  unsigned long offset)
{
	char * error_msg = NULL;

	if (de->rec_len < EXT2_DIR_REC_LEN(1))
		error_msg = "rec_len is smaller than minimal";
	else if (de->rec_len % 4 != 0)
		error_msg = "rec_len % 4 != 0";
	else if (de->rec_len < EXT2_DIR_REC_LEN(de->name_len))
		error_msg = "rec_len is too small for name_len";
	else if (dir && ((char *) de - bh->b_data) + de->rec_len >
		 dir->i_sb->s_blocksize)
		error_msg = "directory entry across blocks";

	if (error_msg != NULL)
		ext2_error (dir->i_sb, function, "bad directory entry: %s\n"
			    "offset=%lu, inode=%lu, rec_len=%d, name_len=%d",
			    error_msg, offset, de->inode, de->rec_len,
			    de->name_len);
	return error_msg == NULL ? 1 : 0;
}

static int ext2_readdir (struct inode * inode, struct file * filp,
			 struct dirent * dirent, int count)
{
	unsigned long offset, blk;
	int i, num;
	struct buffer_head * bh, * tmp, * bha[16];
	struct ext2_dir_entry * de;
	struct super_block * sb;
	int err;
	
	if (!inode || !S_ISDIR(inode->i_mode))
		return -EBADF;
	sb = inode->i_sb;
	while (filp->f_pos < inode->i_size) {
		offset = filp->f_pos & (sb->s_blocksize - 1);
		blk = (filp->f_pos) >> EXT2_BLOCK_SIZE_BITS(sb);
		bh = ext2_bread (inode, blk, 0, &err);
		if (!bh) {
			filp->f_pos += sb->s_blocksize - offset;
			continue;
		}

		/*
		 * Do the readahead
		 */
		if (!offset) {
			for (i = 16 >> (EXT2_BLOCK_SIZE_BITS(sb) - 9), num = 0;
			     i > 0; i--) {
				tmp = ext2_getblk (inode, ++blk, 0, &err);
				if (tmp && !tmp->b_uptodate && !tmp->b_lock)
					bha[num++] = tmp;
				else
					brelse (tmp);
			}
			if (num) {
				ll_rw_block (READA, num, bha);
				for (i = 0; i < num; i++)
					brelse (bha[i]);
			}
		}
		
		de = (struct ext2_dir_entry *) (offset + bh->b_data);
		while (offset < sb->s_blocksize && filp->f_pos < inode->i_size) {
			if (!ext2_check_dir_entry ("ext2_readdir", inode, de,
						   bh, offset)) {
				brelse (bh);
				return 0;
			}
			offset += de->rec_len;
			filp->f_pos += de->rec_len;
			if (de->inode) {
				memcpy_tofs (dirent->d_name, de->name,
					     de->name_len);
				put_fs_long (de->inode, &dirent->d_ino);
				put_fs_byte (0, de->name_len + dirent->d_name);
				put_fs_word (de->name_len, &dirent->d_reclen);
#ifndef DONT_USE_DCACHE
				ext2_dcache_add (inode->i_dev, inode->i_ino,
						 de->name, de->name_len,
						 de->inode);
#endif
				i = de->name_len;
				brelse (bh);
				if (!IS_RDONLY(inode)) {
					inode->i_atime = CURRENT_TIME;
					inode->i_dirt = 1;
				}
				return i;
			}
			de = (struct ext2_dir_entry *) ((char *) de +
							de->rec_len);
		}
		brelse (bh);
	}
	if (!IS_RDONLY(inode)) {
		inode->i_atime = CURRENT_TIME;
		inode->i_dirt = 1;
	}
	return 0;
}
