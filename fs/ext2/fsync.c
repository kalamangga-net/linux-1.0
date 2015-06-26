/*
 *  linux/fs/ext2/fsync.c
 *
 *  Copyright (C) 1993  Stephen Tweedie (sct@dcs.ed.ac.uk)
 *  from
 *  Copyright (C) 1992  Remy Card (card@masi.ibp.fr)
 *                      Laboratoire MASI - Institut Blaise Pascal
 *                      Universite Pierre et Marie Curie (Paris VI)
 *  from
 *  linux/fs/minix/truncate.c   Copyright (C) 1991, 1992  Linus Torvalds
 * 
 *  ext2fs fsync primitive
 */

#include <asm/segment.h>
#include <asm/system.h>

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/ext2_fs.h>
#include <linux/fcntl.h>
#include <linux/sched.h>
#include <linux/stat.h>
#include <linux/locks.h>


#define blocksize (EXT2_BLOCK_SIZE(inode->i_sb))
#define addr_per_block (EXT2_ADDR_PER_BLOCK(inode->i_sb))

static int sync_block (struct inode * inode, unsigned long * block, int wait)
{
	struct buffer_head * bh;
	int tmp;
	
	if (!*block)
		return 0;
	tmp = *block;
	bh = get_hash_table (inode->i_dev, *block, blocksize);
	if (!bh)
		return 0;
	if (*block != tmp) {
		brelse (bh);
		return 1;
	}
	if (wait && bh->b_req && !bh->b_uptodate) {
		brelse (bh);
		return -1;
	}
	if (wait || !bh->b_uptodate || !bh->b_dirt) {
		brelse (bh);
		return 0;
	}
	ll_rw_block (WRITE, 1, &bh);
	bh->b_count--;
	return 0;
}

static int sync_iblock (struct inode * inode, unsigned long * iblock, 
			struct buffer_head ** bh, int wait) 
{
	int rc, tmp;
	
	*bh = NULL;
	tmp = *iblock;
	if (!tmp)
		return 0;
	rc = sync_block (inode, iblock, wait);
	if (rc)
		return rc;
	*bh = bread (inode->i_dev, tmp, blocksize);
	if (tmp != *iblock) {
		brelse (*bh);
		*bh = NULL;
		return 1;
	}
	if (!*bh)
		return -1;
	return 0;
}


static int sync_direct (struct inode * inode, int wait)
{
	int i;
	int rc, err = 0;

	for (i = 0; i < EXT2_NDIR_BLOCKS; i++) {
		rc = sync_block (inode, inode->u.ext2_i.i_data + i, wait);
		if (rc > 0)
			break;
		if (rc)
			err = rc;
	}
	return err;
}

static int sync_indirect (struct inode * inode, unsigned long * iblock,
			  int wait)
{
	int i;
	struct buffer_head * ind_bh;
	int rc, err = 0;

	rc = sync_iblock (inode, iblock, &ind_bh, wait);
	if (rc || !ind_bh)
		return rc;
	
	for (i = 0; i < addr_per_block; i++) {
		rc = sync_block (inode, 
				 ((unsigned long *) ind_bh->b_data) + i,
				 wait);
		if (rc > 0)
			break;
		if (rc)
			err = rc;
	}
	brelse (ind_bh);
	return err;
}

static int sync_dindirect (struct inode * inode, unsigned long * diblock,
			   int wait)
{
	int i;
	struct buffer_head * dind_bh;
	int rc, err = 0;

	rc = sync_iblock (inode, diblock, &dind_bh, wait);
	if (rc || !dind_bh)
		return rc;
	
	for (i = 0; i < addr_per_block; i++) {
		rc = sync_indirect (inode,
				    ((unsigned long *) dind_bh->b_data) + i,
				    wait);
		if (rc > 0)
			break;
		if (rc)
			err = rc;
	}
	brelse (dind_bh);
	return err;
}

static int sync_tindirect (struct inode * inode, unsigned long * tiblock, 
			   int wait)
{
	int i;
	struct buffer_head * tind_bh;
	int rc, err = 0;

	rc = sync_iblock (inode, tiblock, &tind_bh, wait);
	if (rc || !tind_bh)
		return rc;
	
	for (i = 0; i < addr_per_block; i++) {
		rc = sync_dindirect (inode,
				     ((unsigned long *) tind_bh->b_data) + i,
				     wait);
		if (rc > 0)
			break;
		if (rc)
			err = rc;
	}
	brelse (tind_bh);
	return err;
}

int ext2_sync_file (struct inode * inode, struct file * file)
{
	int wait, err = 0;

	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
	     S_ISLNK(inode->i_mode)))
		return -EINVAL;
	if (S_ISLNK(inode->i_mode) && !(inode->i_blocks))
		/*
		 * Don't sync fast links!
		 */
		goto skip;

	for (wait=0; wait<=1; wait++)
	{
		err |= sync_direct (inode, wait);
		err |= sync_indirect (inode,
				      inode->u.ext2_i.i_data+EXT2_IND_BLOCK,
				      wait);
		err |= sync_dindirect (inode,
				       inode->u.ext2_i.i_data+EXT2_DIND_BLOCK, 
				       wait);
		err |= sync_tindirect (inode, 
				       inode->u.ext2_i.i_data+EXT2_TIND_BLOCK, 
				       wait);
	}
skip:
	err |= ext2_sync_inode (inode);
	return (err < 0) ? -EIO : 0;
}
