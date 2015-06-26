/*
 *  linux/fs/isofs/dir.c
 *
 *  (C) 1992  Eric Youngdale Modified for ISO9660 filesystem.
 *
 *  (C) 1991  Linus Torvalds - minix filesystem
 *
 *  isofs directory handling functions
 */

#include <linux/errno.h>

#include <asm/segment.h>

#include <linux/fs.h>
#include <linux/iso_fs.h>
#include <linux/kernel.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/malloc.h>

static int isofs_readdir(struct inode *, struct file *, struct dirent *, int);

static struct file_operations isofs_dir_operations = {
	NULL,			/* lseek - default */
	NULL,			/* read */
	NULL,			/* write - bad */
	isofs_readdir,		/* readdir */
	NULL,			/* select - default */
	NULL,			/* ioctl - default */
	NULL,			/* no special open code */
	NULL,			/* no special release code */
	NULL			/* fsync */
};

/*
 * directories can handle most operations...
 */
struct inode_operations isofs_dir_inode_operations = {
	&isofs_dir_operations,	/* default directory file-ops */
	NULL,      	/* create */
	isofs_lookup,		/* lookup */
	NULL, 		       /* link */
	NULL,       		/* unlink */
	NULL,       		/* symlink */
	NULL,       		/* mkdir */
	NULL,			/* rmdir */
	NULL,			/* mknod */
	NULL,			/* rename */
	NULL,			/* readlink */
	NULL,			/* follow_link */
	isofs_bmap,		/* bmap */
	NULL,			/* truncate */
	NULL			/* permission */
};

/* This is used to speed up lookup.  Without this we would need to
make a linear search of the directory to find the file that the
directory read just returned.  This is a single element cache. */

struct lookup_cache cache = {0,};

static int isofs_readdir(struct inode * inode, struct file * filp,
	struct dirent * dirent, int count)
{
	unsigned long bufsize = ISOFS_BUFFER_SIZE(inode);
	unsigned char bufbits = ISOFS_BUFFER_BITS(inode);
	unsigned int block,offset,i, j;
	char c = 0;
	int inode_number;
	struct buffer_head * bh;
	void * cpnt = NULL;
	unsigned int old_offset;
	int dlen, rrflag;
	char * dpnt;
	struct iso_directory_record * de;
	
	if (!inode || !S_ISDIR(inode->i_mode))
		return -EBADF;
	
	offset = filp->f_pos & (bufsize - 1);
	block = isofs_bmap(inode,filp->f_pos>>bufbits);
	if (!block || !(bh = bread(inode->i_dev,block,bufsize)))
		return 0;
	
	while (filp->f_pos < inode->i_size) {
#ifdef DEBUG
		printk("Block, offset: %x %x %x\n",
		       block, offset, filp->f_pos);
#endif
		de = (struct iso_directory_record *) (bh->b_data + offset);
		inode_number = (block << bufbits) + (offset & (bufsize - 1));
		
		/* If the length byte is zero, we should move on to the next
		   CDROM sector.  If we are at the end of the directory, we
		   kick out of the while loop. */
		
		if (*((unsigned char *) de) == 0)  {
			brelse(bh);
			offset = 0;
			filp->f_pos = ((filp->f_pos & ~(ISOFS_BLOCK_SIZE - 1))
				       + ISOFS_BLOCK_SIZE);
			block = isofs_bmap(inode,(filp->f_pos)>>bufbits);
			if (!block
			    || !(bh = bread(inode->i_dev,block,bufsize)))
				return 0;
			continue;
		}

		/* Make sure that the entire directory record is in the
		   current bh block.
		   If not, we malloc a buffer, and put the two halves together,
		   so that we can cleanly read the block */

		old_offset = offset;
		offset += *((unsigned char *) de);
		filp->f_pos += *((unsigned char *) de);

		if (offset >=  bufsize) {
			cpnt = kmalloc(1 << ISOFS_BLOCK_BITS, GFP_KERNEL);
			memcpy(cpnt, bh->b_data, bufsize);
			de = (struct iso_directory_record *)
				((char *)cpnt + old_offset);
			brelse(bh);
			offset = filp->f_pos & (bufsize - 1);
			block = isofs_bmap(inode,(filp->f_pos)>> bufbits);
			if (!block
			    || !(bh = bread(inode->i_dev,block,bufsize))) {
			        kfree_s(cpnt, 1 << ISOFS_BLOCK_BITS);
				return 0;
			};
			memcpy((char *)cpnt+bufsize, bh->b_data, bufsize);
		}
		
		/* Handle the case of the '.' directory */

		rrflag = 0;
		i = 1;
		if (de->name_len[0] == 1 && de->name[0] == 0) {
			put_fs_byte('.',dirent->d_name);
			inode_number = inode->i_ino;
			dpnt = ".";
		}
		
		/* Handle the case of the '..' directory */
		
		else if (de->name_len[0] == 1 && de->name[0] == 1) {
			put_fs_byte('.',dirent->d_name);
			put_fs_byte('.',dirent->d_name+1);
			i = 2;
			dpnt = "..";
			if((inode->i_sb->u.isofs_sb.s_firstdatazone
			    << bufbits) != inode->i_ino)
				inode_number = inode->u.isofs_i.i_backlink;
			else
				inode_number = inode->i_ino;
			
			/* This should never happen, but who knows.  Try to be forgiving */
			if(inode_number == -1) {
				inode_number = 
					isofs_lookup_grandparent(inode,
					     find_rock_ridge_relocation(de, inode));
				if(inode_number == -1){ /* Should never happen */
					printk("Backlink not properly set.\n");
					goto out;
				};
			}
		}
		
		/* Handle everything else.  Do name translation if there
		   is no Rock Ridge NM field. */
		
		else {
			dlen = de->name_len[0];
			dpnt = de->name;
			i = dlen;
			rrflag = get_rock_ridge_filename(de, &dpnt, &dlen, inode);
			if (rrflag) {
			  if (rrflag == -1) {  /* This is a rock ridge reloc dir */
			    if (cpnt) {
				kfree_s(cpnt, 1 << ISOFS_BLOCK_BITS);
				cpnt = NULL;
			    };
			    continue;
			  };
			  i = dlen;
			}
			else
			  if(inode->i_sb->u.isofs_sb.s_mapping == 'n')
			    for (i = 0; i < dlen && i < NAME_MAX; i++) {
			      if (!(c = dpnt[i])) break;
			      if (c >= 'A' && c <= 'Z') c |= 0x20;  /* lower case */
			      if (c == '.' && i == dlen-3 && de->name[i+1] == ';' && de->name[i+2] == '1')
				break;  /* Drop trailing '.;1' (ISO9660:1988 7.5.1 requires period) */
			      if (c == ';' && i == dlen-2 && de->name[i+1] == '1') 
				break;  /* Drop trailing ';1' */
			      if (c == ';') c = '.';  /* Convert remaining ';' to '.' */
			      dpnt[i] = c;
			  };
			
			for(j=0; j<i; j++)
			  put_fs_byte(dpnt[j],j+dirent->d_name); /* And save it */
		      };
#if 0
		printk("Nchar: %d\n",i);
#endif

		if (i && i+1 < sizeof(cache.filename)) {
			cache.ino = inode_number;
			cache.dir = inode->i_ino;
			cache.dev = inode->i_dev;
			strncpy(cache.filename, dpnt, i);
			cache.dlen = dlen;
		      };

		if (rrflag) kfree(dpnt);
		if (cpnt) {
			kfree_s(cpnt, 1 << ISOFS_BLOCK_BITS);
			cpnt = NULL;
		};
		
		if (i) {
			put_fs_long(inode_number, &dirent->d_ino);
			put_fs_byte(0,i+dirent->d_name);
			put_fs_word(i,&dirent->d_reclen);
			brelse(bh);
			return i;
		}
	      }
	/* We go here for any condition we cannot handle.  We also drop through
	   to here at the end of the directory. */
 out:
	if (cpnt)
		kfree_s(cpnt, 1 << ISOFS_BLOCK_BITS);
	brelse(bh);
	return 0;
}



