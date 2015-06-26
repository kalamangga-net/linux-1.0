/*
 *  linux/fs/xiafs/inode.c
 *
 *  Copyright (C) Q. Frank Xia, 1993.
 *  
 *  Based on Linus' minix/inode.c
 *  Copyright (C) Linus Torvalds, 1991, 1992.
 *
 *  This software may be redistributed per Linux Copyright.
 */

#include <linux/sched.h>
#include <linux/xia_fs.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/locks.h>
#include <asm/system.h>
#include <asm/segment.h>

#include "xiafs_mac.h"

static u_long random_nr;
  
void xiafs_put_inode(struct inode *inode)
{
    if (inode->i_nlink)
        return;
    inode->i_size = 0;
    xiafs_truncate(inode);
    xiafs_free_inode(inode);
}

void xiafs_put_super(struct super_block *sb)
{
    int i;

    lock_super(sb);
    sb->s_dev = 0;
    for(i = 0 ; i < _XIAFS_IMAP_SLOTS ; i++)
        brelse(sb->u.xiafs_sb.s_imap_buf[i]);
    for(i = 0 ; i < _XIAFS_ZMAP_SLOTS ; i++)
        brelse(sb->u.xiafs_sb.s_zmap_buf[i]);
    unlock_super(sb);
}

static struct super_operations xiafs_sops = { 
    xiafs_read_inode,
    NULL,
    xiafs_write_inode,
    xiafs_put_inode,
    xiafs_put_super,
    NULL,
    xiafs_statfs,
    NULL
};

struct super_block *xiafs_read_super(struct super_block *s, void *data,
				     int silent)
{
    struct buffer_head *bh;
    struct xiafs_super_block *sp;
    int i, z, dev;

    dev=s->s_dev;
    lock_super(s);

    set_blocksize(dev, BLOCK_SIZE);

    if (!(bh = bread(dev, 0, BLOCK_SIZE))) {
        s->s_dev=0;
	unlock_super(s);
	printk("XIA-FS: read super_block failed (%s %d)\n", WHERE_ERR);
	return NULL;
    }
    sp = (struct xiafs_super_block *) bh->b_data;
    s->s_magic = sp->s_magic;
    if (s->s_magic != _XIAFS_SUPER_MAGIC) {
        s->s_dev = 0;
	unlock_super(s);
	brelse(bh);
	if (!silent)
		printk("VFS: Can't find a xiafs filesystem on dev 0x%04x.\n",
		   dev);
	return NULL;
    }
    s->s_blocksize = sp->s_zone_size;
    s->s_blocksize_bits = 10 + sp->s_zone_shift;
    if (s->s_blocksize != BLOCK_SIZE && 
	(s->s_blocksize == 1024 || s->s_blocksize == 2048 ||  
	 s->s_blocksize == 4096)) {
      brelse(bh);
      set_blocksize(dev, s->s_blocksize);
      bh = bread (dev, 0,  s->s_blocksize);
      if(!bh) return NULL;
      sp = (struct xiafs_super_block *) (((char *)bh->b_data) + BLOCK_SIZE) ;
    };
    s->u.xiafs_sb.s_nzones = sp->s_nzones;
    s->u.xiafs_sb.s_ninodes = sp->s_ninodes;
    s->u.xiafs_sb.s_ndatazones = sp->s_ndatazones;
    s->u.xiafs_sb.s_imap_zones = sp->s_imap_zones;
    s->u.xiafs_sb.s_zmap_zones = sp->s_zmap_zones;
    s->u.xiafs_sb.s_firstdatazone = sp->s_firstdatazone;
    s->u.xiafs_sb.s_zone_shift = sp->s_zone_shift;
    s->u.xiafs_sb.s_max_size = sp->s_max_size;
    brelse(bh);
    for (i=0;i < _XIAFS_IMAP_SLOTS;i++) {
        s->u.xiafs_sb.s_imap_buf[i] = NULL;
	s->u.xiafs_sb.s_imap_iznr[i] = -1;
    }
    for (i=0;i < _XIAFS_ZMAP_SLOTS;i++) {
        s->u.xiafs_sb.s_zmap_buf[i] = NULL;
	s->u.xiafs_sb.s_zmap_zznr[i] = -1;
    }
    z=1;
    if ( s->u.xiafs_sb.s_imap_zones > _XIAFS_IMAP_SLOTS )
        s->u.xiafs_sb.s_imap_cached=1;
    else {
        s->u.xiafs_sb.s_imap_cached=0;
	for (i=0 ; i < s->u.xiafs_sb.s_imap_zones ; i++) {
	    if (!(s->u.xiafs_sb.s_imap_buf[i]=bread(dev, z++, XIAFS_ZSIZE(s))))
	        goto xiafs_read_super_fail;
	    s->u.xiafs_sb.s_imap_iznr[i]=i;
	}
    }
    if ( s->u.xiafs_sb.s_zmap_zones > _XIAFS_ZMAP_SLOTS )
        s->u.xiafs_sb.s_zmap_cached=1;
    else {
        s->u.xiafs_sb.s_zmap_cached=0;
	for (i=0 ; i < s->u.xiafs_sb.s_zmap_zones ; i++) {
	    if (!(s->u.xiafs_sb.s_zmap_buf[i]=bread(dev, z++, XIAFS_ZSIZE(s))))
	        goto xiafs_read_super_fail;
	    s->u.xiafs_sb.s_zmap_zznr[i]=i;
	}
    }
    /* set up enough so that it can read an inode */
    s->s_dev = dev;
    s->s_op = &xiafs_sops;
    s->s_mounted = iget(s, _XIAFS_ROOT_INO);
    if (!s->s_mounted) 
        goto xiafs_read_super_fail;
    unlock_super(s);
    random_nr=CURRENT_TIME;
    return s;

xiafs_read_super_fail:
    for(i=0; i < _XIAFS_IMAP_SLOTS; i++)
        brelse(s->u.xiafs_sb.s_imap_buf[i]);
    for(i=0; i < _XIAFS_ZMAP_SLOTS; i++)
        brelse(s->u.xiafs_sb.s_zmap_buf[i]);
    s->s_dev=0;
    unlock_super(s);
    printk("XIA-FS: read bitmaps failed (%s %d)\n", WHERE_ERR);
    return NULL;
}

void xiafs_statfs(struct super_block *sb, struct statfs *buf)
{
    long tmp;

    put_fs_long(_XIAFS_SUPER_MAGIC, &buf->f_type);
    put_fs_long(XIAFS_ZSIZE(sb), &buf->f_bsize);
    put_fs_long(sb->u.xiafs_sb.s_ndatazones, &buf->f_blocks);
    tmp = xiafs_count_free_zones(sb);
    put_fs_long(tmp, &buf->f_bfree);
    put_fs_long(tmp, &buf->f_bavail);
    put_fs_long(sb->u.xiafs_sb.s_ninodes, &buf->f_files);
    put_fs_long(xiafs_count_free_inodes(sb), &buf->f_ffree);
    put_fs_long(_XIAFS_NAME_LEN, &buf->f_namelen);
    /* don't know what should be put in buf->f_fsid */
}

static int zone_bmap(struct buffer_head * bh, int nr)
{
    int tmp;

    if (!bh)
        return 0;
    tmp = ((u_long *) bh->b_data)[nr];
    brelse(bh);
    return tmp;
}

int xiafs_bmap(struct inode * inode,int zone)
{
    int i;

    if (zone < 0) {
        printk("XIA-FS: block < 0 (%s %d)\n", WHERE_ERR);
	return 0;
    }
    if (zone >= 8+(1+XIAFS_ADDRS_PER_Z(inode->i_sb))*XIAFS_ADDRS_PER_Z(inode->i_sb)) {
        printk("XIA-FS: zone > big (%s %d)\n", WHERE_ERR);
	return 0;
    }
    if (!IS_RDONLY (inode)) {
	inode->i_atime = CURRENT_TIME;
	inode->i_dirt = 1;
    }
    if (zone < 8)
        return inode->u.xiafs_i.i_zone[zone];
    zone -= 8;
    if (zone < XIAFS_ADDRS_PER_Z(inode->i_sb)) {
        i = inode->u.xiafs_i.i_ind_zone;
	if (i)
	    i = zone_bmap(bread(inode->i_dev, i, XIAFS_ZSIZE(inode->i_sb)), zone);
	return i;
    }
    zone -= XIAFS_ADDRS_PER_Z(inode->i_sb);
    i = inode->u.xiafs_i.i_dind_zone;
    if (i)
      i = zone_bmap(bread(inode->i_dev, i, XIAFS_ZSIZE(inode->i_sb)), 
		    zone >> XIAFS_ADDRS_PER_Z_BITS(inode->i_sb));
    if (i)
      i= zone_bmap(bread(inode->i_dev,i, XIAFS_ZSIZE(inode->i_sb)),
		   zone & (XIAFS_ADDRS_PER_Z(inode->i_sb)-1));
    return i;
}

static u_long get_prev_addr(struct inode * inode, int zone)
{
    u_long tmp;

    if (zone > 0)
        while (--zone >= 0)		/* only files with holes suffer */
	    if ((tmp=xiafs_bmap(inode, zone)))
	        return tmp;
    random_nr=(random_nr+23)%inode->i_sb->u.xiafs_sb.s_ndatazones;
    return random_nr + inode->i_sb->u.xiafs_sb.s_firstdatazone;
}

static struct buffer_head * 
dt_getblk(struct inode * inode, u_long *lp, int create, u_long prev_addr)
{
    int tmp;
    struct buffer_head * result;

repeat:
    if ((tmp=*lp)) {
        result = getblk(inode->i_dev, tmp, XIAFS_ZSIZE(inode->i_sb));
	if (tmp == *lp)
	    return result;
	brelse(result);
	goto repeat;
    }
    if (!create)
        return NULL;
    tmp = xiafs_new_zone(inode->i_sb, prev_addr);
    if (!tmp)
        return NULL;
    result = getblk(inode->i_dev, tmp, XIAFS_ZSIZE(inode->i_sb));
    if (*lp) {
        xiafs_free_zone(inode->i_sb, tmp);
        brelse(result);
        goto repeat;
    }
    *lp = tmp;
    inode->i_blocks+=2 << XIAFS_ZSHIFT(inode->i_sb);
    return result;
}

static struct buffer_head * 
indt_getblk(struct inode * inode, struct buffer_head * bh, 
	    int nr, int create, u_long prev_addr)
{
    int tmp;
    u_long *lp;
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
    lp = nr + (u_long *) bh->b_data;
repeat:
    if ((tmp=*lp)) {
        result = getblk(bh->b_dev, tmp, XIAFS_ZSIZE(inode->i_sb));
	if (tmp == *lp) {
	    brelse(bh);
	    return result;
	}
	brelse(result);
	goto repeat;
    }
    if (!create) {
        brelse(bh);
	return NULL;
    }
    tmp = xiafs_new_zone(inode->i_sb, prev_addr);
    if (!tmp) {
        brelse(bh);
	return NULL;
    }
    result = getblk(bh->b_dev, tmp, XIAFS_ZSIZE(inode->i_sb));
    if (*lp) {
        xiafs_free_zone(inode->i_sb, tmp);
	brelse(result);
	goto repeat;
    }
    *lp = tmp;
    inode->i_blocks+=2 << XIAFS_ZSHIFT(inode->i_sb);
    bh->b_dirt = 1;
    brelse(bh);
    return result;
}

struct buffer_head * xiafs_getblk(struct inode * inode, int zone, int create)
{
    struct buffer_head * bh;
    u_long prev_addr=0;

    if (zone<0) {
        printk("XIA-FS: zone < 0 (%s %d)\n", WHERE_ERR);
	return NULL;
    }
    if (zone >= 8+(1+XIAFS_ADDRS_PER_Z(inode->i_sb))*XIAFS_ADDRS_PER_Z(inode->i_sb)) {
	if (!create)
            printk("XIA-FS: zone > big (%s %d)\n", WHERE_ERR);
	return NULL;
    }
    if (create)
        prev_addr=get_prev_addr(inode, zone);
    if (zone < 8)
        return dt_getblk(inode, zone+inode->u.xiafs_i.i_zone, create, prev_addr);
    zone -= 8;
    if (zone < XIAFS_ADDRS_PER_Z(inode->i_sb)) {
        bh = dt_getblk(inode, &(inode->u.xiafs_i.i_ind_zone), create, prev_addr);
	bh = indt_getblk(inode, bh, zone, create, prev_addr);
	return bh;
    }
    zone -= XIAFS_ADDRS_PER_Z(inode->i_sb);
    bh = dt_getblk(inode, &(inode->u.xiafs_i.i_dind_zone), create, prev_addr);
    bh = indt_getblk(inode, bh, zone>>XIAFS_ADDRS_PER_Z_BITS(inode->i_sb), 
		     create, prev_addr);
    bh = indt_getblk(inode, bh, zone&(XIAFS_ADDRS_PER_Z(inode->i_sb)-1), 
		     create, prev_addr);
    return bh;
}

struct buffer_head * xiafs_bread(struct inode * inode, int zone, int create)
{
    struct buffer_head * bh;

    bh = xiafs_getblk(inode, zone, create);
    if (!bh || bh->b_uptodate)
        return bh;
    ll_rw_block(READ, 1, &bh);
    wait_on_buffer(bh);
    if (bh->b_uptodate)
        return bh;
    brelse(bh);
    return NULL;
}

void xiafs_read_inode(struct inode * inode)
{
    struct buffer_head * bh;
    struct xiafs_inode * raw_inode;
    int zone;
    ino_t ino;

    ino = inode->i_ino;
    inode->i_op = NULL;
    inode->i_mode=0;
    if (!ino || ino > inode->i_sb->u.xiafs_sb.s_ninodes) {
    	printk("XIA-FS: bad inode number (%s %d)\n", WHERE_ERR);
    	return;
    }
    zone = 1 + inode->i_sb->u.xiafs_sb.s_imap_zones +
		inode->i_sb->u.xiafs_sb.s_zmap_zones +
		(ino-1)/ XIAFS_INODES_PER_Z(inode->i_sb);
    if (!(bh=bread(inode->i_dev, zone, XIAFS_ZSIZE(inode->i_sb)))) {
    	printk("XIA-FS: read i-node zone failed (%s %d)\n", WHERE_ERR);
    	return;
    }
    raw_inode = ((struct xiafs_inode *) bh->b_data) + 
                 ((ino-1) & (XIAFS_INODES_PER_Z(inode->i_sb) - 1));
    inode->i_mode = raw_inode->i_mode;
    inode->i_uid = raw_inode->i_uid;
    inode->i_gid = raw_inode->i_gid;
    inode->i_nlink = raw_inode->i_nlinks;
    inode->i_size = raw_inode->i_size;
    inode->i_mtime = raw_inode->i_mtime;
    inode->i_atime = raw_inode->i_atime;
    inode->i_ctime = raw_inode->i_ctime;
    inode->i_blksize = XIAFS_ZSIZE(inode->i_sb);
    if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode)) {
        inode->i_blocks=0;
        inode->i_rdev = raw_inode->i_zone[0];
    } else {
        XIAFS_GET_BLOCKS(raw_inode, inode->i_blocks);
        for (zone = 0; zone < 8; zone++)
	    inode->u.xiafs_i.i_zone[zone] = raw_inode->i_zone[zone] & 0xffffff;
	inode->u.xiafs_i.i_ind_zone       = raw_inode->i_ind_zone   & 0xffffff;
	inode->u.xiafs_i.i_dind_zone      = raw_inode->i_dind_zone  & 0xffffff;
    }
    brelse(bh);
    if (S_ISREG(inode->i_mode))
        inode->i_op = &xiafs_file_inode_operations;
    else if (S_ISDIR(inode->i_mode))
        inode->i_op = &xiafs_dir_inode_operations;
    else if (S_ISLNK(inode->i_mode))
        inode->i_op = &xiafs_symlink_inode_operations;
    else if (S_ISCHR(inode->i_mode))
        inode->i_op = &chrdev_inode_operations;
    else if (S_ISBLK(inode->i_mode))
        inode->i_op = &blkdev_inode_operations;
    else if (S_ISFIFO(inode->i_mode))
    	init_fifo(inode);
}

static struct buffer_head *  xiafs_update_inode(struct inode * inode)
{
    struct buffer_head * bh;
    struct xiafs_inode * raw_inode;
    int zone;
    ino_t ino;

    if (IS_RDONLY (inode)) {
	printk("XIA-FS: write_inode on a read-only filesystem (%s %d)\n", WHERE_ERR);
	inode->i_dirt = 0;
	return 0;
    }

    ino = inode->i_ino;
    if (!ino || ino > inode->i_sb->u.xiafs_sb.s_ninodes) {
    	printk("XIA-FS: bad inode number (%s %d)\n", WHERE_ERR);
    	inode->i_dirt=0;
    	return 0;
    }
    zone = 1 + inode->i_sb->u.xiafs_sb.s_imap_zones + 
                inode->i_sb->u.xiafs_sb.s_zmap_zones +
		(ino-1) / XIAFS_INODES_PER_Z(inode->i_sb);
    if (!(bh=bread(inode->i_dev, zone, XIAFS_ZSIZE(inode->i_sb)))) {
        printk("XIA-FS: read i-node zone failed (%s %d)\n", WHERE_ERR);
	inode->i_dirt=0;
	return 0;
    }
    raw_inode = ((struct xiafs_inode *)bh->b_data) +
                ((ino-1) & (XIAFS_INODES_PER_Z(inode->i_sb) -1));
    raw_inode->i_mode = inode->i_mode;
    raw_inode->i_uid = inode->i_uid;
    raw_inode->i_gid = inode->i_gid;
    raw_inode->i_nlinks = inode->i_nlink;
    raw_inode->i_size = inode->i_size;
    raw_inode->i_atime = inode->i_atime;
    raw_inode->i_ctime = inode->i_ctime;
    raw_inode->i_mtime = inode->i_mtime;
    if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
        raw_inode->i_zone[0] = inode->i_rdev;
    else {
        XIAFS_PUT_BLOCKS(raw_inode, inode->i_blocks);
        for (zone = 0; zone < 8; zone++)
	    raw_inode->i_zone[zone] = (raw_inode->i_zone[zone] & 0xff000000) 
	                             | (inode->u.xiafs_i.i_zone[zone] & 0xffffff);
	raw_inode->i_ind_zone = (raw_inode->i_ind_zone & 0xff000000)
	                             | (inode->u.xiafs_i.i_ind_zone   & 0xffffff);
	raw_inode->i_dind_zone = (raw_inode->i_dind_zone & 0xff000000)
	                             | (inode->u.xiafs_i.i_dind_zone  & 0xffffff);
    }
    inode->i_dirt=0;
    bh->b_dirt=1;
    return bh;
}


void xiafs_write_inode(struct inode * inode)
{
    struct buffer_head * bh;
    bh = xiafs_update_inode(inode);
    brelse (bh);
}

int xiafs_sync_inode (struct inode *inode)
{
    int err = 0;
    struct buffer_head *bh;

    bh = xiafs_update_inode(inode);
    if (bh && bh->b_dirt)
    {
    	ll_rw_block(WRITE, 1, &bh);
    	wait_on_buffer(bh);
    	if (bh->b_req && !bh->b_uptodate)
    	{
    	    printk ("IO error syncing xiafs inode [%04X:%lu]\n",
		    inode->i_dev, inode->i_ino);
    	    err = -1;
    	}
    }
    else if (!bh)
    	err = -1;
    brelse (bh);
    return err;
}
