/*
 *  linux/fs/isofs/file.c
 *
 *  (C) 1992  Eric Youngdale Modified for ISO9660 filesystem.
 *
 *  (C) 1991  Linus Torvalds - minix filesystem
 *
 *  isofs regular file handling primitives
 */

#include <asm/segment.h>
#include <asm/system.h>

#include <linux/sched.h>
#include <linux/iso_fs.h>
#include <linux/fcntl.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/stat.h>
#include <linux/locks.h>

#include <linux/dirent.h>

#define	NBUF	32

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

#include <linux/fs.h>
#include <linux/iso_fs.h>

static int isofs_file_read(struct inode *, struct file *, char *, int);

/*
 * We have mostly NULL's here: the current defaults are ok for
 * the isofs filesystem.
 */
static struct file_operations isofs_file_operations = {
	NULL,			/* lseek - default */
	isofs_file_read,	/* read */
	NULL,			/* write */
	NULL,			/* readdir - bad */
	NULL,			/* select - default */
	NULL,			/* ioctl - default */
	generic_mmap,		/* mmap */
	NULL,			/* no special open is needed */
	NULL,			/* release */
	NULL			/* fsync */
};

struct inode_operations isofs_file_inode_operations = {
	&isofs_file_operations,	/* default file operations */
	NULL,			/* create */
	NULL,			/* lookup */
	NULL,			/* link */
	NULL,			/* unlink */
	NULL,			/* symlink */
	NULL,			/* mkdir */
	NULL,			/* rmdir */
	NULL,			/* mknod */
	NULL,			/* rename */
	NULL,			/* readlink */
	NULL,			/* follow_link */
	isofs_bmap,		/* bmap */
	NULL,	       		/* truncate */
	NULL			/* permission */
};

/* This is a heuristic to determine if a file is text of binary.  If it
 * is text, then we translate all 0x0d characters to spaces.  If the 0x0d
 * character is not preceeded or followed by a 0x0a, then we turn it into
 * a 0x0a.  A control-Z is also turned into a linefeed.
 */

static inline void unixify_text_buffer(char * buffer, int chars, int mode)
{
	while(chars--){
		if(*buffer == 0x1a) *buffer = 0x0a;
		if(*buffer == 0x0d){
			if(mode == ISOFS_FILE_TEXT_M) *buffer = 0x0a;
			if(mode == ISOFS_FILE_TEXT) *buffer = ' ';
		}
		buffer++;
	}
}

/*This function determines if a given file has a DOS-like text format or not*/

static void isofs_determine_filetype(struct inode * inode)
{
	int block;
	int result, i;
	struct buffer_head * bh;
	unsigned char * pnt;
	
	block = isofs_bmap(inode,0);
	if (block && (bh = bread(inode->i_dev,block, ISOFS_BUFFER_SIZE(inode)))) {
		pnt = (unsigned char *) bh->b_data;
		result = ISOFS_FILE_TEXT_M;
		for(i=0;i<(inode->i_size < ISOFS_BUFFER_SIZE(inode) ? inode->i_size : ISOFS_BUFFER_SIZE(inode));
		    i++,pnt++){
			if(*pnt & 0x80) {result = ISOFS_FILE_BINARY; break;};
			if(*pnt >= 0x20 || *pnt == 0x1a) continue;
			if(*pnt == 0x0a) {result = ISOFS_FILE_TEXT; continue;};
			if(*pnt >= 0x9 && *pnt <= 0x0d) continue;
			result = ISOFS_FILE_BINARY;
			break;
		}
		brelse(bh);
		inode->u.isofs_i.i_file_format = result;
	}
}

static int isofs_file_read(struct inode * inode, struct file * filp, char * buf, int count)
{
	int read,left,chars;
	int block, blocks, offset;
	int bhrequest;
	int ra_blocks, max_block, nextblock;
	struct buffer_head ** bhb, ** bhe;
	struct buffer_head * bhreq[NBUF];
	struct buffer_head * buflist[NBUF];
	
	if (!inode) {
		printk("isofs_file_read: inode = NULL\n");
		return -EINVAL;
	}
	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode))) {
		printk("isofs_file_read: mode = %07o\n",inode->i_mode);
		return -EINVAL;
	}
	if (inode->u.isofs_i.i_file_format == ISOFS_FILE_UNKNOWN)
		isofs_determine_filetype(inode);
	if (filp->f_pos > inode->i_size)
		left = 0;
	else
		left = inode->i_size - filp->f_pos;
	if (left > count)
		left = count;
	if (left <= 0)
		return 0;
	read = 0;
	block = filp->f_pos >> ISOFS_BUFFER_BITS(inode);
	offset = filp->f_pos & (ISOFS_BUFFER_SIZE(inode)-1);
	blocks = (left + offset + ISOFS_BUFFER_SIZE(inode) - 1) / ISOFS_BUFFER_SIZE(inode);
	bhb = bhe = buflist;

	ra_blocks = read_ahead[MAJOR(inode->i_dev)] / (BLOCK_SIZE >> 9);
	max_block = (inode->i_size + BLOCK_SIZE - 1)/BLOCK_SIZE;
	nextblock = -1;

	/* We do this in a two stage process.  We first try and request
	   as many blocks as we can, then we wait for the first one to
	   complete, and then we try and wrap up as many as are actually
	   done.  This routine is rather generic, in that it can be used
	   in a filesystem by substituting the appropriate function in
	   for getblk.

	   This routine is optimized to make maximum use of the various
	   buffers and caches. */

	do {
	        bhrequest = 0;
		while (blocks) {
		        int uptodate;
			--blocks;
			*bhb = getblk(inode->i_dev,isofs_bmap(inode, block++), ISOFS_BUFFER_SIZE(inode));
			uptodate = 1;
			if (*bhb && !(*bhb)->b_uptodate) {
			        uptodate = 0;
			        bhreq[bhrequest++] = *bhb;
				nextblock = (*bhb)->b_blocknr + 1;
			      };

			if (++bhb == &buflist[NBUF])
				bhb = buflist;

			/* If the block we have on hand is uptodate, go ahead
			   and complete processing. */
			if(bhrequest == 0 && uptodate) break;

			if (bhb == bhe)
				break;
		      }

		if(blocks == 0 && bhrequest && filp->f_reada && bhb != bhe) { 
		  /* If we are going to read something anyways, add in the
		     read-ahead blocks */
		  while(ra_blocks){
		    if (block >= max_block) break;
		    if(bhrequest == NBUF) break;  /* Block full */
		    --ra_blocks;
		    *bhb = getblk(inode->i_dev,isofs_bmap(inode, block++), ISOFS_BUFFER_SIZE(inode));

		    if (*bhb && !(*bhb)->b_uptodate) {
		      if((*bhb)->b_blocknr != nextblock) {
			brelse(*bhb);
			break;
		      };
		      nextblock = (*bhb)->b_blocknr + 1;
		      bhreq[bhrequest++] = *bhb;
		    };
		    
		    if (++bhb == &buflist[NBUF])
		      bhb = buflist;
		    
		    if (bhb == bhe)
		      break;
		  };
		};
		/* Now request them all */
		if (bhrequest)
		  ll_rw_block(READ, bhrequest, bhreq);

		do{ /* Finish off all I/O that has actually completed */
		  if (*bhe) {/* test for valid buffer */
		    wait_on_buffer(*bhe);
		    if (!(*bhe)->b_uptodate) {
		      brelse(*bhe);
		      if (++bhe == &buflist[NBUF])
			bhe = buflist;
		      left = 0;
		      break;
		    }
		  }
		  
		  if (left < ISOFS_BUFFER_SIZE(inode) - offset)
		    chars = left;
		  else
		    chars = ISOFS_BUFFER_SIZE(inode) - offset;
		  filp->f_pos += chars;
		  left -= chars;
		  read += chars;
		  if (*bhe) {
		    if (inode->u.isofs_i.i_file_format == ISOFS_FILE_TEXT ||
			inode->u.isofs_i.i_file_format == ISOFS_FILE_TEXT_M)
		      unixify_text_buffer(offset+(*bhe)->b_data,
					  chars, inode->u.isofs_i.i_file_format);
		    memcpy_tofs(buf,offset+(*bhe)->b_data,chars);
		    brelse(*bhe);
		    buf += chars;
		  } else {
		    while (chars-->0)
		      put_fs_byte(0,buf++);
		  }
		  offset = 0;
		  if (++bhe == &buflist[NBUF])
		    bhe = buflist;
		} while( bhe != bhb && (*bhe == 0 || !(*bhe)->b_lock) && 
			(left > 0));
	} while (left > 0);

/* Release the read-ahead blocks */
	while (bhe != bhb) {
	  if (*bhe) brelse(*bhe);
	  if (++bhe == &buflist[NBUF])
	    bhe = buflist;
	};

	filp->f_reada = 1;

	if (!read)
		return -EIO;
	return read;
}
