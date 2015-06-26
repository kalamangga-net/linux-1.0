#ifndef _LINUX_MM_H
#define _LINUX_MM_H

#include <linux/page.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/kernel.h>

#define VERIFY_READ 0
#define VERIFY_WRITE 1

int __verify_write(unsigned long addr, unsigned long count);

extern inline int verify_area(int type, const void * addr, unsigned long size)
{
	if (TASK_SIZE <= (unsigned long) addr)
		return -EFAULT;
	if (size > TASK_SIZE - (unsigned long) addr)
		return -EFAULT;
	if (wp_works_ok || type == VERIFY_READ || !size)
		return 0;
	return __verify_write((unsigned long) addr,size);
}

/*
 * Linux kernel virtual memory manager primitives.
 * The idea being to have a "virtual" mm in the same way
 * we have a virtual fs - giving a cleaner interface to the
 * mm details, and allowing different kinds of memory mappings
 * (from shared memory to executable loading to arbitrary
 * mmap() functions).
 */

/*
 * This struct defines a memory VMM memory area. There is one of these
 * per VM-area/task.  A VM area is any part of the process virtual memory
 * space that has a special rule for the page-fault handlers (ie a shared
 * library, the executable area etc).
 */
struct vm_area_struct {
	struct task_struct * vm_task;		/* VM area parameters */
	unsigned long vm_start;
	unsigned long vm_end;
	unsigned short vm_page_prot;
	struct vm_area_struct * vm_next;	/* linked list */
	struct vm_area_struct * vm_share;	/* linked list */
	struct inode * vm_inode;
	unsigned long vm_offset;
	struct vm_operations_struct * vm_ops;
};

/*
 * These are the virtual MM functions - opening of an area, closing it (needed to
 * keep files on disk up-to-date etc), pointer to the functions called when a
 * no-page or a wp-page exception occurs, and the function which decides on sharing
 * of pages between different processes.
 */
struct vm_operations_struct {
	void (*open)(struct vm_area_struct * area);
	void (*close)(struct vm_area_struct * area);
	void (*nopage)(int error_code,
		       struct vm_area_struct * area, unsigned long address);
	void (*wppage)(struct vm_area_struct * area, unsigned long address);
	int (*share)(struct vm_area_struct * from, struct vm_area_struct * to, unsigned long address);
	int (*unmap)(struct vm_area_struct *area, unsigned long, size_t);
};

extern unsigned long __bad_page(void);
extern unsigned long __bad_pagetable(void);
extern unsigned long __zero_page(void);

#define BAD_PAGETABLE __bad_pagetable()
#define BAD_PAGE __bad_page()
#define ZERO_PAGE __zero_page()

extern volatile short free_page_ptr; /* used by malloc and tcp/ip. */

extern int nr_swap_pages;
extern int nr_free_pages;
extern unsigned long free_page_list;
extern int nr_secondary_pages;
extern unsigned long secondary_page_list;

#define MAX_SECONDARY_PAGES 20

/*
 * This is timing-critical - most of the time in getting a new page
 * goes to clearing the page. If you want a page without the clearing
 * overhead, just use __get_free_page() directly..
 */
extern unsigned long __get_free_page(int priority);
extern inline unsigned long get_free_page(int priority)
{
	unsigned long page;

	page = __get_free_page(priority);
	if (page)
		__asm__ __volatile__("rep ; stosl"
			: /* no outputs */ \
			:"a" (0),"c" (1024),"D" (page)
			:"di","cx");
	return page;
}

/* memory.c */

extern void free_page(unsigned long addr);
extern unsigned long put_dirty_page(struct task_struct * tsk,unsigned long page,
	unsigned long address);
extern void free_page_tables(struct task_struct * tsk);
extern void clear_page_tables(struct task_struct * tsk);
extern int copy_page_tables(struct task_struct * to);
extern int clone_page_tables(struct task_struct * to);
extern int unmap_page_range(unsigned long from, unsigned long size);
extern int remap_page_range(unsigned long from, unsigned long to, unsigned long size, int mask);
extern int zeromap_page_range(unsigned long from, unsigned long size, int mask);

extern void do_wp_page(unsigned long error_code, unsigned long address,
	struct task_struct *tsk, unsigned long user_esp);
extern void do_no_page(unsigned long error_code, unsigned long address,
	struct task_struct *tsk, unsigned long user_esp);

extern unsigned long paging_init(unsigned long start_mem, unsigned long end_mem);
extern void mem_init(unsigned long low_start_mem,
		     unsigned long start_mem, unsigned long end_mem);
extern void show_mem(void);
extern void oom(struct task_struct * task);
extern void si_meminfo(struct sysinfo * val);

/* vmalloc.c */

extern void * vmalloc(unsigned long size);
extern void vfree(void * addr);
extern int vread(char *buf, char *addr, int count);

/* swap.c */

extern void swap_free(unsigned long page_nr);
extern unsigned long swap_duplicate(unsigned long page_nr);
extern void swap_in(unsigned long *table_ptr);
extern void si_swapinfo(struct sysinfo * val);
extern void rw_swap_page(int rw, unsigned long nr, char * buf);

/* mmap.c */
extern int do_mmap(struct file * file, unsigned long addr, unsigned long len,
	unsigned long prot, unsigned long flags, unsigned long off);
typedef int (*map_mergep_fnp)(const struct vm_area_struct *,
			      const struct vm_area_struct *, void *);
extern void merge_segments(struct vm_area_struct *, map_mergep_fnp, void *);
extern void insert_vm_struct(struct task_struct *, struct vm_area_struct *);
extern int ignoff_mergep(const struct vm_area_struct *,
			 const struct vm_area_struct *, void *);
extern int do_munmap(unsigned long, size_t);

#define read_swap_page(nr,buf) \
	rw_swap_page(READ,(nr),(buf))
#define write_swap_page(nr,buf) \
	rw_swap_page(WRITE,(nr),(buf))

#define invalidate() \
__asm__ __volatile__("movl %%cr3,%%eax\n\tmovl %%eax,%%cr3": : :"ax")

extern unsigned long high_memory;

#define MAP_NR(addr) ((addr) >> PAGE_SHIFT)
#define MAP_PAGE_RESERVED (1<<15)

extern unsigned short * mem_map;

#define PAGE_PRESENT	0x001
#define PAGE_RW		0x002
#define PAGE_USER	0x004
#define PAGE_PWT	0x008	/* 486 only - not used currently */
#define PAGE_PCD	0x010	/* 486 only - not used currently */
#define PAGE_ACCESSED	0x020
#define PAGE_DIRTY	0x040
#define PAGE_COW	0x200	/* implemented in software (one of the AVL bits) */

#define PAGE_PRIVATE	(PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_ACCESSED | PAGE_COW)
#define PAGE_SHARED	(PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_ACCESSED)
#define PAGE_COPY	(PAGE_PRESENT | PAGE_USER | PAGE_ACCESSED | PAGE_COW)
#define PAGE_READONLY	(PAGE_PRESENT | PAGE_USER | PAGE_ACCESSED)
#define PAGE_TABLE	(PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_ACCESSED)

#define GFP_BUFFER	0x00
#define GFP_ATOMIC	0x01
#define GFP_USER	0x02
#define GFP_KERNEL	0x03


/* vm_ops not present page codes */
#define SHM_SWP_TYPE 0x41        
extern void shm_no_page (ulong *);

#endif
