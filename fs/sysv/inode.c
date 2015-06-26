/*
 *  linux/fs/sysv/inode.c
 *
 *  minix/inode.c
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  xenix/inode.c
 *  Copyright (C) 1992  Doug Evans
 *
 *  coh/inode.c
 *  Copyright (C) 1993  Pascal Haible, Bruno Haible
 *
 *  sysv/inode.c
 *  Copyright (C) 1993  Paul B. Monday
 *
 *  sysv/inode.c
 *  Copyright (C) 1993  Bruno Haible
 *
 *  This file contains code for allocating/freeing inodes and for read/writing
 *  the superblock.
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/sysv_fs.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/locks.h>

#include <asm/segment.h>

void _coh_wait_on_inode (struct inode * inode)
{
	struct wait_queue wait = { current, NULL };

	add_wait_queue(&inode->u.sysv_i.i_wait, &wait);
repeat:
	current->state = TASK_UNINTERRUPTIBLE;
	if (inode->u.sysv_i.i_lock) {
		schedule();
		goto repeat;
	}
	remove_wait_queue(&inode->u.sysv_i.i_wait, &wait);
	current->state = TASK_RUNNING;
}

void sysv_put_inode(struct inode *inode)
{
	if (inode->i_nlink)
		return;
	inode->i_size = 0;
	sysv_truncate(inode);
	sysv_free_inode(inode);
}


static struct super_operations sysv_sops = { 
	sysv_read_inode,
	sysv_notify_change,
	sysv_write_inode,
	sysv_put_inode,
	sysv_put_super,
	sysv_write_super,
	sysv_statfs,
	NULL
};

/* The following functions try to recognize specific filesystems.
 * We recognize:
 * - Xenix FS by its magic number.
 * - SystemV FS by its magic number.
 * - Coherent FS by its funny fname/fpack field.
 * We discriminate among SystemV4 and SystemV2 FS by the assumption that
 * the time stamp is not < 01-01-1980.
 */

static void detected_bs512 (struct super_block *sb)
{
	sb->sv_block_size = 512;
	sb->sv_block_size_1 = 512-1;
	sb->sv_block_size_bits = 9;
	sb->sv_block_size_ratio = 2;
	sb->sv_block_size_ratio_1 = 2-1;
	sb->sv_block_size_ratio_bits = 1;
	sb->sv_inodes_per_block = 512/64;
	sb->sv_inodes_per_block_1 = 512/64-1;
	sb->sv_inodes_per_block_bits = 9-6;
	sb->sv_toobig_block = 10 + 
	  (sb->sv_ind_per_block = 512/4) +
	  (sb->sv_ind_per_block_2 = (512/4)*(512/4)) +
	  (sb->sv_ind_per_block_3 = (512/4)*(512/4)*(512/4));
	sb->sv_ind_per_block_1 = 512/4-1;
	sb->sv_ind_per_block_2_1 = (512/4)*(512/4)-1;
	sb->sv_ind_per_block_2_bits = 2 *
	  (sb->sv_ind_per_block_bits = 9-2);
}

static void detected_bs1024 (struct super_block *sb)
{
	sb->sv_block_size = 1024;
	sb->sv_block_size_1 = 1024-1;
	sb->sv_block_size_bits = 10;
	sb->sv_block_size_ratio = 1;
	sb->sv_block_size_ratio_1 = 1-1;
	sb->sv_block_size_ratio_bits = 0;
	sb->sv_inodes_per_block = 1024/64;
	sb->sv_inodes_per_block_1 = 1024/64-1;
	sb->sv_inodes_per_block_bits = 10-6;
	sb->sv_toobig_block = 10 + 
	  (sb->sv_ind_per_block = 1024/4) +
	  (sb->sv_ind_per_block_2 = (1024/4)*(1024/4)) +
	  (sb->sv_ind_per_block_3 = (1024/4)*(1024/4)*(1024/4));
	sb->sv_ind_per_block_1 = 1024/4-1;
	sb->sv_ind_per_block_2_1 = (1024/4)*(1024/4)-1;
	sb->sv_ind_per_block_2_bits = 2 *
	  (sb->sv_ind_per_block_bits = 10-2);
}

static const char* detect_xenix (struct super_block *sb, struct buffer_head *bh)
{
	struct xenix_super_block * sbd;

	sbd = (struct xenix_super_block *) bh->b_data;
	if (sbd->s_magic != 0x2b5544)
		return NULL;
	sb->sv_type = FSTYPE_XENIX;
	sb->sv_convert = 0;
	sb->sv_kludge_symlinks = 1;
	sb->sv_truncate = 1;
	sb->sv_link_max = XENIX_LINK_MAX;
	sb->sv_fic_size = XENIX_NICINOD;
	sb->sv_flc_size = XENIX_NICFREE;
	sb->sv_bh = bh;
	sb->sv_sbd = (char *) sbd;
	sb->sv_sb_fic_count = &sbd->s_ninode;
	sb->sv_sb_fic_inodes = &sbd->s_inode[0];
	sb->sv_sb_total_free_inodes = &sbd->s_tinode;
	sb->sv_sb_flc_count = &sbd->s_nfree;
	sb->sv_sb_flc_blocks = &sbd->s_free[0];
	sb->sv_sb_total_free_blocks = &sbd->s_tfree;
	sb->sv_sb_time = &sbd->s_time;
	sb->sv_block_base = 0;
	sb->sv_firstinodezone = 2;
	sb->sv_firstdatazone = sbd->s_isize;
	sb->sv_nzones = sbd->s_fsize;
	sb->sv_ndatazones = sb->sv_nzones - sb->sv_firstdatazone;
	switch (sbd->s_type) {
		case 1: detected_bs512(sb); break;
		case 2: detected_bs1024(sb); break;
		default: return NULL;
	}
	return "Xenix";
}

static const char* detect_sysv4 (struct super_block *sb, struct buffer_head *bh)
{
	struct sysv4_super_block * sbd;

	sbd = (struct sysv4_super_block *) (bh->b_data + BLOCK_SIZE/2);
	if (sbd->s_magic != 0xfd187e20)
		return NULL;
	if (sbd->s_time < 315532800) /* this is likely to happen on SystemV2 FS */
		return NULL;
	sb->sv_type = FSTYPE_SYSV4;
	sb->sv_convert = 0;
	sb->sv_kludge_symlinks = 0; /* ?? */
	sb->sv_truncate = 1;
	sb->sv_link_max = SYSV_LINK_MAX;
	sb->sv_fic_size = SYSV_NICINOD;
	sb->sv_flc_size = SYSV_NICFREE;
	sb->sv_bh = bh;
	sb->sv_sbd = (char *) sbd;
	sb->sv_sb_fic_count = &sbd->s_ninode;
	sb->sv_sb_fic_inodes = &sbd->s_inode[0];
	sb->sv_sb_total_free_inodes = &sbd->s_tinode;
	sb->sv_sb_flc_count = &sbd->s_nfree;
	sb->sv_sb_flc_blocks = &sbd->s_free[0];
	sb->sv_sb_total_free_blocks = &sbd->s_tfree;
	sb->sv_sb_time = &sbd->s_time;
	sb->sv_block_base = 0;
	sb->sv_firstinodezone = 2;
	sb->sv_firstdatazone = sbd->s_isize;
	sb->sv_nzones = sbd->s_fsize;
	sb->sv_ndatazones = sb->sv_nzones - sb->sv_firstdatazone;
	switch (sbd->s_type) {
		case 1: detected_bs512(sb); break;
		case 2: detected_bs1024(sb); break;
		default: return NULL;
	}
	return "SystemV";
}

static const char* detect_sysv2 (struct super_block *sb, struct buffer_head *bh)
{
	struct sysv2_super_block * sbd;

	sbd = (struct sysv2_super_block *) (bh->b_data + BLOCK_SIZE/2);
	if (sbd->s_magic != 0xfd187e20)
		return NULL;
	if (sbd->s_time < 315532800) /* this is likely to happen on SystemV4 FS */
		return NULL;
	sb->sv_type = FSTYPE_SYSV2;
	sb->sv_convert = 0;
	sb->sv_kludge_symlinks = 0; /* ?? */
	sb->sv_truncate = 1;
	sb->sv_link_max = SYSV_LINK_MAX;
	sb->sv_fic_size = SYSV_NICINOD;
	sb->sv_flc_size = SYSV_NICFREE;
	sb->sv_bh = bh;
	sb->sv_sbd = (char *) sbd;
	sb->sv_sb_fic_count = &sbd->s_ninode;
	sb->sv_sb_fic_inodes = &sbd->s_inode[0];
	sb->sv_sb_total_free_inodes = &sbd->s_tinode;
	sb->sv_sb_flc_count = &sbd->s_nfree;
	sb->sv_sb_flc_blocks = &sbd->s_free[0];
	sb->sv_sb_total_free_blocks = &sbd->s_tfree;
	sb->sv_sb_time = &sbd->s_time;
	sb->sv_block_base = 0;
	sb->sv_firstinodezone = 2;
	sb->sv_firstdatazone = sbd->s_isize;
	sb->sv_nzones = sbd->s_fsize;
	sb->sv_ndatazones = sb->sv_nzones - sb->sv_firstdatazone;
	switch (sbd->s_type) {
		case 1: detected_bs512(sb); break;
		case 2: detected_bs1024(sb); break;
		default: return NULL;
	}
	return "SystemV Release 2";
}

static const char* detect_coherent (struct super_block *sb, struct buffer_head *bh)
{
	struct coh_super_block * sbd;

	sbd = (struct coh_super_block *) (bh->b_data + BLOCK_SIZE/2);
	if ((memcmp(sbd->s_fname,"noname",6) && memcmp(sbd->s_fname,"xxxxx ",6))
	    || (memcmp(sbd->s_fpack,"nopack",6) && memcmp(sbd->s_fpack,"xxxxx\n",6)))
		return NULL;
	sb->sv_type = FSTYPE_COH;
	sb->sv_convert = 1;
	sb->sv_kludge_symlinks = 1;
	sb->sv_truncate = 1;
	sb->sv_link_max = COH_LINK_MAX;
	sb->sv_fic_size = COH_NICINOD;
	sb->sv_flc_size = COH_NICFREE;
	sb->sv_bh = bh;
	sb->sv_sbd = (char *) sbd;
	sb->sv_sb_fic_count = &sbd->s_ninode;
	sb->sv_sb_fic_inodes = &sbd->s_inode[0];
	sb->sv_sb_total_free_inodes = &sbd->s_tinode;
	sb->sv_sb_flc_count = &sbd->s_nfree;
	sb->sv_sb_flc_blocks = &sbd->s_free[0];
	sb->sv_sb_total_free_blocks = &sbd->s_tfree;
	sb->sv_sb_time = &sbd->s_time;
	sb->sv_block_base = 0;
	sb->sv_firstinodezone = 2;
	sb->sv_firstdatazone = sbd->s_isize;
	sb->sv_nzones = from_coh_ulong(sbd->s_fsize);
	sb->sv_ndatazones = sb->sv_nzones - sb->sv_firstdatazone;
	detected_bs512(sb);
	return "Coherent";
}

struct super_block *sysv_read_super(struct super_block *sb,void *data, 
				     int silent)
{
	struct buffer_head *bh;
	const char *found;
	int dev = sb->s_dev;

	if (1024 != sizeof (struct xenix_super_block))
		panic("Xenix FS: bad super-block size");
	if ((512 != sizeof (struct sysv4_super_block))
            || (512 != sizeof (struct sysv2_super_block)))
		panic("SystemV FS: bad super-block size");
	if (500 != sizeof (struct coh_super_block))
		panic("Coherent FS: bad super-block size");
	if (64 != sizeof (struct sysv_inode))
		panic("sysv fs: bad i-node size");
	lock_super(sb);
	sb->s_blocksize = BLOCK_SIZE; /* anything else not supported by the block device drivers */
	sb->s_blocksize_bits = BLOCK_SIZE_BITS;

	/* Try to read Xenix superblock */
	if ((bh = bread(dev, 1, BLOCK_SIZE)) != NULL) {
		if ((found = detect_xenix(sb,bh)) != NULL)
			goto ok;
		brelse(bh);
	}
	if ((bh = bread(dev, 0, BLOCK_SIZE)) != NULL) {
		/* Try to recognize SystemV superblock */
		if ((found = detect_sysv4(sb,bh)) != NULL)
			goto ok;
		if ((found = detect_sysv2(sb,bh)) != NULL)
			goto ok;
		/* Try to recognize Coherent superblock */
		if ((found = detect_coherent(sb,bh)) != NULL)
			goto ok;
		brelse(bh);
	}
	/* Try to recognize SystemV2 superblock */
	/* Offset by 1 track, i.e. most probably 9, 15, or 18 kilobytes. */
	{	static int offsets[] = { 9, 15, 18, };
		int i;
		for (i = 0; i < sizeof(offsets)/sizeof(offsets[0]); i++)
			if ((bh = bread(dev, offsets[i], BLOCK_SIZE)) != NULL) {
				/* Try to recognize SystemV superblock */
				if ((found = detect_sysv2(sb,bh)) != NULL) {
					sb->sv_block_base = offsets[i];
					goto ok;
				}
				brelse(bh);
			}
	}
	sb->s_dev=0;
	unlock_super(sb);
	if (!silent)
		printk("VFS: unable to read Xenix/SystemV/Coherent superblock on device %d/%d\n",MAJOR(dev),MINOR(dev));
	return NULL;

	ok:
	sb->sv_ninodes = (sb->sv_firstdatazone - sb->sv_firstinodezone) << sb->sv_inodes_per_block_bits;
	if (!silent)
		printk("VFS: Found a %s FS (block size = %d) on device %d/%d\n",found,sb->sv_block_size,MAJOR(dev),MINOR(dev));
	sb->s_magic = SYSV_MAGIC_BASE + sb->sv_type;
	/* set up enough so that it can read an inode */
	sb->s_dev = dev;
	sb->s_op = &sysv_sops;
	sb->s_mounted = iget(sb,SYSV_ROOT_INO);
	unlock_super(sb);
	if (!sb->s_mounted) {
		brelse(bh);
		sb->s_dev = 0;
		printk("SysV FS: get root inode failed\n");
		return NULL;
	}
	sb->s_dirt = 1; /* brelse(bh); occurs when the disk is unmounted. */
	return sb;
}

/* This is only called on sync() and umount(), when s_dirt=1. */
void sysv_write_super (struct super_block *sb)
{
	lock_super(sb);
	if (sb->sv_bh->b_dirt) {
		/* If we are going to write out the super block,
		   then attach current time stamp. */
		unsigned long time = CURRENT_TIME;
		if (sb->sv_convert)
			time = to_coh_ulong(time);
		*sb->sv_sb_time = time;
	}
	sb->s_dirt = 0;
	unlock_super(sb);
}

void sysv_put_super(struct super_block *sb)
{
	/* we can assume sysv_write_super() has already been called */
	lock_super(sb);
	brelse(sb->sv_bh);
	sb->s_dev = 0;
	unlock_super(sb);
}

void sysv_statfs(struct super_block *sb, struct statfs *buf)
{
	long tmp;

	put_fs_long(sb->s_magic, &buf->f_type);		/* type of filesystem */
	put_fs_long(sb->sv_block_size, &buf->f_bsize);	/* block size */
	put_fs_long(sb->sv_ndatazones, &buf->f_blocks);	/* total data blocks in file system */
	tmp = sysv_count_free_blocks(sb);
	put_fs_long(tmp, &buf->f_bfree);		/* free blocks in fs */
	put_fs_long(tmp, &buf->f_bavail);		/* free blocks available to non-superuser */
	put_fs_long(sb->sv_ninodes, &buf->f_files);	/* total file nodes in file system */
	put_fs_long(sysv_count_free_inodes(sb), &buf->f_ffree);	/* free file nodes in fs */
	put_fs_long(SYSV_NAMELEN, &buf->f_namelen);
	/* Don't know what value to put in buf->f_fsid */	/* file system id */
}


/* bmap support for running executables and shared libraries.
   Used only if block_size = BLOCK_SIZE. */

#define IND_PER_BLOCK	(BLOCK_SIZE / sizeof(sysv_zone_t))

static inline int inode_bmap(struct super_block * sb, struct inode * inode, int nr)
{
	int tmp = inode->u.sysv_i.i_data[nr];
	if (!tmp)
		return 0;
	return tmp + sb->sv_block_base;
}

static int block_bmap(struct super_block * sb, struct buffer_head * bh, int nr, int convert)
{
	int tmp;

	if (!bh)
		return 0;
	tmp = ((sysv_zone_t *) bh->b_data) [nr];
	if (convert)
		tmp = from_coh_ulong(tmp);
	brelse(bh);
	if (!tmp)
		return 0;
	return tmp + sb->sv_block_base;
}

int sysv_bmap(struct inode * inode,int block_nr)
{
	unsigned int block = block_nr;
	struct super_block * sb = inode->i_sb;
	int convert;
	int i;
	struct buffer_head * bh;

	if (block < 10)
		return inode_bmap(sb,inode,block);
	block -= 10;
	convert = sb->sv_convert;
	if (block < IND_PER_BLOCK) {
		i = inode_bmap(sb,inode,10);
		if (!i)
			return 0;
		bh = bread(inode->i_dev,i,BLOCK_SIZE);
		return block_bmap(sb,bh,block,convert);
	}
	block -= IND_PER_BLOCK;
	if (block < IND_PER_BLOCK*IND_PER_BLOCK) {
		i = inode_bmap(sb,inode,11);
		if (!i)
			return 0;
		bh = bread(inode->i_dev,i,BLOCK_SIZE);
		i = block_bmap(sb,bh,block/IND_PER_BLOCK,convert);
		if (!i)
			return 0;
		bh = bread(inode->i_dev,i,BLOCK_SIZE);
		return block_bmap(sb,bh,block%IND_PER_BLOCK,convert);
	}
	block -= IND_PER_BLOCK*IND_PER_BLOCK;
	if (block < IND_PER_BLOCK*IND_PER_BLOCK*IND_PER_BLOCK) {
		i = inode_bmap(sb,inode,12);
		if (!i)
			return 0;
		bh = bread(inode->i_dev,i,BLOCK_SIZE);
		i = block_bmap(sb,bh,block/(IND_PER_BLOCK*IND_PER_BLOCK),convert);
		if (!i)
			return 0;
		bh = bread(inode->i_dev,i,BLOCK_SIZE);
		i = block_bmap(sb,bh,(block/IND_PER_BLOCK)%IND_PER_BLOCK,convert);
		if (!i)
			return 0;
		bh = bread(inode->i_dev,i,BLOCK_SIZE);
		return block_bmap(sb,bh,block%IND_PER_BLOCK,convert);
	}
	if ((int)block<0) {
		printk("sysv_bmap: block<0");
		return 0;
	}
	printk("sysv_bmap: block>big");
	return 0;
}

/* End of bmap support. */


/* Access selected blocks of regular files (or directories) */

static struct buffer_head * inode_getblk(struct inode * inode, int nr, int create, char * *start)
{
	struct super_block *sb;
	unsigned long tmp;
	unsigned long *p;
	struct buffer_head * result;

	sb = inode->i_sb;
	p = inode->u.sysv_i.i_data + nr;
repeat:
	tmp = *p;
	if (tmp) {
		result = getblk(inode->i_dev, (tmp >> sb->sv_block_size_ratio_bits) + sb->sv_block_base, BLOCK_SIZE);
		if (tmp == *p) {
			*start = result->b_data + ((tmp & sb->sv_block_size_ratio_1) << sb->sv_block_size_bits);
			return result;
		}
		brelse(result);
		goto repeat;
	}
	if (!create)
		return NULL;
	tmp = sysv_new_block(sb);
	if (!tmp)
		return NULL;
	result = getblk(inode->i_dev, (tmp >> sb->sv_block_size_ratio_bits) + sb->sv_block_base, BLOCK_SIZE);
	if (*p) {
		sysv_free_block(sb,tmp);
		brelse(result);
		goto repeat;
	}
	*p = tmp;
	inode->i_ctime = CURRENT_TIME;
	inode->i_dirt = 1;
	*start = result->b_data + ((tmp & sb->sv_block_size_ratio_1) << sb->sv_block_size_bits);
	return result;
}

static struct buffer_head * block_getblk(struct inode * inode, 
	struct buffer_head * bh, int nr, int create, char * *start)
{
	struct super_block *sb;
	unsigned long tmp, block;
	sysv_zone_t *p;
	struct buffer_head * result;

	if (!bh)
		return NULL;
	if (!bh->b_uptodate) {
		ll_rw_block(READ, 1, &bh);
		wait_on_buffer(bh);
		if (!bh->b_uptodate) {
			brelse(bh);
			return NULL;
		}
	}
	sb = inode->i_sb;
	p = nr + (sysv_zone_t *) *start;
repeat:
	block = tmp = *p;
	if (sb->sv_convert)
		block = from_coh_ulong(block);
	if (tmp) {
		result = getblk(bh->b_dev, (block >> sb->sv_block_size_ratio_bits) + sb->sv_block_base, BLOCK_SIZE);
		if (tmp == *p) {
			brelse(bh);
			*start = result->b_data + ((block & sb->sv_block_size_ratio_1) << sb->sv_block_size_bits);
			return result;
		}
		brelse(result);
		goto repeat;
	}
	if (!create) {
		brelse(bh);
		return NULL;
	}
	block = sysv_new_block(sb);
	if (!block) {
		brelse(bh);
		return NULL;
	}
	result = getblk(bh->b_dev, (block >> sb->sv_block_size_ratio_bits) + sb->sv_block_base, BLOCK_SIZE);
	if (*p) {
		sysv_free_block(sb,block);
		brelse(result);
		goto repeat;
	}
	*p = (sb->sv_convert ? to_coh_ulong(block) : block);
	bh->b_dirt = 1;
	brelse(bh);
	*start = result->b_data + ((block & sb->sv_block_size_ratio_1) << sb->sv_block_size_bits);
	return result;
}

struct buffer_head * sysv_getblk(struct inode * inode, unsigned int block,
	int create, char * *start)
{
	struct super_block * sb = inode->i_sb;
	struct buffer_head * bh;

	if (block < 10)
		return inode_getblk(inode,block,create,start);
	block -= 10;
	if (block < sb->sv_ind_per_block) {
		bh = inode_getblk(inode,10,create,start);
		return block_getblk(inode, bh, block, create, start);
	}
	block -= sb->sv_ind_per_block;
	if (block < sb->sv_ind_per_block_2) {
		bh = inode_getblk(inode,11,create,start);
		bh = block_getblk(inode, bh, block >> sb->sv_ind_per_block_bits, create, start);
		return block_getblk(inode, bh, block & sb->sv_ind_per_block_1, create, start);
	}
	block -= sb->sv_ind_per_block_2;
	if (block < sb->sv_ind_per_block_3) {
		bh = inode_getblk(inode,12,create,start);
		bh = block_getblk(inode, bh, block >> sb->sv_ind_per_block_2_bits, create, start);
		bh = block_getblk(inode, bh, (block >> sb->sv_ind_per_block_bits) & sb->sv_ind_per_block_1, create, start);
		return block_getblk(inode, bh, block & sb->sv_ind_per_block_1, create, start);
	}
	if ((int)block<0) {
		printk("sysv_getblk: block<0");
		return NULL;
	}
	printk("sysv_getblk: block>big");
	return NULL;
}

struct buffer_head * sysv_file_bread(struct inode * inode, int block, int create, char * *start)
{
	struct buffer_head * bh;

	bh = sysv_getblk(inode,block,create,start);
	if (!bh || bh->b_uptodate)
		return bh;
	ll_rw_block(READ, 1, &bh);
	wait_on_buffer(bh);
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return NULL;
}


static inline unsigned long read3byte (char * p)
{
	return (unsigned long)(*(unsigned short *)p)
	     | (unsigned long)(*(unsigned char *)(p+2)) << 16;
}

static inline void write3byte (char * p, unsigned long val)
{
	*(unsigned short *)p = (unsigned short) val;
	*(unsigned char *)(p+2) = val >> 16;
}

static inline unsigned long coh_read3byte (char * p)
{
	return (unsigned long)(*(unsigned char *)p) << 16
	     | (unsigned long)(*(unsigned short *)(p+1));
}

static inline void coh_write3byte (char * p, unsigned long val)
{
	*(unsigned char *)p = val >> 16;
	*(unsigned short *)(p+1) = (unsigned short) val;
}

void sysv_read_inode(struct inode * inode)
{
	struct super_block * sb = inode->i_sb;
	struct buffer_head * bh;
	char * bh_data;
	struct sysv_inode * raw_inode;
	unsigned int block, ino;
	umode_t mode;

	ino = inode->i_ino;
	inode->i_op = NULL;
	inode->i_mode = 0;
	if (!ino || ino > sb->sv_ninodes) {
		printk("Bad inode number on dev 0x%04x: %d is out of range\n",
			inode->i_dev, ino);
		return;
	}
	block = sb->sv_firstinodezone + ((ino-1) >> sb->sv_inodes_per_block_bits);
	if (!(bh=sysv_bread(sb,inode->i_dev,block,&bh_data))) {
		printk("Major problem: unable to read inode from dev 0x%04x\n",
			inode->i_dev);
		return;
	}
	raw_inode = (struct sysv_inode *) bh_data + ((ino-1) & sb->sv_inodes_per_block_1);
	mode = raw_inode->i_mode;
	if (sb->sv_kludge_symlinks)
		mode = from_coh_imode(mode);
	/* SystemV FS: kludge permissions if ino==SYSV_ROOT_INO ?? */
	inode->i_mode = mode;
	inode->i_uid = raw_inode->i_uid;
	inode->i_gid = raw_inode->i_gid;
	inode->i_nlink = raw_inode->i_nlink;
	if (sb->sv_convert) {
		inode->i_size = from_coh_ulong(raw_inode->i_size);
		inode->i_atime = from_coh_ulong(raw_inode->i_atime);
		inode->i_mtime = from_coh_ulong(raw_inode->i_mtime);
		inode->i_ctime = from_coh_ulong(raw_inode->i_ctime);
	} else {
		inode->i_size = raw_inode->i_size;
		inode->i_atime = raw_inode->i_atime;
		inode->i_mtime = raw_inode->i_mtime;
		inode->i_ctime = raw_inode->i_ctime;
	}
	inode->i_blocks = inode->i_blksize = 0;
	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
		inode->i_rdev = raw_inode->i_a.i_rdev;
	else
	if (sb->sv_convert)
		for (block = 0; block < 10+1+1+1; block++)
			inode->u.sysv_i.i_data[block] =
				coh_read3byte(&raw_inode->i_a.i_addb[3*block]);
	else
		for (block = 0; block < 10+1+1+1; block++)
			inode->u.sysv_i.i_data[block] =
				read3byte(&raw_inode->i_a.i_addb[3*block]);
	brelse(bh);
	if (S_ISREG(inode->i_mode))
		if (sb->sv_block_size_ratio_bits == 0) /* block_size == BLOCK_SIZE ? */
			inode->i_op = &sysv_file_inode_operations_with_bmap;
		else
			inode->i_op = &sysv_file_inode_operations;
	else if (S_ISDIR(inode->i_mode))
		inode->i_op = &sysv_dir_inode_operations;
	else if (S_ISLNK(inode->i_mode))
		inode->i_op = &sysv_symlink_inode_operations;
	else if (S_ISCHR(inode->i_mode))
		inode->i_op = &chrdev_inode_operations;
	else if (S_ISBLK(inode->i_mode))
		inode->i_op = &blkdev_inode_operations;
	else if (S_ISFIFO(inode->i_mode))
		init_fifo(inode);
}

/* To avoid inconsistencies between inodes in memory and inodes on disk. */
extern int sysv_notify_change(int flags, struct inode *inode)
{
	if (flags & NOTIFY_MODE)
		if (inode->i_sb->sv_kludge_symlinks)
			if (inode->i_mode == COH_KLUDGE_SYMLINK_MODE) {
				inode->i_mode = COH_KLUDGE_NOT_SYMLINK;
				inode->i_dirt = 1;
			}
	return 0;
}

static struct buffer_head * sysv_update_inode(struct inode * inode)
{
	struct super_block * sb = inode->i_sb;
	struct buffer_head * bh;
	char * bh_data;
	struct sysv_inode * raw_inode;
	unsigned int ino, block;
	umode_t mode;

	ino = inode->i_ino;
	if (!ino || ino > sb->sv_ninodes) {
		printk("Bad inode number on dev 0x%04x: %d is out of range\n",
			inode->i_dev, ino);
		inode->i_dirt = 0;
		return 0;
	}
	block = sb->sv_firstinodezone + ((ino-1) >> sb->sv_inodes_per_block_bits);
	if (!(bh = sysv_bread(sb,inode->i_dev,block,&bh_data))) {
		printk("unable to read i-node block\n");
		inode->i_dirt = 0;
		return 0;
	}
	raw_inode = (struct sysv_inode *) bh_data + ((ino-1) & sb->sv_inodes_per_block_1);
	mode = inode->i_mode;
	if (sb->sv_kludge_symlinks)
		mode = to_coh_imode(mode);
	raw_inode->i_mode = mode;
	raw_inode->i_uid = inode->i_uid;
	raw_inode->i_gid = inode->i_gid;
	raw_inode->i_nlink = inode->i_nlink;
	if (sb->sv_convert) {
		raw_inode->i_size = to_coh_ulong(inode->i_size);
		raw_inode->i_atime = to_coh_ulong(inode->i_atime);
		raw_inode->i_mtime = to_coh_ulong(inode->i_mtime);
		raw_inode->i_ctime = to_coh_ulong(inode->i_ctime);
	} else {
		raw_inode->i_size = inode->i_size;
		raw_inode->i_atime = inode->i_atime;
		raw_inode->i_mtime = inode->i_mtime;
		raw_inode->i_ctime = inode->i_ctime;
	}
	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
		raw_inode->i_a.i_rdev = inode->i_rdev; /* write 2 or 3 bytes ?? */
	else
	if (sb->sv_convert)
		for (block = 0; block < 10+1+1+1; block++)
			coh_write3byte(&raw_inode->i_a.i_addb[3*block],inode->u.sysv_i.i_data[block]);
	else
		for (block = 0; block < 10+1+1+1; block++)
			write3byte(&raw_inode->i_a.i_addb[3*block],inode->u.sysv_i.i_data[block]);
	inode->i_dirt=0;
	bh->b_dirt=1;
	return bh;
}

void sysv_write_inode(struct inode * inode)
{
	struct buffer_head *bh;
	bh = sysv_update_inode(inode);
	brelse(bh);
}

int sysv_sync_inode(struct inode * inode)
{
        int err = 0;
        struct buffer_head *bh;

        bh = sysv_update_inode(inode);
        if (bh && bh->b_dirt) {
                ll_rw_block(WRITE, 1, &bh);
                wait_on_buffer(bh);
                if (bh->b_req && !bh->b_uptodate)
                {
                        printk ("IO error syncing sysv inode [%04x:%08lx]\n",
                                inode->i_dev, inode->i_ino);
                        err = -1;
                }
        }
        else if (!bh)
                err = -1;
        brelse (bh);
        return err;
}

