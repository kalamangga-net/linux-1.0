/*
 * malloc.c --- a general purpose kernel memory allocator for Linux.
 *
 * Written by Theodore Ts'o (tytso@mit.edu), 11/29/91
 *
 * This routine is written to be as fast as possible, so that it
 * can be called from the interrupt level.
 *
 * Limitations: maximum size of memory we can allocate using this routine
 *	is 4k, the size of a page in Linux.
 *
 * The general game plan is that each page (called a bucket) will only hold
 * objects of a given size.  When all of the object on a page are released,
 * the page can be returned to the general free pool.  When kmalloc() is
 * called, it looks for the smallest bucket size which will fulfill its
 * request, and allocate a piece of memory from that bucket pool.
 *
 * Each bucket has as its control block a bucket descriptor which keeps
 * track of how many objects are in use on that page, and the free list
 * for that page.  Like the buckets themselves, bucket descriptors are
 * stored on pages requested from get_free_page().  However, unlike buckets,
 * pages devoted to bucket descriptor pages are never released back to the
 * system.  Fortunately, a system should probably only need 1 or 2 bucket
 * descriptor pages, since a page can hold 256 bucket descriptors (which
 * corresponds to 1 megabyte worth of bucket pages.)  If the kernel is using
 * that much allocated memory, it's probably doing something wrong.  :-)
 *
 * Note: kmalloc() and kfree() both call get_free_page() and free_page()
 *	in sections of code where interrupts are turned off, to allow
 *	kmalloc() and kfree() to be safely called from an interrupt routine.
 *	(We will probably need this functionality when networking code,
 *	particularily things like NFS, is added to Linux.)  However, this
 *	presumes that get_free_page() and free_page() are interrupt-level
 *	safe, which they may not be once paging is added.  If this is the
 *	case, we will need to modify kmalloc() to keep a few unused pages
 *	"pre-allocated" so that it can safely draw upon those pages if
 * 	it is called from an interrupt routine.
 *
 * 	Another concern is that get_free_page() should not sleep; if it
 *	does, the code is carefully ordered so as to avoid any race
 *	conditions.  The catch is that if kmalloc() is called re-entrantly,
 *	there is a chance that unecessary pages will be grabbed from the
 *	system.  Except for the pages for the bucket descriptor page, the
 *	extra pages will eventually get released back to the system, though,
 *	so it isn't all that bad.
 */

/* I'm going to modify it to keep some free pages around.  Get free page
   can sleep, and tcp/ip needs to call kmalloc at interrupt time  (Or keep
   big buffers around for itself.)  I guess I'll have return from
   syscall fill up the free page descriptors. -RAB */

/* since the advent of GFP_ATOMIC, I've changed the kmalloc code to
   use it and return NULL if it can't get a page. -RAB  */
/* (mostly just undid the previous changes -RAB) */

/* I've added the priority argument to kmalloc so routines can
   sleep on memory if they want. - RAB */

/* I've also got to make sure that kmalloc is reentrant now. */

/* Debugging support: add file/line info, add beginning+end markers. -M.U- */

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/malloc.h>

#include <asm/system.h>

struct bucket_desc {	/* 16 bytes */
	void			*page;
	struct bucket_desc	*next;
	void			*freeptr;
	unsigned short		refcnt;
	unsigned short		bucket_size;
};

struct _bucket_dir {	/* 8 bytes */
	unsigned int		size;
	struct bucket_desc	*chain;
};

#ifdef CONFIG_DEBUG_MALLOC

struct hdr_start {
	const char *file;
	const char *ok_file;
	unsigned short line;
	unsigned short ok_line;
	unsigned short size;
	int magic;
};
struct hdr_end {
	int magic;
};

#define DEB_MAGIC_FREE  0x13579BDF /* free block */
#define DEB_MAGIC_ALLOC 0x2468ACE0 /* allocated block */
#define DEB_MAGIC_USED  0x147AD036 /* allocated but bad */
#define DEB_MAGIC_FREED 0x258BE169 /* free but abused */

#define DEB_MAGIC_END   0x369CF258 /* end marker */

#endif
/*
 * The following is the where we store a pointer to the first bucket
 * descriptor for a given size.
 *
 * If it turns out that the Linux kernel allocates a lot of objects of a
 * specific size, then we may want to add that specific size to this list,
 * since that will allow the memory to be allocated more efficiently.
 * However, since an entire page must be dedicated to each specific size
 * on this list, some amount of temperance must be exercised here.
 *
 * Note that this list *must* be kept in order.
 */
struct _bucket_dir bucket_dir[] = {
#ifndef CONFIG_DEBUG_MALLOC /* Debug headers have too much overhead */
	{ 16,	(struct bucket_desc *) 0},
#endif
	{ 32,	(struct bucket_desc *) 0},
	{ 64,	(struct bucket_desc *) 0},
	{ 128,	(struct bucket_desc *) 0},
	{ 256,	(struct bucket_desc *) 0},
	{ 512,	(struct bucket_desc *) 0},
	{ 1024,	(struct bucket_desc *) 0},
	{ 2048, (struct bucket_desc *) 0},
	{ 4096, (struct bucket_desc *) 0},
	{ 0,    (struct bucket_desc *) 0}};   /* End of list marker */

/*
 * This contains a linked list of free bucket descriptor blocks
 */
static struct bucket_desc *free_bucket_desc = (struct bucket_desc *) 0;

/*
 * This routine initializes a bucket description page.
 */

/* It assumes it is called with interrupts on. and will
   return that way.  It also can sleep if priority != GFP_ATOMIC. */
 
static inline void init_bucket_desc(unsigned long page)
{
	struct bucket_desc *bdesc;
	int i;

	bdesc = (struct bucket_desc *) page;
	for (i = PAGE_SIZE/sizeof(struct bucket_desc); --i > 0; bdesc++ )
		bdesc->next = bdesc+1;
	/*
	 * This is done last, to avoid race conditions in case
	 * get_free_page() sleeps and this routine gets called again....
	 */
	cli();
	bdesc->next = free_bucket_desc;
	free_bucket_desc = (struct bucket_desc *) page;
}

/*
 * Re-organized some code to give cleaner assembly output for easier
 * verification.. LBT
 */
#ifdef CONFIG_DEBUG_MALLOC
void *
deb_kmalloc(const char *deb_file, unsigned short deb_line,
	unsigned int len, int priority)
#else
void *
kmalloc(unsigned int len, int priority)
#endif
{
	int i;
	unsigned long		flags;
	unsigned long		page;
	struct _bucket_dir	*bdir;
	struct bucket_desc	*bdesc;
	void			*retval;

#ifdef CONFIG_DEBUG_MALLOC
	len += sizeof(struct hdr_start)+sizeof(struct hdr_end);
#endif
	/*
	 * First we search the bucket_dir to find the right bucket change
	 * for this request.
	 */

	/* The sizes are static so there is no reentry problem here. */
	bdir = bucket_dir;
	for (bdir = bucket_dir ; bdir->size < len ; bdir++) {
		if (!bdir->size)
			goto too_large;
	}

	/*
	 * Now we search for a bucket descriptor which has free space
	 */
	save_flags(flags);
	cli();			/* Avoid race conditions */
	for (bdesc = bdir->chain; bdesc != NULL; bdesc = bdesc->next)
		if (bdesc->freeptr)
			goto found_bdesc;
	/*
	 * If we didn't find a bucket with free space, then we'll
	 * allocate a new one.
	 */
	
	/*
	 * Note that init_bucket_descriptor() does its
	 * own cli() before returning, and guarantees that
	 * there is a bucket desc in the page.
	 */
	if (!free_bucket_desc) {
		restore_flags(flags);
		if(!(page=__get_free_page(priority)))
			return NULL;
		init_bucket_desc(page);
	}
	
	bdesc = free_bucket_desc;
	free_bucket_desc = bdesc->next;
	restore_flags(flags);

	if(!(page=__get_free_page(priority))) {
	/*
	 * Out of memory? Put the bucket descriptor back on the free list
	 */
		cli();
		bdesc->next = free_bucket_desc;
		free_bucket_desc = bdesc;
		restore_flags(flags);
		return NULL;
	}
		
	bdesc->refcnt = 0;
	bdesc->bucket_size = bdir->size;
	bdesc->page = bdesc->freeptr = (void *) page;
	
	/* Set up the chain of free objects */
	for (i=PAGE_SIZE/bdir->size; i > 0 ; i--) {
#ifdef CONFIG_DEBUG_MALLOC
		struct hdr_start *hd;
		struct hdr_end *he;
		hd = (struct hdr_start *) page;
		he = (struct hdr_end *)(page+(bdir->size-sizeof(struct hdr_end)));
		hd->magic = DEB_MAGIC_FREE;
		hd->file = hd->ok_file = "(expand)"; 
		hd->line = hd->ok_line = 0;
		hd->size = bdir->size-sizeof(struct hdr_start)-sizeof(struct hdr_end);
		he->magic = DEB_MAGIC_END;

		memset(hd+1,0xF8,hd->size);

		*((void **) (hd+1)) = (i==1) ? NULL : (void *)(page + bdir->size);
#else
		*((void **) page) = (i==1) ? NULL : (void *)(page + bdir->size);
#endif
		page += bdir->size;
	}
	
	/* turn interrupts back off for putting the
	   thing onto the chain. */
	cli();
	/* remember bdir is not changed. */
	bdesc->next = bdir->chain; /* OK, link it in! */
	bdir->chain = bdesc;

found_bdesc:
	retval = (void *) bdesc->freeptr;
#ifdef CONFIG_DEBUG_MALLOC
	bdesc->freeptr = *((void **) (((char *)retval)+sizeof(struct hdr_start)));
#else
	bdesc->freeptr = *((void **) retval);
#endif
	bdesc->refcnt++;
	restore_flags(flags);	/* OK, we're safe again */
#ifdef CONFIG_DEBUG_MALLOC
	{
		struct hdr_start *hd;
		struct hdr_end *he;

		hd = (struct hdr_start *) retval;
		retval = hd+1;
		len -= sizeof(struct hdr_start)+sizeof(struct hdr_end);
		if(hd->magic != DEB_MAGIC_FREE && hd->magic != DEB_MAGIC_FREED) {
			printk("DEB_MALLOC allocating %s block 0x%x (head 0x%x) from %s:%d, magic %x\n",
				(hd->magic == DEB_MAGIC_ALLOC) ? "nonfree" : "trashed", 
				retval,hd,deb_file,deb_line,hd->magic);
			return NULL;
		}
		if(len > hd->size || len > bdir->size-sizeof(struct hdr_start)-sizeof(struct hdr_end)) {
			printk("DEB_MALLOC got %x:%x-byte block, wanted %x, from %s:%d, last %s:%d\n",
				hd->size,bdir->size,len,hd->file,hd->line,deb_file,deb_line);
			return NULL;
		}
		{
			unsigned char *x = (unsigned char *) retval;
			unsigned short pos = 4;
			x += pos;
			while(pos < hd->size) {
				if(*x++ != 0xF8) {
					printk("DEB_MALLOC used 0x%x:%x(%x) while free, from %s:%d\n",
						retval,pos,hd->size,hd->file,hd->line);
					return NULL;
				}
				pos++;
			}
		}
		he = (struct hdr_end *)(((char *)retval)+hd->size);
		if(he->magic != DEB_MAGIC_END) {
			printk("DEB_MALLOC overran 0x%x:%d while free, from %s:%d\n",retval,hd->size,hd->file,hd->line);
		}
		memset(retval, 0xf0, len);
		he = (struct hdr_end *)(((char *)retval)+len);
		hd->file = hd->ok_file = deb_file;
		hd->line = hd->ok_line = deb_line;
		hd->size = len;
		hd->magic = DEB_MAGIC_ALLOC;
		he->magic = DEB_MAGIC_END;
	}
#endif
	return retval;

too_large:
       /* This should be changed for sizes > 1 page. */
	printk("kmalloc called with impossibly large argument (%d)\n", len);
	return NULL;
}

#ifdef CONFIG_DEBUG_MALLOC
void deb_kcheck_s(const char *deb_file, unsigned short deb_line,
	void *obj, int size)
{
	struct hdr_start *hd;
	struct hdr_end *he;

	if (!obj)
		return;
	hd = (struct hdr_start *) obj;
	hd--;

	if(hd->magic != DEB_MAGIC_ALLOC) {
		if(hd->magic == DEB_MAGIC_FREE) {
			printk("DEB_MALLOC Using free block of 0x%x at %s:%d, by %s:%d, wasOK %s:%d\n",
				obj,deb_file,deb_line,hd->file,hd->line,hd->ok_file,hd->ok_line);
			/* For any other condition it is either superfluous or dangerous to print something. */
			hd->magic = DEB_MAGIC_FREED;
		}
		return;
	}
	if(hd->size != size) {
		if(size != 0) {
			printk("DEB_MALLOC size for 0x%x given as %d, stored %d, at %s:%d, wasOK %s:%d\n",
				obj,size,hd->size,deb_file,deb_line,hd->ok_file,hd->ok_line);
		}
		size = hd->size;
	}
	he = (struct hdr_end *)(((char *)obj)+size);
	if(he->magic != DEB_MAGIC_END) {
		printk("DEB_MALLOC overran block 0x%x:%d, at %s:%d, wasOK %s:%d\n",
			obj,hd->size,deb_file,deb_line,hd->ok_file,hd->ok_line);
		hd->magic = DEB_MAGIC_USED;
		return;
	}
	hd->ok_file = deb_file;
	hd->ok_line = deb_line;
}
#endif

/*
 * Here is the kfree routine.  If you know the size of the object that you
 * are freeing, then kfree_s() will use that information to speed up the
 * search for the bucket descriptor.
 *
 * We will #define a macro so that "kfree(x)" is becomes "kfree_s(x, 0)"
 */
#ifdef CONFIG_DEBUG_MALLOC
void deb_kfree_s(const char *deb_file, unsigned short deb_line,
	void *obj, int size)
#else
void kfree_s(void *obj, int size)
#endif
{
	unsigned long		flags;
	void			*page;
	struct _bucket_dir	*bdir;
	struct bucket_desc	*bdesc, *prev;

	if (!obj)
		return;
#ifdef CONFIG_DEBUG_MALLOC
	{
		struct hdr_start *hd;
		struct hdr_end *he;
		hd = (struct hdr_start *) obj;
		hd--;

		if(hd->magic == DEB_MAGIC_FREE) {
			printk("DEB_MALLOC dup free of 0x%x at %s:%d by %s:%d, wasOK %s:%d\n",
					obj,deb_file,deb_line,hd->file,hd->line,hd->ok_file,hd->ok_line);
			return;
		}
		if(hd->size != size) {
			if(size != 0) {
				if(hd->magic != DEB_MAGIC_USED)
					printk("DEB_MALLOC size for 0x%x given as %d, stored %d, at %s:%d, wasOK %s:%d\n",
						obj,size,hd->size,deb_file,deb_line,hd->ok_file,hd->ok_line);
			}
			size = hd->size;
		}
		he = (struct hdr_end *)(((char *)obj)+size);
		if(he->magic != DEB_MAGIC_END) {
			if(hd->magic != DEB_MAGIC_USED)
				printk("DEB_MALLOC overran block 0x%x:%d, at %s:%d, from %s:%d, wasOK %s:%d\n",
					obj,hd->size,deb_file,deb_line,hd->file,hd->line,hd->ok_file,hd->ok_line);
		}
		size += sizeof(struct hdr_start)+sizeof(struct hdr_end);
	}
#endif
	save_flags(flags);
	/* Calculate what page this object lives in */
	page = (void *)  ((unsigned long) obj & PAGE_MASK);

	/* Now search the buckets looking for that page */
	for (bdir = bucket_dir; bdir->size; bdir++) {
	    prev = 0;
	    /* If size is zero then this conditional is always true */
	    if (bdir->size >= size) {
		/* We have to turn off interrupts here because
		   we are descending the chain.  If something
		   changes it in the middle we could suddenly
		   find ourselves descending the free list.
		   I think this would only cause a memory
		   leak, but better safe than sorry. */
		cli(); /* To avoid race conditions */
		for (bdesc = bdir->chain; bdesc; bdesc = bdesc->next) {
		    if (bdesc->page == page)
			goto found;
		    prev = bdesc;
		}
	    }
	}

	restore_flags(flags);
	printk("Bad address passed to kernel kfree_s(%p, %d)\n",obj, size);
#ifdef CONFIG_DEBUG_MALLOC
	printk("Offending code: %s:%d\n",deb_file,deb_line);
#else
	printk("Offending eip: %08x\n",((unsigned long *) &obj)[-1]);
#endif
	return;

found:
	/* interrupts are off here. */
#ifdef CONFIG_DEBUG_MALLOC

	{
		struct hdr_start *hd;
		struct hdr_end *he;
		hd = (struct hdr_start *) obj;
		hd--;
		
		hd->file = deb_file;
		hd->line = deb_line;
		hd->magic = DEB_MAGIC_FREE;
		hd->size = bdir->size-sizeof(struct hdr_start)-sizeof(struct hdr_end);
		he = (struct hdr_end *)(((char *)obj)+hd->size);
		memset(obj, 0xf8, hd->size);
		he->magic = DEB_MAGIC_END;
		*((void **)obj) = bdesc->freeptr;
		obj = hd;
	}
#else
	*((void **)obj) = bdesc->freeptr;
#endif

	bdesc->freeptr = obj;
	bdesc->refcnt--;
	if (bdesc->refcnt == 0) {
		/*
		 * We need to make sure that prev is still accurate.  It
		 * may not be, if someone rudely interrupted us....
		 */
		if ((prev && (prev->next != bdesc)) ||
		    (!prev && (bdir->chain != bdesc)))
			for (prev = bdir->chain; prev; prev = prev->next)
				if (prev->next == bdesc)
					break;
		if (prev)
			prev->next = bdesc->next;
		else {
			if (bdir->chain != bdesc)
				panic("kmalloc bucket chains corrupted");
			bdir->chain = bdesc->next;
		}
		bdesc->next = free_bucket_desc;
		free_bucket_desc = bdesc;
		free_page((unsigned long) bdesc->page);
	}
	restore_flags(flags);
	return;
}

#ifdef CONFIG_DEBUG_MALLOC
int get_malloc(char *buffer)
{
	int len = 0;
	int i;
	unsigned long		flags;
	void			*page;
	struct _bucket_dir	*bdir;
	struct bucket_desc	*bdesc;

	save_flags(flags);
	cli(); /* To avoid race conditions */
	for (bdir = bucket_dir; bdir->size; bdir++) {
		for (bdesc = bdir->chain; bdesc; bdesc = bdesc->next) {
			page = bdesc->page;
			for (i=PAGE_SIZE/bdir->size; i > 0 ; i--) {
				struct hdr_start *hd;
				hd = (struct hdr_start *)page;
				if(hd->magic == DEB_MAGIC_ALLOC) {
					if(len > PAGE_SIZE-80) {
						restore_flags(flags);
						len += sprintf(buffer+len,"...\n");
						return len;
					}
					len += sprintf(buffer+len,"%08x:%03x %s:%d %s:%d\n",
						(long)(page+sizeof(struct hdr_start)),hd->size,hd->file,hd->line,hd->ok_file,hd->ok_line);
				}
				page += bdir->size;
			}
		}
	}

	restore_flags(flags);
	return len;
}
#endif
