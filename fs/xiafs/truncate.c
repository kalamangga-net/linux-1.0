/*
 *  linux/fs/xiafs/truncate.c
 *
 *  Copyright (C) Q. Frank Xia, 1993.
 *  
 *  Based on Linus' minix/truncate.c
 *  Copyright (C) Linus Torvalds, 1991, 1992.
 *
 *  This software may be redistributed per Linux Copyright.
 */

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/xia_fs.h>
#include <linux/stat.h>
#include <linux/fcntl.h>

#include "xiafs_mac.h"

/*
 * Linus' comment:
 *
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

#define DT_ZONE		((inode->i_size + XIAFS_ZSIZE(inode->i_sb) - 1) \
			 >> XIAFS_ZSIZE_BITS(inode->i_sb) )

static int trunc_direct(struct inode * inode)
{
    u_long * lp;
    struct buffer_head * bh;
    int i, tmp;
    int retry = 0;

repeat:
    for (i = DT_ZONE ; i < 8 ; i++) {
        if (i < DT_ZONE)
	    goto repeat;
        lp=i + inode->u.xiafs_i.i_zone;
        if (!(tmp = *lp))
	    continue;
	bh = getblk(inode->i_dev, tmp, XIAFS_ZSIZE(inode->i_sb));
	if (i < DT_ZONE) {
	    brelse(bh);
	    goto repeat;
	}
	if ((bh && bh->b_count != 1) || tmp != *lp)
	    retry = 1;
	else {
	    *lp = 0;
	    inode->i_dirt = 1;
	    inode->i_blocks-=2 << XIAFS_ZSHIFT(inode->i_sb);
	    xiafs_free_zone(inode->i_sb, tmp);
	}
	brelse(bh);
    }
    return retry;
}

static int trunc_indirect(struct inode * inode, int addr_off, u_long * lp)
{

#define INDT_ZONE 	(DT_ZONE - addr_off)

    struct buffer_head * bh, * ind_bh;
    int i, tmp;
    u_long * indp;
    int retry = 0;

    if ( !(tmp=*lp) )
        return 0;
    ind_bh = bread(inode->i_dev, tmp, XIAFS_ZSIZE(inode->i_sb));
    if (tmp != *lp) {
        brelse(ind_bh);
	return 1;
    }
    if (!ind_bh) {
        *lp = 0;
	return 0;
    }
repeat:
    for (i = INDT_ZONE<0?0:INDT_ZONE; i < XIAFS_ADDRS_PER_Z(inode->i_sb); i++) {
        if (i < INDT_ZONE)
	    goto repeat;
        indp = i+(u_long *) ind_bh->b_data;
	if (!(tmp=*indp))
	    continue;
	bh = getblk(inode->i_dev, tmp, XIAFS_ZSIZE(inode->i_sb));
	if (i < INDT_ZONE) {
	    brelse(bh);
	    goto repeat;
	}
	if ((bh && bh->b_count != 1) || tmp != *indp)
	    retry = 1;
	else {
	    *indp = 0;
	    ind_bh->b_dirt = 1;
	    inode->i_blocks-= 2 << XIAFS_ZSHIFT(inode->i_sb);
	    xiafs_free_zone(inode->i_sb, tmp);
	}
	brelse(bh);
    }
    indp = (u_long *) ind_bh->b_data;
    for (i = 0; i < XIAFS_ADDRS_PER_Z(inode->i_sb) && !(*indp++); i++) ;
    if (i >= XIAFS_ADDRS_PER_Z(inode->i_sb)) {
      if (ind_bh->b_count != 1)
	   retry = 1;
      else {
	  tmp = *lp;
	  *lp = 0;
	  inode->i_blocks-= 2 << XIAFS_ZSHIFT(inode->i_sb);
	  xiafs_free_zone(inode->i_sb, tmp);
      }
    }
    brelse(ind_bh);
    return retry;
}
		
static int trunc_dindirect(struct inode * inode)
{

#define DINDT_ZONE \
    ((DT_ZONE-XIAFS_ADDRS_PER_Z(inode->i_sb)-8)>>XIAFS_ADDRS_PER_Z_BITS(inode->i_sb))

    int i, tmp;
    struct buffer_head * dind_bh;
    u_long * dindp, * lp;
    int retry = 0;

    lp = &(inode->u.xiafs_i.i_dind_zone);
    if (!(tmp = *lp))
        return 0;
    dind_bh = bread(inode->i_dev, tmp, XIAFS_ZSIZE(inode->i_sb));
    if (tmp != *lp) {
        brelse(dind_bh);
	return 1;
    }
    if (!dind_bh) {
        *lp = 0;
	return 0;
    }
repeat:
    for (i=DINDT_ZONE<0?0:DINDT_ZONE ; i < XIAFS_ADDRS_PER_Z(inode->i_sb) ; i ++) {
        if (i < DINDT_ZONE)
	    goto repeat;
        dindp = i+(u_long *) dind_bh->b_data;
	retry |= trunc_indirect(inode, 
				8+((1+i)<<XIAFS_ADDRS_PER_Z_BITS(inode->i_sb)), 
				dindp);
	dind_bh->b_dirt = 1;
    }
    dindp = (u_long *) dind_bh->b_data;
    for (i = 0; i < XIAFS_ADDRS_PER_Z(inode->i_sb) && !(*dindp++); i++);
    if (i >= XIAFS_ADDRS_PER_Z(inode->i_sb)) {
        if (dind_bh->b_count != 1)
	    retry = 1;
	else {
	    tmp = *lp;
	    *lp = 0;
	    inode->i_dirt = 1;
	    inode->i_blocks-=2 << XIAFS_ZSHIFT(inode->i_sb);
	    xiafs_free_zone(inode->i_sb, tmp);
	}
    }
    brelse(dind_bh);
    return retry;
}

void xiafs_truncate(struct inode * inode)
{
    int retry;

    if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
	  S_ISLNK(inode->i_mode)))
        return;
    while (1) {
        retry = trunc_direct(inode);
        retry |= trunc_indirect(inode, 8, &(inode->u.xiafs_i.i_ind_zone)); 
        retry |= trunc_dindirect(inode);
	if (!retry)
	    break;
	current->counter = 0;
	schedule();
    }
    inode->i_ctime = inode->i_mtime = CURRENT_TIME;
    inode->i_dirt = 1;
}
