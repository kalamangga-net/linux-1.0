/*
 *  linux/fs/sysv/truncate.c
 *
 *  minix/truncate.c
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  coh/truncate.c
 *  Copyright (C) 1993  Pascal Haible, Bruno Haible
 *
 *  sysv/truncate.c
 *  Copyright (C) 1993  Bruno Haible
 */

#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/sysv_fs.h>
#include <linux/stat.h>


/* There are two different implementations of truncate() here.
 * One (by Bruno) needs to do locking to ensure that noone is writing
 * to a block being truncated away and incorporated into the free list.
 * The better one (by Linus) doesn't need locking because it can tell from
 * looking at bh->b_count whether a given block is in use elsewhere.
 * Alas, this doesn't work if block_size < BLOCK_SIZE.
 */


/* Bruno's implementation of truncate. */

/* Leave at most `blocks' direct blocks. */
static int coh_trunc_direct (struct inode * inode, unsigned long blocks)
{
	unsigned int i;
	unsigned long * p;
	unsigned long block;

	for (i = blocks; i < 10 ; i++) {
		p = &inode->u.sysv_i.i_data[i];
		block = *p;
		if (!block)
			continue;
		*p = 0;
		inode->i_dirt = 1;
		sysv_free_block(inode->i_sb,block);
	}
	return 0;
}

/* Leave at most `blocks' blocks out of an indirect block whose number is
 * from_coh_ulong(*p) if convert=1, *p if convert=0.
 */
static int coh_trunc_indirect (struct inode * inode, unsigned long blocks, unsigned long * p, int convert, unsigned char * dirt)
{
	struct super_block * sb = inode->i_sb;
	unsigned long tmp, block, indblock;
	struct buffer_head * bh;
	char * bh_data;
	unsigned long i;
	sysv_zone_t * ind;

	if (blocks >= sb->sv_ind_per_block)
		return 0;
	block = tmp = *p;
	if (convert)
		block = from_coh_ulong(block);
	if (!block)
		return 0;
	bh = sysv_bread(sb,inode->i_dev,block,&bh_data);
	if (tmp != *p) {
		brelse(bh);
		return 1;
	}
	if (!bh) {
		*p = 0;
		*dirt = 1;
		return 0;
	}
	for (i = blocks; i < sb->sv_ind_per_block; i++) {
		ind = &((sysv_zone_t *) bh_data)[i];
		indblock = *ind;
		if (sb->sv_convert)
			indblock = from_coh_ulong(indblock);
		if (!indblock)
			continue;
		*ind = 0;
		bh->b_dirt = 1;
		sysv_free_block(sb,indblock);
	}
	for (i = 0; i < sb->sv_ind_per_block; i++)
		if (((sysv_zone_t *) bh_data)[i])
			goto done;
	if (tmp != *p) {
		brelse(bh);
		return 1;
	}
	*p = 0;
	*dirt = 1;
	sysv_free_block(sb,block);
done:
	brelse(bh);
	return 0;
}

/* Leave at most `blocks' blocks out of an double indirect block whose number is
 * from_coh_ulong(*p) if convert=1, *p if convert=0.
 */
static int coh_trunc_dindirect (struct inode * inode, unsigned long blocks, unsigned long * p, int convert, unsigned char * dirt)
{
	struct super_block * sb = inode->i_sb;
	unsigned long tmp, block, dindblock;
	struct buffer_head * bh;
	char * bh_data;
	unsigned long i, j;
	sysv_zone_t * dind;
	int retry = 0;

	if (blocks >= sb->sv_ind_per_block_2)
		return 0;
	block = tmp = *p;
	if (convert)
		block = from_coh_ulong(block);
	if (!block)
		return 0;
	bh = sysv_bread(sb,inode->i_dev,block,&bh_data);
	if (tmp != *p) {
		brelse(bh);
		return 1;
	}
	if (!bh) {
		*p = 0;
		*dirt = 1;
		return 0;
	}
	for (i = blocks >> sb->sv_ind_per_block_bits, j = blocks & sb->sv_ind_per_block_1;
	     i < sb->sv_ind_per_block;
	     i++, j = 0) {
		/* j = max(blocks-i*ind_per_block,0) */
		dind = &((sysv_zone_t *) bh_data)[i];
		dindblock = *dind;
		if (sb->sv_convert)
			dindblock = from_coh_ulong(dindblock);
		if (!dindblock)
			continue;
		retry |= coh_trunc_indirect(inode,j,dind,sb->sv_convert,&bh->b_dirt);
	}
	for (i = 0; i < sb->sv_ind_per_block; i++)
		if (((sysv_zone_t *) bh_data)[i])
			goto done;
	if (tmp != *p) {
		brelse(bh);
		return 1;
	}
	*p = 0;
	*dirt = 1;
	sysv_free_block(sb,block);
done:
	brelse(bh);
	return retry;
}

/* Leave at most `blocks' blocks out of an triple indirect block whose number is
 * from_coh_ulong(*p) if convert=1, *p if convert=0.
 */
static int coh_trunc_tindirect (struct inode * inode, unsigned long blocks, unsigned long * p)
{
	struct super_block * sb = inode->i_sb;
	unsigned long block, tindblock;
	struct buffer_head * bh;
	char * bh_data;
	unsigned long i, j;
	sysv_zone_t * tind;
	int retry = 0;

	if (blocks >= sb->sv_ind_per_block_3)
		return 0;
	block = *p;
	if (!block)
		return 0;
	bh = sysv_bread(sb,inode->i_dev,block,&bh_data);
	if (block != *p) {
		brelse(bh);
		return 1;
	}
	if (!bh) {
		*p = 0;
		inode->i_dirt = 1;
		return 0;
	}
	for (i = blocks >> sb->sv_ind_per_block_2_bits, j = blocks & sb->sv_ind_per_block_2_1;
	     i < sb->sv_ind_per_block;
	     i++, j = 0) {
		/* j = max(blocks-i*ind_per_block^2,0) */
		tind = &((sysv_zone_t *) bh_data)[i];
		tindblock = *tind;
		if (sb->sv_convert)
			tindblock = from_coh_ulong(tindblock);
		if (!tindblock)
			continue;
		retry |= coh_trunc_dindirect(inode,j,tind,sb->sv_convert,&bh->b_dirt);
	}
	for (i = 0; i < sb->sv_ind_per_block; i++)
		if (((sysv_zone_t *) bh_data)[i])
			goto done;
	if (block != *p) {
		brelse(bh);
		return 1;
	}
	*p = 0;
	inode->i_dirt = 1;
	sysv_free_block(sb,block);
done:
	brelse(bh);
	return retry;
}

static int coh_trunc_all(struct inode * inode)
{
	struct super_block * sb = inode->i_sb;
	long blocks;
	int retry;

	blocks = (inode->i_size + sb->sv_block_size_1) >> sb->sv_block_size_bits;
	retry = coh_trunc_direct(inode,blocks);
	blocks -= 10;
	if (blocks < 0) blocks = 0;
	retry |= coh_trunc_indirect(inode,blocks,&inode->u.sysv_i.i_data[10],0,&inode->i_dirt);
	blocks -= sb->sv_ind_per_block;
	if (blocks < 0) blocks = 0;
	retry |= coh_trunc_dindirect(inode,blocks,&inode->u.sysv_i.i_data[11],0,&inode->i_dirt);
	blocks -= sb->sv_ind_per_block_2;
	if (blocks < 0) blocks = 0;
	retry |= coh_trunc_tindirect(inode,blocks,&inode->u.sysv_i.i_data[12]);
	return retry;
}


/* Linus' implementation of truncate. Used only if block_size = BLOCK_SIZE. */

/*
 * Truncate has the most races in the whole filesystem: coding it is
 * a pain in the a**. Especially as I don't do any locking...
 *
 * The code may look a bit weird, but that's just because I've tried to
 * handle things like file-size changes in a somewhat graceful manner.
 * Anyway, truncating a file at the same time somebody else writes to it
 * is likely to result in pretty weird behaviour...
 *
 * The new code handles normal truncates (size = 0) as well as the more
 * general case (size = XXX). I hope.
 */

/* We throw away any data beyond inode->i_size. */

static int trunc_direct(struct inode * inode)
{
	struct super_block * sb;
	unsigned int i;
	unsigned long * p;
	unsigned long block;
	struct buffer_head * bh;
	int retry = 0;

	sb = inode->i_sb;
repeat:
	for (i = ((unsigned long) inode->i_size + BLOCK_SIZE-1) / BLOCK_SIZE; i < 10; i++) {
		p = inode->u.sysv_i.i_data + i;
		block = *p;
		if (!block)
			continue;
		bh = get_hash_table(inode->i_dev,block+sb->sv_block_base,BLOCK_SIZE);
		if (i*BLOCK_SIZE < inode->i_size) {
			brelse(bh);
			goto repeat;
		}
		if ((bh && bh->b_count != 1) || (block != *p)) {
			retry = 1;
			brelse(bh);
			continue;
		}
		*p = 0;
		inode->i_dirt = 1;
		brelse(bh);
		sysv_free_block(sb,block);
	}
	return retry;
}

#define IND_PER_BLOCK   (BLOCK_SIZE / sizeof(sysv_zone_t))

static int trunc_indirect(struct inode * inode, unsigned long offset, unsigned long * p, int convert, unsigned char * dirt)
{
	unsigned long indtmp, indblock;
	struct super_block * sb;
	struct buffer_head * indbh;
	unsigned int i;
	sysv_zone_t * ind;
	unsigned long tmp, block;
	struct buffer_head * bh;
	int retry = 0;

	indblock = indtmp = *p;
	if (convert)
		indblock = from_coh_ulong(indblock);
	if (!indblock)
		return 0;
	sb = inode->i_sb;
	indbh = bread(inode->i_dev,indblock+sb->sv_block_base,BLOCK_SIZE);
	if (indtmp != *p) {
		brelse(indbh);
		return 1;
	}
	if (!indbh) {
		*p = 0;
		*dirt = 1;
		return 0;
	}
repeat:
	if (inode->i_size < offset)
		i = 0;
	else
		i = (inode->i_size - offset + BLOCK_SIZE-1) / BLOCK_SIZE;
	for (; i < IND_PER_BLOCK; i++) {
		ind = ((sysv_zone_t *) indbh->b_data) + i;
		block = tmp = *ind;
		if (sb->sv_convert)
			block = from_coh_ulong(block);
		if (!block)
			continue;
		bh = get_hash_table(inode->i_dev,block+sb->sv_block_base,BLOCK_SIZE);
		if (i*BLOCK_SIZE + offset < inode->i_size) {
			brelse(bh);
			goto repeat;
		}
		if ((bh && bh->b_count != 1) || (tmp != *ind)) {
			retry = 1;
			brelse(bh);
			continue;
		}
		*ind = 0;
		indbh->b_dirt = 1;
		brelse(bh);
		sysv_free_block(sb,block);
	}
	for (i = 0; i < IND_PER_BLOCK; i++)
		if (((sysv_zone_t *) indbh->b_data)[i])
			goto done;
	if ((indbh->b_count != 1) || (indtmp != *p)) {
		brelse(indbh);
		return 1;
	}
	*p = 0;
	*dirt = 1;
	sysv_free_block(sb,indblock);
done:
	brelse(indbh);
	return retry;
}

static int trunc_dindirect(struct inode * inode, unsigned long offset, unsigned long * p, int convert, unsigned char * dirt)
{
	unsigned long indtmp, indblock;
	struct super_block * sb;
	struct buffer_head * indbh;
	unsigned int i;
	sysv_zone_t * ind;
	unsigned long tmp, block;
	int retry = 0;

	indblock = indtmp = *p;
	if (convert)
		indblock = from_coh_ulong(indblock);
	if (!indblock)
		return 0;
	sb = inode->i_sb;
	indbh = bread(inode->i_dev,indblock+sb->sv_block_base,BLOCK_SIZE);
	if (indtmp != *p) {
		brelse(indbh);
		return 1;
	}
	if (!indbh) {
		*p = 0;
		*dirt = 1;
		return 0;
	}
	if (inode->i_size < offset)
		i = 0;
	else
		i = (inode->i_size - offset + IND_PER_BLOCK*BLOCK_SIZE-1) / (IND_PER_BLOCK*BLOCK_SIZE);
	for (; i < IND_PER_BLOCK; i++) {
		ind = ((sysv_zone_t *) indbh->b_data) + i;
		block = tmp = *ind;
		if (sb->sv_convert)
			block = from_coh_ulong(block);
		if (!block)
			continue;
		retry |= trunc_indirect(inode,offset+i*IND_PER_BLOCK,ind,sb->sv_convert,&indbh->b_dirt);
	}
	for (i = 0; i < IND_PER_BLOCK; i++)
		if (((sysv_zone_t *) indbh->b_data)[i])
			goto done;
	if ((indbh->b_count != 1) || (indtmp != *p)) {
		brelse(indbh);
		return 1;
	}
	*p = 0;
	*dirt = 1;
	sysv_free_block(sb,indblock);
done:
	brelse(indbh);
	return retry;
}

static int trunc_tindirect(struct inode * inode, unsigned long offset, unsigned long * p, int convert, unsigned char * dirt)
{
	unsigned long indtmp, indblock;
	struct super_block * sb;
	struct buffer_head * indbh;
	unsigned int i;
	sysv_zone_t * ind;
	unsigned long tmp, block;
	int retry = 0;

	indblock = indtmp = *p;
	if (convert)
		indblock = from_coh_ulong(indblock);
	if (!indblock)
		return 0;
	sb = inode->i_sb;
	indbh = bread(inode->i_dev,indblock+sb->sv_block_base,BLOCK_SIZE);
	if (indtmp != *p) {
		brelse(indbh);
		return 1;
	}
	if (!indbh) {
		*p = 0;
		*dirt = 1;
		return 0;
	}
	if (inode->i_size < offset)
		i = 0;
	else
		i = (inode->i_size - offset + IND_PER_BLOCK*IND_PER_BLOCK*BLOCK_SIZE-1) / (IND_PER_BLOCK*IND_PER_BLOCK*BLOCK_SIZE);
	for (; i < IND_PER_BLOCK; i++) {
		ind = ((sysv_zone_t *) indbh->b_data) + i;
		block = tmp = *ind;
		if (sb->sv_convert)
			block = from_coh_ulong(block);
		if (!block)
			continue;
		retry |= trunc_dindirect(inode,offset+i*IND_PER_BLOCK*IND_PER_BLOCK,ind,sb->sv_convert,&indbh->b_dirt);
	}
	for (i = 0; i < IND_PER_BLOCK; i++)
		if (((sysv_zone_t *) indbh->b_data)[i])
			goto done;
	if ((indbh->b_count != 1) || (indtmp != *p)) {
		brelse(indbh);
		return 1;
	}
	*p = 0;
	*dirt = 1;
	sysv_free_block(sb,indblock);
done:
	brelse(indbh);
	return retry;
}

static int trunc_all(struct inode * inode)
{
	return trunc_direct(inode)
	     | trunc_indirect(inode,10*BLOCK_SIZE,&inode->u.sysv_i.i_data[10],0,&inode->i_dirt)
	     | trunc_dindirect(inode,(10+IND_PER_BLOCK)*BLOCK_SIZE,&inode->u.sysv_i.i_data[11],0,&inode->i_dirt)
	     | trunc_tindirect(inode,(10+IND_PER_BLOCK+IND_PER_BLOCK*IND_PER_BLOCK)*BLOCK_SIZE,&inode->u.sysv_i.i_data[12],0,&inode->i_dirt);
}


void sysv_truncate(struct inode * inode)
{
	/* If this is called from sysv_put_inode, we needn't worry about
	 * races as we are just losing the last reference to the inode.
	 * If this is called from another place, let's hope it's a regular
	 * file.
	 * Truncating symbolic links is strange. We assume we don't truncate
	 * a directory we are just modifying. We ensure we don't truncate
	 * a regular file we are just writing to, by use of a lock.
	 */
	if (S_ISLNK(inode->i_mode))
		printk("sysv_truncate: truncating symbolic link\n");
	else if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode)))
		return;
	if (inode->i_sb->sv_block_size_ratio_bits > 0) { /* block_size < BLOCK_SIZE ? */
		coh_lock_inode(inode); /* do not write to the inode while we truncate */
		while (coh_trunc_all(inode)) {
			current->counter = 0;
			schedule();
		}
		inode->i_mtime = inode->i_ctime = CURRENT_TIME;
		inode->i_dirt = 1;
		coh_unlock_inode(inode);
	} else {
		while (trunc_all(inode)) {
			current->counter = 0;
			schedule();
		}
		inode->i_mtime = inode->i_ctime = CURRENT_TIME;
		inode->i_dirt = 1;
	}
}
