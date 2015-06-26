/*
 *  linux/fs/minix/bitmap.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/* bitmap.c contains the code that handles the inode and block bitmaps */

#include <linux/sched.h>
#include <linux/minix_fs.h>
#include <linux/stat.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include <asm/bitops.h>

#define clear_block(addr) \
__asm__("cld\n\t" \
	"rep\n\t" \
	"stosl" \
	: \
	:"a" (0),"c" (BLOCK_SIZE/4),"D" ((long) (addr)):"cx","di")

#define find_first_zero(addr) ({ \
int __res; \
__asm__("cld\n" \
	"1:\tlodsl\n\t" \
	"notl %%eax\n\t" \
	"bsfl %%eax,%%edx\n\t" \
	"jne 2f\n\t" \
	"addl $32,%%ecx\n\t" \
	"cmpl $8192,%%ecx\n\t" \
	"jl 1b\n\t" \
	"xorl %%edx,%%edx\n" \
	"2:\taddl %%edx,%%ecx" \
	:"=c" (__res):"0" (0),"S" (addr):"ax","dx","si"); \
__res;})

static int nibblemap[] = { 0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4 };

static unsigned long count_used(struct buffer_head *map[], unsigned numblocks,
	unsigned numbits)
{
	unsigned i, j, end, sum = 0;
	struct buffer_head *bh;
  
	for (i=0; (i<numblocks) && numbits; i++) {
		if (!(bh=map[i])) 
			return(0);
		if (numbits >= (8*BLOCK_SIZE)) { 
			end = BLOCK_SIZE;
			numbits -= 8*BLOCK_SIZE;
		} else {
			int tmp;
			end = numbits >> 3;
			numbits &= 0x7;
			tmp = bh->b_data[end] & ((1<<numbits)-1);
			sum += nibblemap[tmp&0xf] + nibblemap[(tmp>>4)&0xf];
			numbits = 0;
		}  
		for (j=0; j<end; j++)
			sum += nibblemap[bh->b_data[j] & 0xf] 
				+ nibblemap[(bh->b_data[j]>>4)&0xf];
	}
	return(sum);
}

void minix_free_block(struct super_block * sb, int block)
{
	struct buffer_head * bh;
	unsigned int bit,zone;

	if (!sb) {
		printk("trying to free block on nonexistent device\n");
		return;
	}
	if (block < sb->u.minix_sb.s_firstdatazone ||
	    block >= sb->u.minix_sb.s_nzones) {
		printk("trying to free block not in datazone\n");
		return;
	}
	bh = get_hash_table(sb->s_dev,block,BLOCK_SIZE);
	if (bh)
		bh->b_dirt=0;
	brelse(bh);
	zone = block - sb->u.minix_sb.s_firstdatazone + 1;
	bit = zone & 8191;
	zone >>= 13;
	bh = sb->u.minix_sb.s_zmap[zone];
	if (!bh) {
		printk("minix_free_block: nonexistent bitmap buffer\n");
		return;
	}
	if (!clear_bit(bit,bh->b_data))
		printk("free_block (%04x:%d): bit already cleared\n",sb->s_dev,block);
	bh->b_dirt = 1;
	return;
}

int minix_new_block(struct super_block * sb)
{
	struct buffer_head * bh;
	int i,j;

	if (!sb) {
		printk("trying to get new block from nonexistent device\n");
		return 0;
	}
repeat:
	j = 8192;
	for (i=0 ; i<8 ; i++)
		if ((bh=sb->u.minix_sb.s_zmap[i]) != NULL)
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	if (i>=8 || !bh || j>=8192)
		return 0;
	if (set_bit(j,bh->b_data)) {
		printk("new_block: bit already set");
		goto repeat;
	}
	bh->b_dirt = 1;
	j += i*8192 + sb->u.minix_sb.s_firstdatazone-1;
	if (j < sb->u.minix_sb.s_firstdatazone ||
	    j >= sb->u.minix_sb.s_nzones)
		return 0;
	if (!(bh = getblk(sb->s_dev,j,BLOCK_SIZE))) {
		printk("new_block: cannot get block");
		return 0;
	}
	clear_block(bh->b_data);
	bh->b_uptodate = 1;
	bh->b_dirt = 1;
	brelse(bh);
	return j;
}

unsigned long minix_count_free_blocks(struct super_block *sb)
{
	return (sb->u.minix_sb.s_nzones - count_used(sb->u.minix_sb.s_zmap,sb->u.minix_sb.s_zmap_blocks,sb->u.minix_sb.s_nzones))
		 << sb->u.minix_sb.s_log_zone_size;
}

void minix_free_inode(struct inode * inode)
{
	struct buffer_head * bh;
	unsigned long ino;

	if (!inode)
		return;
	if (!inode->i_dev) {
		printk("free_inode: inode has no device\n");
		return;
	}
	if (inode->i_count != 1) {
		printk("free_inode: inode has count=%d\n",inode->i_count);
		return;
	}
	if (inode->i_nlink) {
		printk("free_inode: inode has nlink=%d\n",inode->i_nlink);
		return;
	}
	if (!inode->i_sb) {
		printk("free_inode: inode on nonexistent device\n");
		return;
	}
	if (inode->i_ino < 1 || inode->i_ino >= inode->i_sb->u.minix_sb.s_ninodes) {
		printk("free_inode: inode 0 or nonexistent inode\n");
		return;
	}
	ino = inode->i_ino;
	if (!(bh=inode->i_sb->u.minix_sb.s_imap[ino >> 13])) {
		printk("free_inode: nonexistent imap in superblock\n");
		return;
	}
	clear_inode(inode);
	if (!clear_bit(ino & 8191, bh->b_data))
		printk("free_inode: bit %lu already cleared.\n",ino);
	bh->b_dirt = 1;
}

struct inode * minix_new_inode(const struct inode * dir)
{
	struct super_block * sb;
	struct inode * inode;
	struct buffer_head * bh;
	int i,j;

	if (!dir || !(inode = get_empty_inode()))
		return NULL;
	sb = dir->i_sb;
	inode->i_sb = sb;
	inode->i_flags = inode->i_sb->s_flags;
	j = 8192;
	for (i=0 ; i<8 ; i++)
		if ((bh = inode->i_sb->u.minix_sb.s_imap[i]) != NULL)
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	if (!bh || j >= 8192) {
		iput(inode);
		return NULL;
	}
	if (set_bit(j,bh->b_data)) {	/* shouldn't happen */
		printk("new_inode: bit already set");
		iput(inode);
		return NULL;
	}
	bh->b_dirt = 1;
	j += i*8192;
	if (!j || j >= inode->i_sb->u.minix_sb.s_ninodes) {
		iput(inode);
		return NULL;
	}
	inode->i_count = 1;
	inode->i_nlink = 1;
	inode->i_dev = sb->s_dev;
	inode->i_uid = current->euid;
	inode->i_gid = (dir->i_mode & S_ISGID) ? dir->i_gid : current->egid;
	inode->i_dirt = 1;
	inode->i_ino = j;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	inode->i_op = NULL;
	inode->i_blocks = inode->i_blksize = 0;
	insert_inode_hash(inode);
	return inode;
}

unsigned long minix_count_free_inodes(struct super_block *sb)
{
	return sb->u.minix_sb.s_ninodes - count_used(sb->u.minix_sb.s_imap,sb->u.minix_sb.s_imap_blocks,sb->u.minix_sb.s_ninodes);
}
