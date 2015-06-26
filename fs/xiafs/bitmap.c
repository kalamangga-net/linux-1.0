/*
 *  linux/fs/xiafs/bitmap.c
 *
 *  Copyright (C) Q. Frank Xia, 1993.
 *  
 *  Based on Linus' minix/bitmap.c
 *  Copyright (C) Linus Torvalds, 1991, 1992.
 *  
 *  This software may be redistributed per Linux Copyright.
 */

/* bitmap.c contains the code that handles the inode and block bitmaps */

#include <linux/sched.h>
#include <linux/locks.h>
#include <linux/xia_fs.h>
#include <linux/stat.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "xiafs_mac.h"


#define clear_bit(nr,addr) ({\
char res; \
__asm__ __volatile__("btrl %1,%2\n\tsetnb %0": \
"=q" (res):"r" (nr),"m" (*(addr))); \
res;})

char internal_error_message[]="XIA-FS: internal error %s %d\n"; 

static int find_first_zero(struct buffer_head *bh, int start_bit, int end_bit) 
{
    /* This routine searches first 0 bit from (start_bit) to (end_bit-1).
     * If found the bit is set to 1 and the bit # is returned, otherwise,
     * -1 is returned. Race condition is avoid by using "btsl" and 
     * "goto repeat".  ---Frank.
     */

    int end, i, j, tmp;
    u_long *bmap;
    char res;

    bmap=(u_long *)bh->b_data;
    end = end_bit >> 5;

repeat:
    i=start_bit >> 5;
    if ( (tmp=(~bmap[i]) & (0xffffffff << (start_bit & 31))) )        
        goto zone_found;
    while (++i < end)
        if (~bmap[i]) {
	    tmp=~bmap[i];
	    goto zone_found;
	}
    if ( !(tmp=~bmap[i] & ((1 << (end_bit & 31)) -1)) )
        return -1;
zone_found:    
    for (j=0; j < 32; j++)
        if (tmp & (1 << j))
	    break;
    __asm__ ("btsl %1,%2\n\tsetb %0": \
	     "=q" (res):"r" (j),"m" (bmap[i]));
    if (res) {
        start_bit=j + (i << 5) + 1;
	goto repeat;
    }
    bh->b_dirt=1;
    return j + (i << 5);
}

static void clear_buf(struct buffer_head * bh) 
{
    register int i;
    register long * lp;

    lp=(long *)bh->b_data;
    for (i= bh->b_size >> 2; i-- > 0; )
        *lp++=0;
}

static void que(struct buffer_head * bmap[], int bznr[], int pos)
{
    struct buffer_head * tbh;
    int tmp;
    int i;
    
    tbh=bmap[pos];
    tmp=bznr[pos];
    for (i=pos; i > 0; i--) {
        bmap[i]=bmap[i-1];
	bznr[i]=bznr[i-1];
    }
    bmap[0]=tbh;
    bznr[0]=tmp;
}

#define get_imap_zone(sb, bit_nr, not_que) \
      	get__map_zone((sb), (sb)->u.xiafs_sb.s_imap_buf, \
		      (sb)->u.xiafs_sb.s_imap_iznr, \
		      (sb)->u.xiafs_sb.s_imap_cached, 1, \
		      (sb)->u.xiafs_sb.s_imap_zones, _XIAFS_IMAP_SLOTS, \
		      bit_nr, not_que)

#define get_zmap_zone(sb, bit_nr, not_que) \
      	get__map_zone((sb), (sb)->u.xiafs_sb.s_zmap_buf, \
		      (sb)->u.xiafs_sb.s_zmap_zznr, \
		      (sb)->u.xiafs_sb.s_zmap_cached, \
		      1+(sb)->u.xiafs_sb.s_imap_zones, \
		      (sb)->u.xiafs_sb.s_zmap_zones, _XIAFS_ZMAP_SLOTS, \
		      bit_nr, not_que)

static struct buffer_head * 
get__map_zone(struct super_block *sb, struct buffer_head * bmap_buf[],
	  int bznr[], u_char cache, int first_zone, 
	  int bmap_zones, int slots, u_long bit_nr, int * not_que)
{
    struct buffer_head * tmp_bh;
    int z_nr, i;

    z_nr = bit_nr >> XIAFS_BITS_PER_Z_BITS(sb);
    if (z_nr >= bmap_zones) {
        printk("XIA-FS: bad inode/zone number (%s %d)\n", WHERE_ERR);
	return NULL;
    }
    if (!cache)
        return bmap_buf[z_nr];
    lock_super(sb);
    for (i=0; i < slots; i++) 
        if (bznr[i]==z_nr)
	    break;
    if (i < slots) {			/* cache hit */
        if (not_que) {
	    *not_que=i;
	    return bmap_buf[i];
	} else {
	    que(bmap_buf, bznr, i);
	    return bmap_buf[0];
	}
    }
    tmp_bh=bread(sb->s_dev, z_nr+first_zone, XIAFS_ZSIZE(sb)); /* cache not hit */
    if (!tmp_bh) {
        printk("XIA-FS: read bitmap failed (%s %d)\n", WHERE_ERR);
	unlock_super(sb);
	return NULL;
    }
    brelse(bmap_buf[slots-1]);
    bmap_buf[slots-1]=tmp_bh;
    bznr[slots-1]=z_nr;
    if (not_que)
        *not_que=slots-1;
    else
        que(bmap_buf, bznr, slots-1);
    return tmp_bh;
}

#define xiafs_unlock_super(sb, cache)	if (cache) unlock_super(sb);

#define get_free_ibit(sb, prev_bit) \
   	get_free__bit(sb, sb->u.xiafs_sb.s_imap_buf, \
		      sb->u.xiafs_sb.s_imap_iznr, \
		      sb->u.xiafs_sb.s_imap_cached, \
		      1, sb->u.xiafs_sb.s_imap_zones, \
		      _XIAFS_IMAP_SLOTS, prev_bit);

#define get_free_zbit(sb, prev_bit) \
   	get_free__bit(sb, sb->u.xiafs_sb.s_zmap_buf, \
		      sb->u.xiafs_sb.s_zmap_zznr, \
		      sb->u.xiafs_sb.s_zmap_cached, \
		      1 + sb->u.xiafs_sb.s_imap_zones, \
		      sb->u.xiafs_sb.s_zmap_zones, \
		      _XIAFS_ZMAP_SLOTS, prev_bit);

static u_long 
get_free__bit(struct super_block *sb, struct buffer_head * bmap_buf[],
	      int bznr[], u_char cache, int first_zone, int bmap_zones, 
	      int slots, u_long prev_bit)
{
    struct buffer_head * bh;
    int not_done=0;
    u_long pos, start_bit, end_bit, total_bits;
    int z_nr, tmp;
 
    total_bits=bmap_zones << XIAFS_BITS_PER_Z_BITS(sb); 
    if (prev_bit >= total_bits)
        prev_bit=0;
    pos=prev_bit+1;
    end_bit=XIAFS_BITS_PER_Z(sb);

    do {
        if (pos >= total_bits)
	    pos=0;
        if (!not_done) {		/* first time */
	    not_done=1;
	    start_bit= pos & (end_bit-1);     
	} else 
	    start_bit=0;
	if ( pos < prev_bit && pos+end_bit >= prev_bit) {   /* last time */
	    not_done=0;
	    end_bit=prev_bit & (end_bit-1);   /* only here end_bit modified */
	}
        bh = get__map_zone(sb, bmap_buf, bznr, cache, first_zone, 
			   bmap_zones, slots, pos, &z_nr);
	if (!bh)
	    return 0;
	tmp=find_first_zero(bh, start_bit, end_bit);
	if (tmp >= 0)
	    break;
	xiafs_unlock_super(sb, sb->u.xiafs_sb.s_zmap_cached);
	pos=(pos & ~(end_bit-1))+end_bit;
    } while (not_done);

    if (tmp < 0) 
        return 0;
    if (cache)
      que(bmap_buf, bznr, z_nr);
    xiafs_unlock_super(sb, cache);
    return (pos & ~(XIAFS_BITS_PER_Z(sb)-1))+tmp;
}

void xiafs_free_zone(struct super_block * sb, int d_addr)
{
    struct buffer_head * bh;
    unsigned int bit, offset;

    if (!sb) {
        printk(INTERN_ERR);
	return;
    }
    if (d_addr < sb->u.xiafs_sb.s_firstdatazone ||
	d_addr >= sb->u.xiafs_sb.s_nzones) {
        printk("XIA-FS: bad zone number (%s %d)\n", WHERE_ERR);
	return;
    }
    bh = get_hash_table(sb->s_dev, d_addr, XIAFS_ZSIZE(sb));
    if (bh)
        bh->b_dirt=0;
    brelse(bh);
    bit=d_addr - sb->u.xiafs_sb.s_firstdatazone + 1;
    bh = get_zmap_zone(sb, bit, NULL);
    if (!bh)
	return;
    offset = bit & (XIAFS_BITS_PER_Z(sb) -1);
    if (clear_bit(offset, bh->b_data))
        printk("XIA-FS: dev %04x"
	       " block bit %u (0x%x) already cleared (%s %d)\n",
	       sb->s_dev, bit, bit, WHERE_ERR);
    bh->b_dirt = 1;
    xiafs_unlock_super(sb, sb->u.xiafs_sb.s_zmap_cached);
}

int xiafs_new_zone(struct super_block * sb, u_long prev_addr)
{
    struct buffer_head * bh;
    int prev_znr, tmp;

    if (!sb) {
        printk(INTERN_ERR);
	return 0;
    }
    if (prev_addr < sb->u.xiafs_sb.s_firstdatazone || 
	prev_addr >= sb->u.xiafs_sb.s_nzones) {
        prev_addr=sb->u.xiafs_sb.s_firstdatazone;
    }      
    prev_znr=prev_addr-sb->u.xiafs_sb.s_firstdatazone+1;
    tmp=get_free_zbit(sb, prev_znr);
    if (!tmp)
        return 0;
    tmp += sb->u.xiafs_sb.s_firstdatazone -1;
    if (!(bh = getblk(sb->s_dev, tmp, XIAFS_ZSIZE(sb)))) {
        printk("XIA-FS: I/O error (%s %d)\n", WHERE_ERR);
	return 0;
    }
    if (bh->b_count != 1) {
        printk(INTERN_ERR);
	return 0;
    }
    clear_buf(bh);
    bh->b_uptodate = 1;
    bh->b_dirt = 1;
    brelse(bh);
    return tmp;
}

void xiafs_free_inode(struct inode * inode)
{
    struct buffer_head * bh;
    struct super_block * sb;
    unsigned long ino;

    if (!inode)
        return;
    if (!inode->i_dev || inode->i_count!=1 || inode->i_nlink || !inode->i_sb ||
	inode->i_ino < 3 || inode->i_ino > inode->i_sb->u.xiafs_sb.s_ninodes) {
        printk("XIA-FS: bad inode (%s %d)\n", WHERE_ERR);
	return;
    }
    sb = inode->i_sb;
    ino = inode->i_ino;
    bh = get_imap_zone(sb, ino, NULL);
    if (!bh)
	return;
    clear_inode(inode);
    if (clear_bit(ino & (XIAFS_BITS_PER_Z(sb)-1), bh->b_data))
        printk("XIA-FS: dev %04x"
	       "inode bit %ld (0x%lx) already cleared (%s %d)\n",
	       inode->i_dev, ino, ino, WHERE_ERR);
    bh->b_dirt = 1;
    xiafs_unlock_super(sb, sb->u.xiafs_sb.s_imap_cached);
}

struct inode * xiafs_new_inode(struct inode * dir)
{
    struct super_block * sb;
    struct inode * inode;
    ino_t tmp;

    sb = dir->i_sb;
    if (!dir || !(inode = get_empty_inode()))
        return NULL;
    inode->i_sb = sb;
    inode->i_flags = inode->i_sb->s_flags;

    tmp=get_free_ibit(sb, dir->i_ino); 
    if (!tmp) {
        iput(inode);
	return NULL;
    }
    inode->i_count = 1;
    inode->i_nlink = 1;
    inode->i_dev = sb->s_dev;
    inode->i_uid = current->euid;
    inode->i_gid = (dir->i_mode & S_ISGID) ? dir->i_gid : current->egid;
    inode->i_dirt = 1;
    inode->i_ino = tmp;
    inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
    inode->i_op = NULL;
    inode->i_blocks = 0;
    inode->i_blksize = XIAFS_ZSIZE(inode->i_sb);
    insert_inode_hash(inode);
    return inode;
}

static int nibblemap[] = { 0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4 };

static u_long count_zone(struct buffer_head * bh)
{
    int i, tmp;
    u_long sum;

    sum=0;
    for (i=bh->b_size; i-- > 0; ) {
        tmp=bh->b_data[i];
        sum += nibblemap[tmp & 0xf] + nibblemap[(tmp & 0xff) >> 4];
    }
    return sum;
} 

unsigned long xiafs_count_free_inodes(struct super_block *sb)
{
    struct buffer_head * bh;
    int izones, i, not_que;
    u_long sum;

    sum=0;
    izones=sb->u.xiafs_sb.s_imap_zones;
    for (i=0; i < izones; i++) {
        bh=get_imap_zone(sb, i << XIAFS_BITS_PER_Z_BITS(sb), &not_que);
	if (bh) {
	    sum += count_zone(bh);
	    xiafs_unlock_super(sb, sb->u.xiafs_sb.s_imap_cached);
	}
    }
    i=izones << XIAFS_BITS_PER_Z_BITS(sb);
    return i - sum;
}

unsigned long xiafs_count_free_zones(struct super_block *sb)
{
    struct buffer_head * bh;
    int zzones, i, not_que;
    u_long sum;

    sum=0;
    zzones=sb->u.xiafs_sb.s_zmap_zones;
    for (i=0; i < zzones; i++) {
        bh=get_zmap_zone(sb, i << XIAFS_BITS_PER_Z_BITS(sb), &not_que);
	if (bh) {
	    sum += count_zone(bh);
	    xiafs_unlock_super(sb, sb->u.xiafs_sb.s_zmap_cached);
	}
    }
    i=zzones << XIAFS_BITS_PER_Z_BITS(sb);
    return i - sum;
}
