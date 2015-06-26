/*
 *  linux/fs/sysv/ialloc.c
 *
 *  minix/bitmap.c
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext/freelists.c
 *  Copyright (C) 1992  Remy Card (card@masi.ibp.fr)
 *
 *  xenix/alloc.c
 *  Copyright (C) 1992  Doug Evans
 *
 *  coh/alloc.c
 *  Copyright (C) 1993  Pascal Haible, Bruno Haible
 *
 *  sysv/ialloc.c
 *  Copyright (C) 1993  Bruno Haible
 *
 *  This file contains code for allocating/freeing inodes.
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/sysv_fs.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/locks.h>

/* We don't trust the value of
   sb->sv_sbd->s_tinode = *sb->sv_sb_total_free_inodes
   but we nevertheless keep it up to date. */

/* An inode on disk is considered free if both i_mode == 0 and i_nlink == 0. */

void sysv_free_inode(struct inode * inode)
{
	struct super_block * sb;
	unsigned int ino;
	struct buffer_head * bh;
	char * bh_data;
	struct sysv_inode * raw_inode;

	if (!inode)
		return;
	if (!inode->i_dev) {
		printk("sysv_free_inode: inode has no device\n");
		return;
	}
	if (inode->i_count != 1) {
		printk("sysv_free_inode: inode has count=%d\n", inode->i_count);
		return;
	}
	if (inode->i_nlink) {
		printk("sysv_free_inode: inode has nlink=%d\n", inode->i_nlink);
		return;
	}
	if (!(sb = inode->i_sb)) {
		printk("sysv_free_inode: inode on nonexistent device\n");
		return;
	}
	ino = inode->i_ino;
	if (ino <= SYSV_ROOT_INO || ino > sb->sv_ninodes) {
		printk("sysv_free_inode: inode 0,1,2 or nonexistent inode\n");
		return;
	}
	if (!(bh = sysv_bread(sb, inode->i_dev, sb->sv_firstinodezone + ((ino-1) >> sb->sv_inodes_per_block_bits), &bh_data))) {
		printk("sysv_free_inode: unable to read inode block on device %d/%d\n",MAJOR(inode->i_dev),MINOR(inode->i_dev));
		clear_inode(inode);
		return;
	}
	raw_inode = (struct sysv_inode *) bh_data + ((ino-1) & sb->sv_inodes_per_block_1);
	lock_super(sb);
	if (*sb->sv_sb_fic_count < sb->sv_fic_size)
		sb->sv_sb_fic_inodes[(*sb->sv_sb_fic_count)++] = ino;
	(*sb->sv_sb_total_free_inodes)++;
	sb->sv_bh->b_dirt = 1; /* super-block has been modified */
	sb->s_dirt = 1; /* and needs time stamp */
	memset(raw_inode, 0, sizeof(struct sysv_inode));
	bh->b_dirt = 1;
	unlock_super(sb);
	brelse(bh);
	clear_inode(inode);
}

struct inode * sysv_new_inode(const struct inode * dir)
{
	struct inode * inode;
	struct super_block * sb;
	struct buffer_head * bh;
	char * bh_data;
	struct sysv_inode * raw_inode;
	int i,j,ino,block;

	if (!dir || !(inode = get_empty_inode()))
		return NULL;
	sb = dir->i_sb;
	inode->i_sb = sb;
	inode->i_flags = inode->i_sb->s_flags;
	lock_super(sb);		/* protect against task switches */
	if ((*sb->sv_sb_fic_count == 0)
	    || (sb->sv_sb_fic_inodes[(*sb->sv_sb_fic_count)-1] == 0) /* Applies only to SystemV2 FS */
	   ) {
		/* Rebuild cache of free inodes: */
		/* i : index into cache slot being filled	     */
		/* ino : inode we are trying			     */
		/* block : firstinodezone + (ino-1)/inodes_per_block */
		/* j : (ino-1)%inodes_per_block			     */
		/* bh : buffer for block			     */
		/* raw_inode : pointer to inode ino in the block     */
		for (i = 0, ino = SYSV_ROOT_INO+1, block = sb->sv_firstinodezone, j = SYSV_ROOT_INO ; i < sb->sv_fic_size && block < sb->sv_firstdatazone ; block++, j = 0) {
			if (!(bh = sysv_bread(sb, sb->s_dev, block, &bh_data))) {
				printk("sysv_new_inode: unable to read inode table\n");
				break;	/* go with what we've got */
				/* FIXME: Perhaps try the next block? */
			}
			raw_inode = (struct sysv_inode *) bh_data + j;
			for (; j < sb->sv_inodes_per_block && i < sb->sv_fic_size; ino++, j++, raw_inode++) {
				if (raw_inode->i_mode == 0 && raw_inode->i_nlink == 0)
					sb->sv_sb_fic_inodes[i++] = ino;
			}
			brelse(bh);
		}
		if (i == 0) {
			iput(inode);
			unlock_super(sb);
			return NULL;	/* no inodes available */
		}
		*sb->sv_sb_fic_count = i;
	}
	/* Now *sb->sv_sb_fic_count > 0. */
	ino = sb->sv_sb_fic_inodes[--(*sb->sv_sb_fic_count)];
	sb->sv_bh->b_dirt = 1; /* super-block has been modified */
	sb->s_dirt = 1; /* and needs time stamp */
	inode->i_count = 1;
	inode->i_nlink = 1;
	inode->i_dev = sb->s_dev;
	inode->i_uid = current->euid;
	inode->i_gid = (dir->i_mode & S_ISGID) ? dir->i_gid : current->egid;
	inode->i_dirt = 1;
	inode->i_ino = ino;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	inode->i_op = NULL;
	inode->i_blocks = inode->i_blksize = 0;
	inode->u.sysv_i.i_lock = 0; inode->u.sysv_i.i_wait = NULL;
	insert_inode_hash(inode);
	/* Change directory entry: */
	inode->i_mode = 0;		/* for sysv_write_inode() */
	inode->i_size = 0;		/* ditto */
	sysv_write_inode(inode);	/* ensure inode not allocated again */
					/* FIXME: caller may call this too. */
	inode->i_dirt = 1;		/* cleared by sysv_write_inode() */
	/* That's it. */
	(*sb->sv_sb_total_free_inodes)--;
	sb->sv_bh->b_dirt = 1; /* super-block has been modified again */
	sb->s_dirt = 1; /* and needs time stamp again */
	unlock_super(sb);
	return inode;
}

unsigned long sysv_count_free_inodes(struct super_block * sb)
{
#if 1 /* test */
	struct buffer_head * bh;
	char * bh_data;
	struct sysv_inode * raw_inode;
	int j,block,count;

	/* this causes a lot of disk traffic ... */
	count = 0;
	lock_super(sb);
	/* i : index into cache slot being filled	     */
	/* ino : inode we are trying			     */
	/* block : firstinodezone + (ino-1)/inodes_per_block */
	/* j : (ino-1)%inodes_per_block			     */
	/* bh : buffer for block			     */
	/* raw_inode : pointer to inode ino in the block     */
	for (block = sb->sv_firstinodezone, j = SYSV_ROOT_INO ; block < sb->sv_firstdatazone ; block++, j = 0) {
		if (!(bh = sysv_bread(sb, sb->s_dev, block, &bh_data))) {
			printk("sysv_count_free_inodes: unable to read inode table\n");
			break;	/* go with what we've got */
			/* FIXME: Perhaps try the next block? */
		}
		raw_inode = (struct sysv_inode *) bh_data + j;
		for (; j < sb->sv_inodes_per_block ; j++, raw_inode++)
			if (raw_inode->i_mode == 0 && raw_inode->i_nlink == 0)
				count++;
		brelse(bh);
	}
	if (count != *sb->sv_sb_total_free_inodes) {
		printk("sysv_count_free_inodes: free inode count was %d, correcting to %d\n",(short)(*sb->sv_sb_total_free_inodes),count);
		if (!(sb->s_flags & MS_RDONLY)) {
			*sb->sv_sb_total_free_inodes = count;
			sb->sv_bh->b_dirt = 1; /* super-block has been modified */
			sb->s_dirt = 1; /* and needs time stamp */
		}
	}
	unlock_super(sb);
	return count;
#else
	return *sb->sv_sb_total_free_inodes;
#endif
}

