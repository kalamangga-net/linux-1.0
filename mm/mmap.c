/*
 *	linux/mm/mmap.c
 *
 * Written by obz.
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

#include <asm/segment.h>
#include <asm/system.h>

static int anon_map(struct inode *, struct file *,
		    unsigned long, size_t, int,
		    unsigned long);
/*
 * description of effects of mapping type and prot in current implementation.
 * this is due to the current handling of page faults in memory.c. the expected
 * behavior is in parens:
 *
 * map_type	prot
 *		PROT_NONE	PROT_READ	PROT_WRITE	PROT_EXEC
 * MAP_SHARED	r: (no) yes	r: (yes) yes	r: (no) yes	r: (no) no
 *		w: (no) yes	w: (no) copy	w: (yes) yes	w: (no) no
 *		x: (no) no	x: (no) no	x: (no) no	x: (yes) no
 *		
 * MAP_PRIVATE	r: (no) yes	r: (yes) yes	r: (no) yes	r: (no) no
 *		w: (no) copy	w: (no) copy	w: (copy) copy	w: (no) no
 *		x: (no) no	x: (no) no	x: (no) no	x: (yes) no
 *
 */

#define CODE_SPACE(addr)	\
 (PAGE_ALIGN(addr) < current->start_code + current->end_code)

int do_mmap(struct file * file, unsigned long addr, unsigned long len,
	unsigned long prot, unsigned long flags, unsigned long off)
{
	int mask, error;

	if ((len = PAGE_ALIGN(len)) == 0)
		return addr;

	if (addr > TASK_SIZE || len > TASK_SIZE || addr > TASK_SIZE-len)
		return -EINVAL;

	/*
	 * do simple checking here so the lower-level routines won't have
	 * to. we assume access permissions have been handled by the open
	 * of the memory object, so we don't do any here.
	 */

	if (file != NULL)
		switch (flags & MAP_TYPE) {
		case MAP_SHARED:
			if ((prot & PROT_WRITE) && !(file->f_mode & 2))
				return -EACCES;
			/* fall through */
		case MAP_PRIVATE:
			if (!(file->f_mode & 1))
				return -EACCES;
			break;

		default:
			return -EINVAL;
		}
	/*
	 * obtain the address to map to. we verify (or select) it and ensure
	 * that it represents a valid section of the address space.
	 */

	if (flags & MAP_FIXED) {
		if (addr & ~PAGE_MASK)
			return -EINVAL;
		if (len > TASK_SIZE || addr > TASK_SIZE - len)
			return -EINVAL;
	} else {
		struct vm_area_struct * vmm;

		/* Maybe this works.. Ugly it is. */
		addr = SHM_RANGE_START;
		while (addr+len < SHM_RANGE_END) {
			for (vmm = current->mmap ; vmm ; vmm = vmm->vm_next) {
				if (addr >= vmm->vm_end)
					continue;
				if (addr + len <= vmm->vm_start)
					continue;
				addr = PAGE_ALIGN(vmm->vm_end);
				break;
			}
			if (!vmm)
				break;
		}
		if (addr+len >= SHM_RANGE_END)
			return -ENOMEM;
	}

	/*
	 * determine the object being mapped and call the appropriate
	 * specific mapper. the address has already been validated, but
	 * not unmapped, but the maps are removed from the list.
	 */
	if (file && (!file->f_op || !file->f_op->mmap))
		return -ENODEV;
	mask = 0;
	if (prot & (PROT_READ | PROT_EXEC))
		mask |= PAGE_READONLY;
	if (prot & PROT_WRITE)
		if ((flags & MAP_TYPE) == MAP_PRIVATE)
			mask |= PAGE_COPY;
		else
			mask |= PAGE_SHARED;
	if (!mask)
		return -EINVAL;

	do_munmap(addr, len);	/* Clear old maps */

	if (file)
		error = file->f_op->mmap(file->f_inode, file, addr, len, mask, off);
	else
		error = anon_map(NULL, NULL, addr, len, mask, off);
	
	if (!error)
		return addr;

	if (!current->errno)
		current->errno = -error;
	return -1;
}

asmlinkage int sys_mmap(unsigned long *buffer)
{
	int error;
	unsigned long flags;
	struct file * file = NULL;

	error = verify_area(VERIFY_READ, buffer, 6*4);
	if (error)
		return error;
	flags = get_fs_long(buffer+3);
	if (!(flags & MAP_ANONYMOUS)) {
		unsigned long fd = get_fs_long(buffer+4);
		if (fd >= NR_OPEN || !(file = current->filp[fd]))
			return -EBADF;
	}
	return do_mmap(file, get_fs_long(buffer), get_fs_long(buffer+1),
		get_fs_long(buffer+2), flags, get_fs_long(buffer+5));
}

/*
 * Normal function to fix up a mapping
 * This function is the default for when an area has no specific
 * function.  This may be used as part of a more specific routine.
 * This function works out what part of an area is affected and
 * adjusts the mapping information.  Since the actual page
 * manipulation is done in do_mmap(), none need be done here,
 * though it would probably be more appropriate.
 *
 * By the time this function is called, the area struct has been
 * removed from the process mapping list, so it needs to be
 * reinserted if necessary.
 *
 * The 4 main cases are:
 *    Unmapping the whole area
 *    Unmapping from the start of the segment to a point in it
 *    Unmapping from an intermediate point to the end
 *    Unmapping between to intermediate points, making a hole.
 *
 * Case 4 involves the creation of 2 new areas, for each side of
 * the hole.
 */
void unmap_fixup(struct vm_area_struct *area,
		 unsigned long addr, size_t len)
{
	struct vm_area_struct *mpnt;
	unsigned long end = addr + len;

	if (addr < area->vm_start || addr >= area->vm_end ||
	    end <= area->vm_start || end > area->vm_end ||
	    end < addr)
	{
		printk("unmap_fixup: area=%lx-%lx, unmap %lx-%lx!!\n",
		       area->vm_start, area->vm_end, addr, end);
		return;
	}

	/* Unmapping the whole area */
	if (addr == area->vm_start && end == area->vm_end) {
		if (area->vm_ops && area->vm_ops->close)
			area->vm_ops->close(area);
		return;
	}

	/* Work out to one of the ends */
	if (addr >= area->vm_start && end == area->vm_end)
		area->vm_end = addr;
	if (addr == area->vm_start && end <= area->vm_end) {
		area->vm_offset += (end - area->vm_start);
		area->vm_start = end;
	}

	/* Unmapping a hole */
	if (addr > area->vm_start && end < area->vm_end)
	{
		/* Add end mapping -- leave beginning for below */
		mpnt = (struct vm_area_struct *)kmalloc(sizeof(*mpnt), GFP_KERNEL);

		*mpnt = *area;
		mpnt->vm_offset += (end - area->vm_start);
		mpnt->vm_start = end;
		if (mpnt->vm_inode)
			mpnt->vm_inode->i_count++;
		insert_vm_struct(current, mpnt);
		area->vm_end = addr;	/* Truncate area */
	}

	/* construct whatever mapping is needed */
	mpnt = (struct vm_area_struct *)kmalloc(sizeof(*mpnt), GFP_KERNEL);
	*mpnt = *area;
	insert_vm_struct(current, mpnt);
}


asmlinkage int sys_mprotect(unsigned long addr, size_t len, unsigned long prot)
{
	return -EINVAL; /* Not implemented yet */
}

asmlinkage int sys_munmap(unsigned long addr, size_t len)
{
	return do_munmap(addr, len);
}

/*
 * Munmap is split into 2 main parts -- this part which finds
 * what needs doing, and the areas themselves, which do the
 * work.  This now handles partial unmappings.
 * Jeremy Fitzhardine <jeremy@sw.oz.au>
 */
int do_munmap(unsigned long addr, size_t len)
{
	struct vm_area_struct *mpnt, **npp, *free;

	if ((addr & ~PAGE_MASK) || addr > TASK_SIZE || len > TASK_SIZE-addr)
		return -EINVAL;

	if ((len = PAGE_ALIGN(len)) == 0)
		return 0;

	/*
	 * Check if this memory area is ok - put it on the temporary
	 * list if so..  The checks here are pretty simple --
	 * every area affected in some way (by any overlap) is put
	 * on the list.  If nothing is put on, nothing is affected.
	 */
	npp = &current->mmap;
	free = NULL;
	for (mpnt = *npp; mpnt != NULL; mpnt = *npp) {
		unsigned long end = addr+len;

		if ((addr < mpnt->vm_start && end <= mpnt->vm_start) ||
		    (addr >= mpnt->vm_end && end > mpnt->vm_end))
		{
			npp = &mpnt->vm_next;
			continue;
		}

		*npp = mpnt->vm_next;
		mpnt->vm_next = free;
		free = mpnt;
	}

	if (free == NULL)
		return 0;

	/*
	 * Ok - we have the memory areas we should free on the 'free' list,
	 * so release them, and unmap the page range..
	 * If the one of the segments is only being partially unmapped,
	 * it will put new vm_area_struct(s) into the address space.
	 */
	while (free) {
		unsigned long st, end;

		mpnt = free;
		free = free->vm_next;

		st = addr < mpnt->vm_start ? mpnt->vm_start : addr;
		end = addr+len;
		end = end > mpnt->vm_end ? mpnt->vm_end : end;

		if (mpnt->vm_ops && mpnt->vm_ops->unmap)
			mpnt->vm_ops->unmap(mpnt, st, end-st);
		else
			unmap_fixup(mpnt, st, end-st);

		kfree(mpnt);
	}

	unmap_page_range(addr, len);
	return 0;
}

/* This is used for a general mmap of a disk file */
int generic_mmap(struct inode * inode, struct file * file,
	unsigned long addr, size_t len, int prot, unsigned long off)
{
  	struct vm_area_struct * mpnt;
	extern struct vm_operations_struct file_mmap;
	struct buffer_head * bh;

	if (prot & PAGE_RW)	/* only PAGE_COW or read-only supported right now */
		return -EINVAL;
	if (off & (inode->i_sb->s_blocksize - 1))
		return -EINVAL;
	if (!inode->i_sb || !S_ISREG(inode->i_mode))
		return -EACCES;
	if (!inode->i_op || !inode->i_op->bmap)
		return -ENOEXEC;
	if (!(bh = bread(inode->i_dev,bmap(inode,0),inode->i_sb->s_blocksize)))
		return -EACCES;
	if (!IS_RDONLY(inode)) {
		inode->i_atime = CURRENT_TIME;
		inode->i_dirt = 1;
	}
	brelse(bh);

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
	mpnt->vm_ops = &file_mmap;
	insert_vm_struct(current, mpnt);
	merge_segments(current->mmap, NULL, NULL);
	
	return 0;
}

/*
 * Insert vm structure into process list
 * This makes sure the list is sorted by start address, and
 * some some simple overlap checking.
 * JSGF
 */
void insert_vm_struct(struct task_struct *t, struct vm_area_struct *vmp)
{
	struct vm_area_struct **nxtpp, *mpnt;

	nxtpp = &t->mmap;
	
	for(mpnt = t->mmap; mpnt != NULL; mpnt = mpnt->vm_next)
	{
		if (mpnt->vm_start > vmp->vm_start)
			break;
		nxtpp = &mpnt->vm_next;

		if ((vmp->vm_start >= mpnt->vm_start &&
		     vmp->vm_start < mpnt->vm_end) ||
		    (vmp->vm_end >= mpnt->vm_start &&
		     vmp->vm_end < mpnt->vm_end))
			printk("insert_vm_struct: ins area %lx-%lx in area %lx-%lx\n",
			       vmp->vm_start, vmp->vm_end,
			       mpnt->vm_start, vmp->vm_end);
	}
	
	vmp->vm_next = mpnt;

	*nxtpp = vmp;
}

/*
 * Merge a list of memory segments if possible.
 * Redundant vm_area_structs are freed.
 * This assumes that the list is ordered by address.
 */
void merge_segments(struct vm_area_struct *mpnt,
		    map_mergep_fnp mergep, void *mpd)
{
	struct vm_area_struct *prev, *next;

	if (mpnt == NULL)
		return;
	
	for(prev = mpnt, mpnt = mpnt->vm_next;
	    mpnt != NULL;
	    prev = mpnt, mpnt = next)
	{
		int mp;

		next = mpnt->vm_next;
		
		if (mergep == NULL)
		{
			unsigned long psz = prev->vm_end - prev->vm_start;
			mp = prev->vm_offset + psz == mpnt->vm_offset;
		}
		else
			mp = (*mergep)(prev, mpnt, mpd);

		/*
		 * Check they are compatible.
		 * and the like...
		 * What does the share pointer mean?
		 */
		if (prev->vm_ops != mpnt->vm_ops ||
		    prev->vm_page_prot != mpnt->vm_page_prot ||
		    prev->vm_inode != mpnt->vm_inode ||
		    prev->vm_end != mpnt->vm_start ||
		    !mp ||
		    prev->vm_share != mpnt->vm_share ||		/* ?? */
		    prev->vm_next != mpnt)			/* !!! */
			continue;

		/*
		 * merge prev with mpnt and set up pointers so the new
		 * big segment can possibly merge with the next one.
		 * The old unused mpnt is freed.
		 */
		prev->vm_end = mpnt->vm_end;
		prev->vm_next = mpnt->vm_next;
		kfree_s(mpnt, sizeof(*mpnt));
		mpnt = prev;
	}
}

/*
 * Map memory not associated with any file into a process
 * address space.  Adjecent memory is merged.
 */
static int anon_map(struct inode *ino, struct file * file,
		    unsigned long addr, size_t len, int mask,
		    unsigned long off)
{
  	struct vm_area_struct * mpnt;

	if (zeromap_page_range(addr, len, mask))
		return -ENOMEM;

	mpnt = (struct vm_area_struct * ) kmalloc(sizeof(struct vm_area_struct), GFP_KERNEL);
	if (!mpnt)
		return -ENOMEM;

	mpnt->vm_task = current;
	mpnt->vm_start = addr;
	mpnt->vm_end = addr + len;
	mpnt->vm_page_prot = mask;
	mpnt->vm_share = NULL;
	mpnt->vm_inode = NULL;
	mpnt->vm_offset = 0;
	mpnt->vm_ops = NULL;
	insert_vm_struct(current, mpnt);
	merge_segments(current->mmap, ignoff_mergep, NULL);

	return 0;
}

/* Merge, ignoring offsets */
int ignoff_mergep(const struct vm_area_struct *m1,
		  const struct vm_area_struct *m2,
		  void *data)
{
	if (m1->vm_inode != m2->vm_inode)	/* Just to be sure */
		return 0;

	return (struct inode *)data == m1->vm_inode;
}
