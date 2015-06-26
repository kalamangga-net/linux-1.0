/*
 *	fs/nfs/mmap.c	by Jon Tombs 15 Aug 1993
 *
 * This code is from
 *	linux/mm/mmap.c which was written by obz, Linus and Eric
 * and
 *	linux/mm/memory.c  by Linus Torvalds and others
 *
 *	Copyright (C) 1993
 *
 */
#include <linux/stat.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/shm.h>
#include <linux/errno.h>
#include <linux/mman.h>
#include <linux/string.h>
#include <linux/malloc.h>
#include <linux/nfs_fs.h>

#include <asm/segment.h>
#include <asm/system.h>

extern int share_page(struct vm_area_struct * area, struct task_struct * tsk,
	struct inode * inode, unsigned long address, unsigned long error_code,
	unsigned long newpage);

extern unsigned long put_page(struct task_struct * tsk,unsigned long page,
	unsigned long address,int prot);

static void nfs_file_mmap_nopage(int error_code, struct vm_area_struct * area,
				unsigned long address);

extern void file_mmap_free(struct vm_area_struct * area);
extern int file_mmap_share(struct vm_area_struct * from, struct vm_area_struct * to,
				unsigned long address);

struct vm_operations_struct nfs_file_mmap = {
	NULL,			/* open */
	file_mmap_free,		/* close */
	nfs_file_mmap_nopage,	/* nopage */
	NULL,			/* wppage */
	file_mmap_share,	/* share */
	NULL,			/* unmap */
};


/* This is used for a general mmap of a nfs file */
int nfs_mmap(struct inode * inode, struct file * file,
	unsigned long addr, size_t len, int prot, unsigned long off)
{
	struct vm_area_struct * mpnt;

	if (prot & PAGE_RW)	/* only PAGE_COW or read-only supported now */
		return -EINVAL;
	if (off & (inode->i_sb->s_blocksize - 1))
		return -EINVAL;
	if (!inode->i_sb || !S_ISREG(inode->i_mode))
		return -EACCES;
	if (!IS_RDONLY(inode)) {
		inode->i_atime = CURRENT_TIME;
		inode->i_dirt = 1;
	}

	mpnt = (struct vm_area_struct * ) kmalloc(sizeof(struct vm_area_struct), GFP_KERNEL);
	if (!mpnt)
		return -ENOMEM;

	unmap_page_range(addr, len);
	mpnt->vm_task = current;
	mpnt->vm_start = addr;
	mpnt->vm_end = addr + len;
	mpnt->vm_page_prot = prot;
	mpnt->vm_share = NULL;
	mpnt->vm_inode = inode;
	inode->i_count++;
	mpnt->vm_offset = off;
	mpnt->vm_ops = &nfs_file_mmap;
	insert_vm_struct(current, mpnt);
	merge_segments(current->mmap, NULL, NULL);
	return 0;
}


static void nfs_file_mmap_nopage(int error_code, struct vm_area_struct * area,
				unsigned long address)
{
	struct inode * inode = area->vm_inode;
	unsigned int clear;
	unsigned long page;
	unsigned long tmp;
	int n;
	int i;
	int pos;
	struct nfs_fattr fattr;

	address &= PAGE_MASK;
	pos = address - area->vm_start + area->vm_offset;

	page = get_free_page(GFP_KERNEL);
	if (share_page(area, area->vm_task, inode, address, error_code, page)) {
		++area->vm_task->min_flt;
		return;
	}

	++area->vm_task->maj_flt;
	if (!page) {
		oom(current);
		put_page(area->vm_task, BAD_PAGE, address, PAGE_PRIVATE);
		return;
	}

	clear = 0;
	if (address + PAGE_SIZE > area->vm_end) {
		clear = address + PAGE_SIZE - area->vm_end;
	}

	n = NFS_SERVER(inode)->rsize; /* what we can read in one go */

	for (i = 0; i < (PAGE_SIZE - clear); i += n) {
		int hunk, result;

		hunk = PAGE_SIZE - i;
		if (hunk > n)
			hunk = n;
		result = nfs_proc_read(NFS_SERVER(inode), NFS_FH(inode),
			pos, hunk, (char *) (page + i), &fattr);
		if (result < 0)
			break;
		pos += result;
		if (result < n) {
			i += result;
			break;
		}
	}

#ifdef doweneedthishere
	nfs_refresh_inode(inode, &fattr);
#endif

	if (!(error_code & PAGE_RW)) {
		if (share_page(area, area->vm_task, inode, address, error_code, page))
			return;
	}

	tmp = page + PAGE_SIZE;
	while (clear--) {
		*(char *)--tmp = 0;
	}
	if (put_page(area->vm_task,page,address,area->vm_page_prot))
		return;
	free_page(page);
	oom(current);
}
