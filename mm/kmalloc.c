/*
 *  linux/mm/kmalloc.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds & Roger Wolff.
 *
 *  Written by R.E. Wolff Sept/Oct '93.
 *
 */

#include <linux/mm.h>
#include <asm/system.h>
#include <linux/delay.h>

#define GFP_LEVEL_MASK 0xf

/* I want this low enough for a while to catch errors.
   I want this number to be increased in the near future:
        loadable device drivers should use this function to get memory */

#define MAX_KMALLOC_K 4


/* This defines how many times we should try to allocate a free page before
   giving up. Normally this shouldn't happen at all. */
#define MAX_GET_FREE_PAGE_TRIES 4


/* Private flags. */

#define MF_USED 0xffaa0055
#define MF_FREE 0x0055ffaa


/* 
 * Much care has gone into making these routines in this file reentrant.
 *
 * The fancy bookkeeping of nbytesmalloced and the like are only used to
 * report them to the user (oooohhhhh, aaaaahhhhh....) are not 
 * protected by cli(). (If that goes wrong. So what?)
 *
 * These routines restore the interrupt status to allow calling with ints
 * off. 
 */

/* 
 * A block header. This is in front of every malloc-block, whether free or not.
 */
struct block_header {
	unsigned long bh_flags;
	union {
		unsigned long ubh_length;
		struct block_header *fbh_next;
	} vp;
};


#define bh_length vp.ubh_length
#define bh_next   vp.fbh_next
#define BH(p) ((struct block_header *)(p))


/* 
 * The page descriptor is at the front of every page that malloc has in use. 
 */
struct page_descriptor {
	struct page_descriptor *next;
	struct block_header *firstfree;
	int order;
	int nfree;
};


#define PAGE_DESC(p) ((struct page_descriptor *)(((unsigned long)(p)) & PAGE_MASK))


/*
 * A size descriptor describes a specific class of malloc sizes.
 * Each class of sizes has its own freelist.
 */
struct size_descriptor {
	struct page_descriptor *firstfree;
	int size;
	int nblocks;

	int nmallocs;
	int nfrees;
	int nbytesmalloced;
	int npages;
};


struct size_descriptor sizes[] = { 
	{ NULL,  32,127, 0,0,0,0 },
	{ NULL,  64, 63, 0,0,0,0 },
	{ NULL, 128, 31, 0,0,0,0 },
	{ NULL, 252, 16, 0,0,0,0 },
	{ NULL, 508,  8, 0,0,0,0 },
	{ NULL,1020,  4, 0,0,0,0 },
	{ NULL,2040,  2, 0,0,0,0 },
	{ NULL,4080,  1, 0,0,0,0 },
	{ NULL,   0,  0, 0,0,0,0 }
};


#define NBLOCKS(order)          (sizes[order].nblocks)
#define BLOCKSIZE(order)        (sizes[order].size)



long kmalloc_init (long start_mem,long end_mem)
{
	int order;

/* 
 * Check the static info array. Things will blow up terribly if it's
 * incorrect. This is a late "compile time" check.....
 */
for (order = 0;BLOCKSIZE(order);order++)
    {
    if ((NBLOCKS (order)*BLOCKSIZE(order) + sizeof (struct page_descriptor)) >
        PAGE_SIZE) 
        {
        printk ("Cannot use %d bytes out of %d in order = %d block mallocs\n",
                NBLOCKS (order) * BLOCKSIZE(order) + 
                        sizeof (struct page_descriptor),
                (int) PAGE_SIZE,
                BLOCKSIZE (order));
        panic ("This only happens if someone messes with kmalloc");
        }
    }
return start_mem;
}



int get_order (int size)
{
	int order;

	/* Add the size of the header */
	size += sizeof (struct block_header); 
	for (order = 0;BLOCKSIZE(order);order++)
		if (size <= BLOCKSIZE (order))
			return order; 
	return -1;
}

void * kmalloc (size_t size, int priority)
{
	unsigned long flags;
	int order,tries,i,sz;
	struct block_header *p;
	struct page_descriptor *page;
	extern unsigned long intr_count;

/* Sanity check... */
	if (intr_count && priority != GFP_ATOMIC) {
		printk("kmalloc called nonatomically from interrupt %08lx\n",
			((unsigned long *)&size)[-1]);
		priority = GFP_ATOMIC;
	}
if (size > MAX_KMALLOC_K * 1024) 
     {
     printk ("kmalloc: I refuse to allocate %d bytes (for now max = %d).\n",
                size,MAX_KMALLOC_K*1024);
     return (NULL);
     }

order = get_order (size);
if (order < 0)
    {
    printk ("kmalloc of too large a block (%d bytes).\n",size);
    return (NULL);
    }

save_flags(flags);

/* It seems VERY unlikely to me that it would be possible that this 
   loop will get executed more than once. */
tries = MAX_GET_FREE_PAGE_TRIES; 
while (tries --)
    {
    /* Try to allocate a "recently" freed memory block */
    cli ();
    if ((page = sizes[order].firstfree) &&
        (p    =  page->firstfree))
        {
        if (p->bh_flags == MF_FREE)
            {
            page->firstfree = p->bh_next;
            page->nfree--;
            if (!page->nfree)
                {
                sizes[order].firstfree = page->next;
                page->next = NULL;
                }
            restore_flags(flags);

            sizes [order].nmallocs++;
            sizes [order].nbytesmalloced += size;
            p->bh_flags =  MF_USED; /* As of now this block is officially in use */
            p->bh_length = size;
            return p+1; /* Pointer arithmetic: increments past header */
            }
        printk ("Problem: block on freelist at %08lx isn't free.\n",(long)p);
        return (NULL);
        }
    restore_flags(flags);


    /* Now we're in trouble: We need to get a new free page..... */

    sz = BLOCKSIZE(order); /* sz is the size of the blocks we're dealing with */

    /* This can be done with ints on: This is private to this invocation */
    page = (struct page_descriptor *) __get_free_page (priority & GFP_LEVEL_MASK);
    if (!page) 
        {
        printk ("Couldn't get a free page.....\n");
        return NULL;
        }
#if 0
    printk ("Got page %08x to use for %d byte mallocs....",(long)page,sz);
#endif
    sizes[order].npages++;

    /* Loop for all but last block: */
    for (i=NBLOCKS(order),p=BH (page+1);i > 1;i--,p=p->bh_next) 
        {
        p->bh_flags = MF_FREE;
        p->bh_next = BH ( ((long)p)+sz);
        }
    /* Last block: */
    p->bh_flags = MF_FREE;
    p->bh_next = NULL;

    page->order = order;
    page->nfree = NBLOCKS(order); 
    page->firstfree = BH(page+1);
#if 0
    printk ("%d blocks per page\n",page->nfree);
#endif
    /* Now we're going to muck with the "global" freelist for this size:
       this should be uniterruptible */
    cli ();
    /* 
     * sizes[order].firstfree used to be NULL, otherwise we wouldn't be
     * here, but you never know.... 
     */
    page->next = sizes[order].firstfree;
    sizes[order].firstfree = page;
    restore_flags(flags);
    }

/* Pray that printk won't cause this to happen again :-) */

printk ("Hey. This is very funny. I tried %d times to allocate a whole\n"
        "new page for an object only %d bytes long, but some other process\n"
        "beat me to actually allocating it. Also note that this 'error'\n"
        "message is soooo very long to catch your attention. I'd appreciate\n"
        "it if you'd be so kind as to report what conditions caused this to\n"
        "the author of this kmalloc: wolff@dutecai.et.tudelft.nl.\n"
        "(Executive summary: This can't happen)\n", 
                MAX_GET_FREE_PAGE_TRIES,
                size);
return NULL;
}


void kfree_s (void *ptr,int size)
{
unsigned long flags;
int order;
register struct block_header *p=((struct block_header *)ptr) -1;
struct page_descriptor *page,*pg2;

page = PAGE_DESC (p);
order = page->order;
if ((order < 0) || 
    (order > sizeof (sizes)/sizeof (sizes[0])) ||
    (((long)(page->next)) & ~PAGE_MASK) ||
    (p->bh_flags != MF_USED))
    {
    printk ("kfree of non-kmalloced memory: %p, next= %p, order=%d\n",
                p, page->next, page->order);
    return;
    }
if (size &&
    size != p->bh_length)
    {
    printk ("Trying to free pointer at %p with wrong size: %d instead of %lu.\n",
        p,size,p->bh_length);
    return;
    }
size = p->bh_length;
p->bh_flags = MF_FREE; /* As of now this block is officially free */

save_flags(flags);
cli ();
p->bh_next = page->firstfree;
page->firstfree = p;
page->nfree ++;

if (page->nfree == 1)
   { /* Page went from full to one free block: put it on the freelist */
   if (page->next)
        {
        printk ("Page %p already on freelist dazed and confused....\n", page);
        }
   else
        {
        page->next = sizes[order].firstfree;
        sizes[order].firstfree = page;
        }
   }

/* If page is completely free, free it */
if (page->nfree == NBLOCKS (page->order))
    {
#if 0
    printk ("Freeing page %08x.\n", (long)page);
#endif
    if (sizes[order].firstfree == page)
        {
        sizes[order].firstfree = page->next;
        }
    else
        {
        for (pg2=sizes[order].firstfree;
                (pg2 != NULL) && (pg2->next != page);
                        pg2=pg2->next)
            /* Nothing */;
        if (pg2 != NULL)
            pg2->next = page->next;
        else
            printk ("Ooops. page %p doesn't show on freelist.\n", page);
        }
    free_page ((long)page);
    }
restore_flags(flags);

sizes[order].nfrees++;      /* Noncritical (monitoring) admin stuff */
sizes[order].nbytesmalloced -= size;
}
