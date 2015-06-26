/*
 *  linux/fs/ext2/dcache.c
 *
 *  Copyright (C) 1992, 1993, 1994  Remy Card (card@masi.ibp.fr)
 *                                  Laboratoire MASI - Institut Blaise Pascal
 *                                  Universite Pierre et Marie Curie (Paris VI)
 *
 */

/*
 * dcache.c contains the code that handles the directory cache used by
 * lookup() and readdir()
 */

#include <asm/segment.h>

#include <linux/fs.h>
#include <linux/ext2_fs.h>
#include <linux/string.h>

#ifndef DONT_USE_DCACHE

#define DCACHE_NAME_LEN	32

struct dir_cache_entry {
	unsigned short dev;
	unsigned long dir;
	unsigned long ino;
	char name[DCACHE_NAME_LEN + 1];
	int len;
	struct dir_cache_entry * queue_prev;
	struct dir_cache_entry * queue_next;
	struct dir_cache_entry * prev;
	struct dir_cache_entry * next;
};

static struct dir_cache_entry * first = NULL;
static struct dir_cache_entry * last = NULL;
static struct dir_cache_entry * first_free = NULL;
static int cache_initialized = 0;
#ifdef EXT2FS_DEBUG_CACHE
static int hits = 0;
static int misses = 0;
#endif

#define CACHE_SIZE 128

static struct dir_cache_entry dcache[CACHE_SIZE];

#define HASH_QUEUES 16

static struct dir_cache_entry * queue_head[HASH_QUEUES];
static struct dir_cache_entry * queue_tail[HASH_QUEUES];

#define hash(dev,dir)	((dev ^ dir) % HASH_QUEUES)

/*
 * Initialize the cache
 */
static void init_cache (void)
{
	int i;

	dcache[0].prev = NULL;
	dcache[0].next = &dcache[1];
	dcache[0].queue_next = dcache[0].queue_prev = NULL;
	for (i = 1; i < CACHE_SIZE - 1; i++) {
		dcache[i].prev = &dcache[i - 1];
		dcache[i].next = &dcache[i + 1];
		dcache[i].queue_next = dcache[i].queue_prev = NULL;
	}
	dcache[i].prev = &dcache[i - 1];
	dcache[i].next = NULL;
	dcache[i].queue_next = dcache[i].queue_prev = NULL;
	first_free = &dcache[0];
	for (i = 0; i < HASH_QUEUES; i++)
		queue_tail[i] = queue_head[i] = NULL;
	cache_initialized = 1;
}

/*
 * Find a name in the cache
 */
static struct dir_cache_entry * find_name (int queue, unsigned short dev,
					   unsigned long dir,
					   const char * name, int len)
{
	struct dir_cache_entry * p;

	for (p = queue_head[queue]; p != NULL && (p->dev != dev ||
	     p->dir != dir || p->len != len ||
	     strncmp (name, p->name, p->len) != 0);
	     p = p->queue_next)
		;
	return p;
}

#ifdef EXT2FS_DEBUG_CACHE
/*
 * List the cache entries for debugging
 */
static void show_cache (const char * func_name)
{
	struct dir_cache_entry * p;

	printk ("%s: cache status\n", func_name);
	for (p = first; p != NULL; p = p->next)
		printk ("dev:%04x, dir=%4lu, name=%s\n",
			p->dev, p->dir, p->name);
}
#endif

/*
 * Add an entry at the beginning of the cache
 */
static void add_to_cache (struct dir_cache_entry * p)
{
	p->prev = NULL;
	p->next = first;
	if (first)
		first->prev = p;
	if (!last)
		last = p;
	first = p;
}

/*
 * Add an entry at the beginning of a queue
 */
static void add_to_queue (int queue, struct dir_cache_entry * p)
{
	p->queue_prev = NULL;
	p->queue_next = queue_head[queue];
	if (queue_head[queue])
		queue_head[queue]->queue_prev = p;
	if (!queue_tail[queue])
		queue_tail[queue] = p;
	queue_head[queue] = p;
}

/*
 * Remove an entry from the cache
 */
static void remove_from_cache (struct dir_cache_entry * p)
{
	if (p->prev)
		p->prev->next = p->next;
	else
		first = p->next;
	if (p->next)
		p->next->prev = p->prev;
	else
		last = p->prev;
	p->prev = NULL;
	p->next = NULL;
}

/*
 * Remove an entry from a queue
 */
static void remove_from_queue (int queue, struct dir_cache_entry * p)
{
	if (p->queue_prev)
		p->queue_prev->queue_next = p->queue_next;
	else
		queue_head[queue] = p->queue_next;
	if (p->queue_next)
		p->queue_next->queue_prev = p->queue_prev;
	else
		queue_tail[queue] = p->queue_prev;
	p->queue_prev = NULL;
	p->queue_next = NULL;
}

/*
 * Invalidate all cache entries on a device (called by put_super() when
 * a file system is unmounted)
 */
void ext2_dcache_invalidate (unsigned short dev)
{
	struct dir_cache_entry * p;
	struct dir_cache_entry * p2;

	if (!cache_initialized)
		init_cache ();
	for (p = first; p != NULL; p = p2) {
		p2 = p->next;
		if (p->dev == dev) {
			remove_from_cache (p);
			remove_from_queue (hash (p->dev, p->dir), p);
			p->next = first_free;
			first_free = p;
		}
	}
#ifdef EXT2FS_DEBUG_CACHE
	show_cache ("dcache_invalidate");
#endif
}

/*
 * Lookup a directory entry in the cache
 */
unsigned long ext2_dcache_lookup (unsigned short dev, unsigned long dir,
				  const char * name, int len)
{
	char our_name[EXT2_NAME_LEN];
	int queue;
	struct dir_cache_entry * p;

	if (!cache_initialized)
		init_cache ();
	if (len > DCACHE_NAME_LEN)
		return 0;
	memcpy (our_name, (char *) name, len);
	our_name[len] = '\0';
#ifdef EXT2FS_DEBUG_CACHE
	printk ("dcache_lookup (%04x, %lu, %s, %d)\n", dev, dir, our_name, len);
#endif
	queue = hash (dev, dir);
	if ((p = find_name (queue, dev, dir, our_name, len))) {
		if (p != first) {
			remove_from_cache (p);
			add_to_cache (p);
		}
		if (p != queue_head[queue]) {
			remove_from_queue (queue, p);
			add_to_queue (queue, p);
		}
#ifdef EXT2FS_DEBUG_CACHE
		hits++;
		printk ("dcache_lookup: %s,hit,inode=%lu,hits=%d,misses=%d\n",
			our_name, p->ino, hits, misses);
		show_cache ("dcache_lookup");
#endif
		return p->ino;
	} else {
#ifdef EXT2FS_DEBUG_CACHE
		misses++;
		printk ("dcache_lookup: %s,miss,hits=%d,misses=%d\n",
			our_name, hits, misses);
		show_cache ("dcache_lookup");
#endif
		return 0;
	}
}

/*
 * Add a directory entry to the cache
 *
 * This function is called by ext2_lookup(), ext2_readdir()
 * and the functions which create directory entries
 */
void ext2_dcache_add (unsigned short dev, unsigned long dir, const char * name,
		      int len, unsigned long ino)
{
	struct dir_cache_entry * p;
	int queue;

	if (!cache_initialized)
		init_cache ();
#ifdef EXT2FS_DEBUG_CACHE
	printk ("dcache_add (%04x, %lu, %s, %d, %lu)\n",
		dev, dir, name, len, ino);
#endif
	if (len > DCACHE_NAME_LEN)
		return;
	queue = hash (dev, dir);
	if ((p = find_name (queue, dev, dir, name, len))) {
		p->dir = dir;
		p->ino = ino;
		if (p != first) {
			remove_from_cache (p);
			add_to_cache (p);
		}
		if (p != queue_head[queue]) {
			remove_from_queue (queue, p);
			add_to_queue (queue, p);
		}
	} else {
		if (first_free) {
			p = first_free;
			first_free = p->next;
		} else {
			if (!last)
				panic ("dcache_add: last == NULL\n");
			else {
				p = last;
				last = p->prev;
				if (last)
					last->next = NULL;
				remove_from_queue (hash (p->dev, p->dir), p);
			}
		}
		p->dev = dev;
		p->dir = dir;
		p->ino = ino;
		strncpy (p->name, name, len);
		p->len = len;
		p->name[len] = '\0';
		add_to_cache (p);
		add_to_queue (queue, p);
	}
#ifdef EXT2FS_DEBUG_CACHE
	show_cache ("dcache_add");
#endif
}

/*
 * Remove a directory from the cache
 *
 * This function is called by the functions which remove directory entries
 */
void ext2_dcache_remove (unsigned short dev, unsigned long dir,
			 const char * name, int len)
{
	struct dir_cache_entry * p;
	int queue;

	if (!cache_initialized)
		init_cache ();
#ifdef EXT2FS_DEBUG_CACHE
	printk ("dcache_remove (%04x, %lu, %s, %d)\n", dev, dir, name, len);
#endif
	if (len > DCACHE_NAME_LEN)
		return;
	queue = hash (dev, dir);
	if ((p = find_name (queue, dev, dir, name, len))) {
		remove_from_cache (p);
		remove_from_queue (queue, p);
		p->next = first_free;
		first_free = p;
	}
#ifdef EXT2FS_DEBUG_CACHE
	show_cache ("dcache_remove");
#endif
}

#endif
