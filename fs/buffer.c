/*
 *  linux/fs/buffer.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 *  'buffer.c' implements the buffer-cache functions. Race-conditions have
 * been avoided by NEVER letting an interrupt change a buffer (except for the
 * data, of course), but instead letting the caller do it.
 */

/*
 * NOTE! There is one discordant note here: checking floppies for
 * disk change. This is where it fits best, I think, as it should
 * invalidate changed floppy-disk-caches.
 */

#include <stdarg.h>
 
#include <linux/config.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/locks.h>
#include <linux/errno.h>

#include <asm/system.h>
#include <asm/io.h>

#ifdef CONFIG_SCSI
#ifdef CONFIG_BLK_DEV_SR
extern int check_cdrom_media_change(int, int);
#endif
#ifdef CONFIG_BLK_DEV_SD
extern int check_scsidisk_media_change(int, int);
extern int revalidate_scsidisk(int, int);
#endif
#endif
#ifdef CONFIG_CDU31A
extern int check_cdu31a_media_change(int, int);
#endif
#ifdef CONFIG_MCD
extern int check_mcd_media_change(int, int);
#endif

static int grow_buffers(int pri, int size);

static struct buffer_head * hash_table[NR_HASH];
static struct buffer_head * free_list = NULL;
static struct buffer_head * unused_list = NULL;
static struct wait_queue * buffer_wait = NULL;

int nr_buffers = 0;
int buffermem = 0;
int nr_buffer_heads = 0;
static int min_free_pages = 20;	/* nr free pages needed before buffer grows */
extern int *blksize_size[];

/*
 * Rewrote the wait-routines to use the "new" wait-queue functionality,
 * and getting rid of the cli-sti pairs. The wait-queue routines still
 * need cli-sti, but now it's just a couple of 386 instructions or so.
 *
 * Note that the real wait_on_buffer() is an inline function that checks
 * if 'b_wait' is set before calling this, so that the queues aren't set
 * up unnecessarily.
 */
void __wait_on_buffer(struct buffer_head * bh)
{
	struct wait_queue wait = { current, NULL };

	bh->b_count++;
	add_wait_queue(&bh->b_wait, &wait);
repeat:
	current->state = TASK_UNINTERRUPTIBLE;
	if (bh->b_lock) {
		schedule();
		goto repeat;
	}
	remove_wait_queue(&bh->b_wait, &wait);
	bh->b_count--;
	current->state = TASK_RUNNING;
}

/* Call sync_buffers with wait!=0 to ensure that the call does not
   return until all buffer writes have completed.  Sync() may return
   before the writes have finished; fsync() may not. */

static int sync_buffers(dev_t dev, int wait)
{
	int i, retry, pass = 0, err = 0;
	struct buffer_head * bh;

	/* One pass for no-wait, three for wait:
	   0) write out all dirty, unlocked buffers;
	   1) write out all dirty buffers, waiting if locked;
	   2) wait for completion by waiting for all buffers to unlock.
	 */
repeat:
	retry = 0;
	bh = free_list;
	for (i = nr_buffers*2 ; i-- > 0 ; bh = bh->b_next_free) {
		if (dev && bh->b_dev != dev)
			continue;
#ifdef 0 /* Disable bad-block debugging code */
		if (bh->b_req && !bh->b_lock &&
		    !bh->b_dirt && !bh->b_uptodate)
			printk ("Warning (IO error) - orphaned block %08x on %04x\n",
				bh->b_blocknr, bh->b_dev);
#endif
		if (bh->b_lock)
		{
			/* Buffer is locked; skip it unless wait is
			   requested AND pass > 0. */
			if (!wait || !pass) {
				retry = 1;
				continue;
			}
			wait_on_buffer (bh);
		}
		/* If an unlocked buffer is not uptodate, there has been 
		   an IO error. Skip it. */
		if (wait && bh->b_req && !bh->b_lock &&
		    !bh->b_dirt && !bh->b_uptodate)
		{
			err = 1;
			continue;
		}
		/* Don't write clean buffers.  Don't write ANY buffers
		   on the third pass. */
		if (!bh->b_dirt || pass>=2)
			continue;
		bh->b_count++;
		ll_rw_block(WRITE, 1, &bh);
		bh->b_count--;
		retry = 1;
	}
	/* If we are waiting for the sync to succeed, and if any dirty
	   blocks were written, then repeat; on the second pass, only
	   wait for buffers being written (do not pass to write any
	   more buffers on the second pass). */
	if (wait && retry && ++pass<=2)
		goto repeat;
	return err;
}

void sync_dev(dev_t dev)
{
	sync_buffers(dev, 0);
	sync_supers(dev);
	sync_inodes(dev);
	sync_buffers(dev, 0);
}

int fsync_dev(dev_t dev)
{
	sync_buffers(dev, 0);
	sync_supers(dev);
	sync_inodes(dev);
	return sync_buffers(dev, 1);
}

asmlinkage int sys_sync(void)
{
	sync_dev(0);
	return 0;
}

int file_fsync (struct inode *inode, struct file *filp)
{
	return fsync_dev(inode->i_dev);
}

asmlinkage int sys_fsync(unsigned int fd)
{
	struct file * file;
	struct inode * inode;

	if (fd>=NR_OPEN || !(file=current->filp[fd]) || !(inode=file->f_inode))
		return -EBADF;
	if (!file->f_op || !file->f_op->fsync)
		return -EINVAL;
	if (file->f_op->fsync(inode,file))
		return -EIO;
	return 0;
}

void invalidate_buffers(dev_t dev)
{
	int i;
	struct buffer_head * bh;

	bh = free_list;
	for (i = nr_buffers*2 ; --i > 0 ; bh = bh->b_next_free) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev)
			bh->b_uptodate = bh->b_dirt = bh->b_req = 0;
	}
}

/*
 * This routine checks whether a floppy has been changed, and
 * invalidates all buffer-cache-entries in that case. This
 * is a relatively slow routine, so we have to try to minimize using
 * it. Thus it is called only upon a 'mount' or 'open'. This
 * is the best way of combining speed and utility, I think.
 * People changing diskettes in the middle of an operation deserve
 * to loose :-)
 *
 * NOTE! Although currently this is only for floppies, the idea is
 * that any additional removable block-device will use this routine,
 * and that mount/open needn't know that floppies/whatever are
 * special.
 */
void check_disk_change(dev_t dev)
{
	int i;
	struct buffer_head * bh;

	switch(MAJOR(dev)){
	case FLOPPY_MAJOR:
		if (!(bh = getblk(dev,0,1024)))
			return;
		i = floppy_change(bh);
		brelse(bh);
		break;

#if defined(CONFIG_BLK_DEV_SD) && defined(CONFIG_SCSI)
         case SCSI_DISK_MAJOR:
		i = check_scsidisk_media_change(dev, 0);
		break;
#endif

#if defined(CONFIG_BLK_DEV_SR) && defined(CONFIG_SCSI)
	 case SCSI_CDROM_MAJOR:
		i = check_cdrom_media_change(dev, 0);
		break;
#endif

#if defined(CONFIG_CDU31A)
         case CDU31A_CDROM_MAJOR:
		i = check_cdu31a_media_change(dev, 0);
		break;
#endif

#if defined(CONFIG_MCD)
         case MITSUMI_CDROM_MAJOR:
		i = check_mcd_media_change(dev, 0);
		break;
#endif

         default:
		return;
	};

	if (!i)	return;

	printk("VFS: Disk change detected on device %d/%d\n",
					MAJOR(dev), MINOR(dev));
	for (i=0 ; i<NR_SUPER ; i++)
		if (super_blocks[i].s_dev == dev)
			put_super(super_blocks[i].s_dev);
	invalidate_inodes(dev);
	invalidate_buffers(dev);

#if defined(CONFIG_BLK_DEV_SD) && defined(CONFIG_SCSI)
/* This is trickier for a removable hardisk, because we have to invalidate
   all of the partitions that lie on the disk. */
	if (MAJOR(dev) == SCSI_DISK_MAJOR)
		revalidate_scsidisk(dev, 0);
#endif
}

#define _hashfn(dev,block) (((unsigned)(dev^block))%NR_HASH)
#define hash(dev,block) hash_table[_hashfn(dev,block)]

static inline void remove_from_hash_queue(struct buffer_head * bh)
{
	if (bh->b_next)
		bh->b_next->b_prev = bh->b_prev;
	if (bh->b_prev)
		bh->b_prev->b_next = bh->b_next;
	if (hash(bh->b_dev,bh->b_blocknr) == bh)
		hash(bh->b_dev,bh->b_blocknr) = bh->b_next;
	bh->b_next = bh->b_prev = NULL;
}

static inline void remove_from_free_list(struct buffer_head * bh)
{
	if (!(bh->b_prev_free) || !(bh->b_next_free))
		panic("VFS: Free block list corrupted");
	bh->b_prev_free->b_next_free = bh->b_next_free;
	bh->b_next_free->b_prev_free = bh->b_prev_free;
	if (free_list == bh)
		free_list = bh->b_next_free;
	bh->b_next_free = bh->b_prev_free = NULL;
}

static inline void remove_from_queues(struct buffer_head * bh)
{
	remove_from_hash_queue(bh);
	remove_from_free_list(bh);
}

static inline void put_first_free(struct buffer_head * bh)
{
	if (!bh || (bh == free_list))
		return;
	remove_from_free_list(bh);
/* add to front of free list */
	bh->b_next_free = free_list;
	bh->b_prev_free = free_list->b_prev_free;
	free_list->b_prev_free->b_next_free = bh;
	free_list->b_prev_free = bh;
	free_list = bh;
}

static inline void put_last_free(struct buffer_head * bh)
{
	if (!bh)
		return;
	if (bh == free_list) {
		free_list = bh->b_next_free;
		return;
	}
	remove_from_free_list(bh);
/* add to back of free list */
	bh->b_next_free = free_list;
	bh->b_prev_free = free_list->b_prev_free;
	free_list->b_prev_free->b_next_free = bh;
	free_list->b_prev_free = bh;
}

static inline void insert_into_queues(struct buffer_head * bh)
{
/* put at end of free list */
	bh->b_next_free = free_list;
	bh->b_prev_free = free_list->b_prev_free;
	free_list->b_prev_free->b_next_free = bh;
	free_list->b_prev_free = bh;
/* put the buffer in new hash-queue if it has a device */
	bh->b_prev = NULL;
	bh->b_next = NULL;
	if (!bh->b_dev)
		return;
	bh->b_next = hash(bh->b_dev,bh->b_blocknr);
	hash(bh->b_dev,bh->b_blocknr) = bh;
	if (bh->b_next)
		bh->b_next->b_prev = bh;
}

static struct buffer_head * find_buffer(dev_t dev, int block, int size)
{		
	struct buffer_head * tmp;

	for (tmp = hash(dev,block) ; tmp != NULL ; tmp = tmp->b_next)
		if (tmp->b_dev==dev && tmp->b_blocknr==block)
			if (tmp->b_size == size)
				return tmp;
			else {
				printk("VFS: Wrong blocksize on device %d/%d\n",
							MAJOR(dev), MINOR(dev));
				return NULL;
			}
	return NULL;
}

/*
 * Why like this, I hear you say... The reason is race-conditions.
 * As we don't lock buffers (unless we are readint them, that is),
 * something might happen to it while we sleep (ie a read-error
 * will force it bad). This shouldn't really happen currently, but
 * the code is ready.
 */
struct buffer_head * get_hash_table(dev_t dev, int block, int size)
{
	struct buffer_head * bh;

	for (;;) {
		if (!(bh=find_buffer(dev,block,size)))
			return NULL;
		bh->b_count++;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_blocknr == block && bh->b_size == size)
			return bh;
		bh->b_count--;
	}
}

void set_blocksize(dev_t dev, int size)
{
	int i;
	struct buffer_head * bh, *bhnext;

	if (!blksize_size[MAJOR(dev)])
		return;

	switch(size) {
		default: panic("Invalid blocksize passed to set_blocksize");
		case 512: case 1024: case 2048: case 4096:;
	}

	if (blksize_size[MAJOR(dev)][MINOR(dev)] == 0 && size == BLOCK_SIZE) {
		blksize_size[MAJOR(dev)][MINOR(dev)] = size;
		return;
	}
	if (blksize_size[MAJOR(dev)][MINOR(dev)] == size)
		return;
	sync_buffers(dev, 2);
	blksize_size[MAJOR(dev)][MINOR(dev)] = size;

  /* We need to be quite careful how we do this - we are moving entries
     around on the free list, and we can get in a loop if we are not careful.*/

	bh = free_list;
	for (i = nr_buffers*2 ; --i > 0 ; bh = bhnext) {
		bhnext = bh->b_next_free; 
		if (bh->b_dev != dev)
			continue;
		if (bh->b_size == size)
			continue;

		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_size != size)
			bh->b_uptodate = bh->b_dirt = 0;
		remove_from_hash_queue(bh);
/*    put_first_free(bh); */
	}
}

/*
 * Ok, this is getblk, and it isn't very clear, again to hinder
 * race-conditions. Most of the code is seldom used, (ie repeating),
 * so it should be much more efficient than it looks.
 *
 * The algoritm is changed: hopefully better, and an elusive bug removed.
 *
 * 14.02.92: changed it to sync dirty buffers a bit: better performance
 * when the filesystem starts to get full of dirty blocks (I hope).
 */
#define BADNESS(bh) (((bh)->b_dirt<<1)+(bh)->b_lock)
struct buffer_head * getblk(dev_t dev, int block, int size)
{
	struct buffer_head * bh, * tmp;
	int buffers;
	static int grow_size = 0;

repeat:
	bh = get_hash_table(dev, block, size);
	if (bh) {
		if (bh->b_uptodate && !bh->b_dirt)
			put_last_free(bh);
		return bh;
	}
	grow_size -= size;
	if (nr_free_pages > min_free_pages && grow_size <= 0) {
		if (grow_buffers(GFP_BUFFER, size))
			grow_size = PAGE_SIZE;
	}
	buffers = nr_buffers;
	bh = NULL;

	for (tmp = free_list; buffers-- > 0 ; tmp = tmp->b_next_free) {
		if (tmp->b_count || tmp->b_size != size)
			continue;
		if (mem_map[MAP_NR((unsigned long) tmp->b_data)] != 1)
			continue;
		if (!bh || BADNESS(tmp)<BADNESS(bh)) {
			bh = tmp;
			if (!BADNESS(tmp))
				break;
		}
#if 0
		if (tmp->b_dirt) {
			tmp->b_count++;
			ll_rw_block(WRITEA, 1, &tmp);
			tmp->b_count--;
		}
#endif
	}

	if (!bh) {
		if (nr_free_pages > 5)
			if (grow_buffers(GFP_BUFFER, size))
				goto repeat;
		if (!grow_buffers(GFP_ATOMIC, size))
			sleep_on(&buffer_wait);
		goto repeat;
	}

	wait_on_buffer(bh);
	if (bh->b_count || bh->b_size != size)
		goto repeat;
	if (bh->b_dirt) {
		sync_buffers(0,0);
		goto repeat;
	}
/* NOTE!! While we slept waiting for this block, somebody else might */
/* already have added "this" block to the cache. check it */
	if (find_buffer(dev,block,size))
		goto repeat;
/* OK, FINALLY we know that this buffer is the only one of its kind, */
/* and that it's unused (b_count=0), unlocked (b_lock=0), and clean */
	bh->b_count=1;
	bh->b_dirt=0;
	bh->b_uptodate=0;
	bh->b_req=0;
	remove_from_queues(bh);
	bh->b_dev=dev;
	bh->b_blocknr=block;
	insert_into_queues(bh);
	return bh;
}

void brelse(struct buffer_head * buf)
{
	if (!buf)
		return;
	wait_on_buffer(buf);
	if (buf->b_count) {
		if (--buf->b_count)
			return;
		wake_up(&buffer_wait);
		return;
	}
	printk("VFS: brelse: Trying to free free buffer\n");
}

/*
 * bread() reads a specified block and returns the buffer that contains
 * it. It returns NULL if the block was unreadable.
 */
struct buffer_head * bread(dev_t dev, int block, int size)
{
	struct buffer_head * bh;

	if (!(bh = getblk(dev, block, size))) {
		printk("VFS: bread: READ error on device %d/%d\n",
						MAJOR(dev), MINOR(dev));
		return NULL;
	}
	if (bh->b_uptodate)
		return bh;
	ll_rw_block(READ, 1, &bh);
	wait_on_buffer(bh);
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return NULL;
}

/*
 * Ok, breada can be used as bread, but additionally to mark other
 * blocks for reading as well. End the argument list with a negative
 * number.
 */
struct buffer_head * breada(dev_t dev,int first, ...)
{
	va_list args;
	unsigned int blocksize;
	struct buffer_head * bh, *tmp;

	va_start(args,first);

	blocksize = BLOCK_SIZE;
	if (blksize_size[MAJOR(dev)] && blksize_size[MAJOR(dev)][MINOR(dev)])
		blocksize = blksize_size[MAJOR(dev)][MINOR(dev)];

	if (!(bh = getblk(dev, first, blocksize))) {
		printk("VFS: breada: READ error on device %d/%d\n",
						MAJOR(dev), MINOR(dev));
		return NULL;
	}
	if (!bh->b_uptodate)
		ll_rw_block(READ, 1, &bh);
	while ((first=va_arg(args,int))>=0) {
		tmp = getblk(dev, first, blocksize);
		if (tmp) {
			if (!tmp->b_uptodate)
				ll_rw_block(READA, 1, &tmp);
			tmp->b_count--;
		}
	}
	va_end(args);
	wait_on_buffer(bh);
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return (NULL);
}

/*
 * See fs/inode.c for the weird use of volatile..
 */
static void put_unused_buffer_head(struct buffer_head * bh)
{
	struct wait_queue * wait;

	wait = ((volatile struct buffer_head *) bh)->b_wait;
	memset((void *) bh,0,sizeof(*bh));
	((volatile struct buffer_head *) bh)->b_wait = wait;
	bh->b_next_free = unused_list;
	unused_list = bh;
}

static void get_more_buffer_heads(void)
{
	int i;
	struct buffer_head * bh;

	if (unused_list)
		return;

	if(! (bh = (struct buffer_head*) get_free_page(GFP_BUFFER)))
		return;

	for (nr_buffer_heads+=i=PAGE_SIZE/sizeof*bh ; i>0; i--) {
		bh->b_next_free = unused_list;	/* only make link */
		unused_list = bh++;
	}
}

static struct buffer_head * get_unused_buffer_head(void)
{
	struct buffer_head * bh;

	get_more_buffer_heads();
	if (!unused_list)
		return NULL;
	bh = unused_list;
	unused_list = bh->b_next_free;
	bh->b_next_free = NULL;
	bh->b_data = NULL;
	bh->b_size = 0;
	bh->b_req = 0;
	return bh;
}

/*
 * Create the appropriate buffers when given a page for data area and
 * the size of each buffer.. Use the bh->b_this_page linked list to
 * follow the buffers created.  Return NULL if unable to create more
 * buffers.
 */
static struct buffer_head * create_buffers(unsigned long page, unsigned long size)
{
	struct buffer_head *bh, *head;
	unsigned long offset;

	head = NULL;
	offset = PAGE_SIZE;
	while ((offset -= size) < PAGE_SIZE) {
		bh = get_unused_buffer_head();
		if (!bh)
			goto no_grow;
		bh->b_this_page = head;
		head = bh;
		bh->b_data = (char *) (page+offset);
		bh->b_size = size;
	}
	return head;
/*
 * In case anything failed, we just free everything we got.
 */
no_grow:
	bh = head;
	while (bh) {
		head = bh;
		bh = bh->b_this_page;
		put_unused_buffer_head(head);
	}
	return NULL;
}

static void read_buffers(struct buffer_head * bh[], int nrbuf)
{
	int i;
	int bhnum = 0;
	struct buffer_head * bhr[8];

	for (i = 0 ; i < nrbuf ; i++) {
		if (bh[i] && !bh[i]->b_uptodate)
			bhr[bhnum++] = bh[i];
	}
	if (bhnum)
		ll_rw_block(READ, bhnum, bhr);
	for (i = 0 ; i < nrbuf ; i++) {
		if (bh[i]) {
			wait_on_buffer(bh[i]);
		}
	}
}

static unsigned long check_aligned(struct buffer_head * first, unsigned long address,
	dev_t dev, int *b, int size)
{
	struct buffer_head * bh[8];
	unsigned long page;
	unsigned long offset;
	int block;
	int nrbuf;

	page = (unsigned long) first->b_data;
	if (page & ~PAGE_MASK) {
		brelse(first);
		return 0;
	}
	mem_map[MAP_NR(page)]++;
	bh[0] = first;
	nrbuf = 1;
	for (offset = size ; offset < PAGE_SIZE ; offset += size) {
		block = *++b;
		if (!block)
			goto no_go;
		first = get_hash_table(dev, block, size);
		if (!first)
			goto no_go;
		bh[nrbuf++] = first;
		if (page+offset != (unsigned long) first->b_data)
			goto no_go;
	}
	read_buffers(bh,nrbuf);		/* make sure they are actually read correctly */
	while (nrbuf-- > 0)
		brelse(bh[nrbuf]);
	free_page(address);
	++current->min_flt;
	return page;
no_go:
	while (nrbuf-- > 0)
		brelse(bh[nrbuf]);
	free_page(page);
	return 0;
}

static unsigned long try_to_load_aligned(unsigned long address,
	dev_t dev, int b[], int size)
{
	struct buffer_head * bh, * tmp, * arr[8];
	unsigned long offset;
	int * p;
	int block;

	bh = create_buffers(address, size);
	if (!bh)
		return 0;
	/* do any of the buffers already exist? punt if so.. */
	p = b;
	for (offset = 0 ; offset < PAGE_SIZE ; offset += size) {
		block = *(p++);
		if (!block)
			goto not_aligned;
		if (find_buffer(dev, block, size))
			goto not_aligned;
	}
	tmp = bh;
	p = b;
	block = 0;
	while (1) {
		arr[block++] = bh;
		bh->b_count = 1;
		bh->b_dirt = 0;
		bh->b_uptodate = 0;
		bh->b_dev = dev;
		bh->b_blocknr = *(p++);
		nr_buffers++;
		insert_into_queues(bh);
		if (bh->b_this_page)
			bh = bh->b_this_page;
		else
			break;
	}
	buffermem += PAGE_SIZE;
	bh->b_this_page = tmp;
	mem_map[MAP_NR(address)]++;
	read_buffers(arr,block);
	while (block-- > 0)
		brelse(arr[block]);
	++current->maj_flt;
	return address;
not_aligned:
	while ((tmp = bh) != NULL) {
		bh = bh->b_this_page;
		put_unused_buffer_head(tmp);
	}
	return 0;
}

/*
 * Try-to-share-buffers tries to minimize memory use by trying to keep
 * both code pages and the buffer area in the same page. This is done by
 * (a) checking if the buffers are already aligned correctly in memory and
 * (b) if none of the buffer heads are in memory at all, trying to load
 * them into memory the way we want them.
 *
 * This doesn't guarantee that the memory is shared, but should under most
 * circumstances work very well indeed (ie >90% sharing of code pages on
 * demand-loadable executables).
 */
static inline unsigned long try_to_share_buffers(unsigned long address,
	dev_t dev, int *b, int size)
{
	struct buffer_head * bh;
	int block;

	block = b[0];
	if (!block)
		return 0;
	bh = get_hash_table(dev, block, size);
	if (bh)
		return check_aligned(bh, address, dev, b, size);
	return try_to_load_aligned(address, dev, b, size);
}

#define COPYBLK(size,from,to) \
__asm__ __volatile__("rep ; movsl": \
	:"c" (((unsigned long) size) >> 2),"S" (from),"D" (to) \
	:"cx","di","si")

/*
 * bread_page reads four buffers into memory at the desired address. It's
 * a function of its own, as there is some speed to be got by reading them
 * all at the same time, not waiting for one to be read, and then another
 * etc. This also allows us to optimize memory usage by sharing code pages
 * and filesystem buffers..
 */
unsigned long bread_page(unsigned long address, dev_t dev, int b[], int size, int prot)
{
	struct buffer_head * bh[8];
	unsigned long where;
	int i, j;

	if (!(prot & PAGE_RW)) {
		where = try_to_share_buffers(address,dev,b,size);
		if (where)
			return where;
	}
	++current->maj_flt;
 	for (i=0, j=0; j<PAGE_SIZE ; i++, j+= size) {
		bh[i] = NULL;
		if (b[i])
			bh[i] = getblk(dev, b[i], size);
	}
	read_buffers(bh,i);
	where = address;
 	for (i=0, j=0; j<PAGE_SIZE ; i++, j += size,address += size) {
		if (bh[i]) {
			if (bh[i]->b_uptodate)
				COPYBLK(size, (unsigned long) bh[i]->b_data,address);
			brelse(bh[i]);
		}
	}
	return where;
}

/*
 * Try to increase the number of buffers available: the size argument
 * is used to determine what kind of buffers we want.
 */
static int grow_buffers(int pri, int size)
{
	unsigned long page;
	struct buffer_head *bh, *tmp;

	if ((size & 511) || (size > PAGE_SIZE)) {
		printk("VFS: grow_buffers: size = %d\n",size);
		return 0;
	}
	if(!(page = __get_free_page(pri)))
		return 0;
	bh = create_buffers(page, size);
	if (!bh) {
		free_page(page);
		return 0;
	}
	tmp = bh;
	while (1) {
		if (free_list) {
			tmp->b_next_free = free_list;
			tmp->b_prev_free = free_list->b_prev_free;
			free_list->b_prev_free->b_next_free = tmp;
			free_list->b_prev_free = tmp;
		} else {
			tmp->b_prev_free = tmp;
			tmp->b_next_free = tmp;
		}
		free_list = tmp;
		++nr_buffers;
		if (tmp->b_this_page)
			tmp = tmp->b_this_page;
		else
			break;
	}
	tmp->b_this_page = bh;
	buffermem += PAGE_SIZE;
	return 1;
}

/*
 * try_to_free() checks if all the buffers on this particular page
 * are unused, and free's the page if so.
 */
static int try_to_free(struct buffer_head * bh, struct buffer_head ** bhp)
{
	unsigned long page;
	struct buffer_head * tmp, * p;

	*bhp = bh;
	page = (unsigned long) bh->b_data;
	page &= PAGE_MASK;
	tmp = bh;
	do {
		if (!tmp)
			return 0;
		if (tmp->b_count || tmp->b_dirt || tmp->b_lock || tmp->b_wait)
			return 0;
		tmp = tmp->b_this_page;
	} while (tmp != bh);
	tmp = bh;
	do {
		p = tmp;
		tmp = tmp->b_this_page;
		nr_buffers--;
		if (p == *bhp)
			*bhp = p->b_prev_free;
		remove_from_queues(p);
		put_unused_buffer_head(p);
	} while (tmp != bh);
	buffermem -= PAGE_SIZE;
	free_page(page);
	return !mem_map[MAP_NR(page)];
}

/*
 * Try to free up some pages by shrinking the buffer-cache
 *
 * Priority tells the routine how hard to try to shrink the
 * buffers: 3 means "don't bother too much", while a value
 * of 0 means "we'd better get some free pages now".
 */
int shrink_buffers(unsigned int priority)
{
	struct buffer_head *bh;
	int i;

	if (priority < 2)
		sync_buffers(0,0);
	bh = free_list;
	i = nr_buffers >> priority;
	for ( ; i-- > 0 ; bh = bh->b_next_free) {
		if (bh->b_count ||
		    (priority >= 5 &&
		     mem_map[MAP_NR((unsigned long) bh->b_data)] > 1)) {
			put_last_free(bh);
			continue;
		}
		if (!bh->b_this_page)
			continue;
		if (bh->b_lock)
			if (priority)
				continue;
			else
				wait_on_buffer(bh);
		if (bh->b_dirt) {
			bh->b_count++;
			ll_rw_block(WRITEA, 1, &bh);
			bh->b_count--;
			continue;
		}
		if (try_to_free(bh, &bh))
			return 1;
	}
	return 0;
}

void show_buffers(void)
{
	struct buffer_head * bh;
	int found = 0, locked = 0, dirty = 0, used = 0, lastused = 0;

	printk("Buffer memory:   %6dkB\n",buffermem>>10);
	printk("Buffer heads:    %6d\n",nr_buffer_heads);
	printk("Buffer blocks:   %6d\n",nr_buffers);
	bh = free_list;
	do {
		found++;
		if (bh->b_lock)
			locked++;
		if (bh->b_dirt)
			dirty++;
		if (bh->b_count)
			used++, lastused = found;
		bh = bh->b_next_free;
	} while (bh != free_list);
	printk("Buffer mem: %d buffers, %d used (last=%d), %d locked, %d dirty\n",
		found, used, lastused, locked, dirty);
}

/*
 * This initializes the initial buffer free list.  nr_buffers is set
 * to one less the actual number of buffers, as a sop to backwards
 * compatibility --- the old code did this (I think unintentionally,
 * but I'm not sure), and programs in the ps package expect it.
 * 					- TYT 8/30/92
 */
void buffer_init(void)
{
	int i;

	if (high_memory >= 4*1024*1024)
		min_free_pages = 200;
	else
		min_free_pages = 20;
	for (i = 0 ; i < NR_HASH ; i++)
		hash_table[i] = NULL;
	free_list = 0;
	grow_buffers(GFP_KERNEL, BLOCK_SIZE);
	if (!free_list)
		panic("VFS: Unable to initialize buffer free list!");
	return;
}
