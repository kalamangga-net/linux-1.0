/*
 * linux/ipc/shm.c
 * Copyright (C) 1992, 1993 Krishna Balasubramanian 
 *         Many improvements/fixes by Bruno Haible.
 * assume user segments start at 0x0
 */

#include <linux/errno.h>
#include <asm/segment.h>
#include <linux/sched.h>
#include <linux/ipc.h> 
#include <linux/shm.h>
#include <linux/stat.h>
#include <linux/malloc.h>

extern int ipcperms (struct ipc_perm *ipcp, short semflg);
extern unsigned int get_swap_page(void);
static int findkey (key_t key);
static int newseg (key_t key, int shmflg, int size);
static int shm_map (struct shm_desc *shmd, int remap);
static void killseg (int id);

static int shm_tot = 0;  /* total number of shared memory pages */
static int shm_rss = 0; /* number of shared memory pages that are in memory */
static int shm_swp = 0; /* number of shared memory pages that are in swap */
static int max_shmid = 0; /* every used id is <= max_shmid */
static struct wait_queue *shm_lock = NULL;
static struct shmid_ds *shm_segs[SHMMNI];

static unsigned short shm_seq = 0; /* incremented, for recognizing stale ids */

/* some statistics */
static ulong swap_attempts = 0;
static ulong swap_successes = 0;
static ulong used_segs = 0;

void shm_init (void)
{
	int id;
    
       	for (id = 0; id < SHMMNI; id++) 
		shm_segs[id] = (struct shmid_ds *) IPC_UNUSED;
	shm_tot = shm_rss = shm_seq = max_shmid = used_segs = 0;
	shm_lock = NULL;
	return;
}

static int findkey (key_t key)    
{
	int id;
	struct shmid_ds *shp;
	
	for (id=0; id <= max_shmid; id++) {
		while ((shp = shm_segs[id]) == IPC_NOID) 
			sleep_on (&shm_lock);
		if (shp == IPC_UNUSED)
			continue;
		if (key == shp->shm_perm.key) 
			return id;
	}
	return -1;
}

/* 
 * allocate new shmid_ds and pgtable. protected by shm_segs[id] = NOID.
 */
static int newseg (key_t key, int shmflg, int size)
{
	struct shmid_ds *shp;
	int numpages = (size + PAGE_SIZE -1) >> PAGE_SHIFT;
	int id, i;

	if (size < SHMMIN)
		return -EINVAL;
	if (shm_tot + numpages >= SHMALL)
		return -ENOSPC;
	for (id=0; id < SHMMNI; id++)
		if (shm_segs[id] == IPC_UNUSED) {
			shm_segs[id] = (struct shmid_ds *) IPC_NOID;
			goto found;
		}
	return -ENOSPC;

found:
	shp = (struct shmid_ds *) kmalloc (sizeof (*shp), GFP_KERNEL);
	if (!shp) {
		shm_segs[id] = (struct shmid_ds *) IPC_UNUSED;
		if (shm_lock)
			wake_up (&shm_lock);
		return -ENOMEM;
	}

	shp->shm_pages = (ulong *) kmalloc (numpages*sizeof(ulong),GFP_KERNEL);
	if (!shp->shm_pages) {
		shm_segs[id] = (struct shmid_ds *) IPC_UNUSED;
		if (shm_lock)
			wake_up (&shm_lock);
		kfree_s (shp, sizeof (*shp));
		return -ENOMEM;
	}

	for (i=0; i< numpages; shp->shm_pages[i++] = 0);
	shm_tot += numpages;
	shp->shm_perm.key = key;
	shp->shm_perm.mode = (shmflg & S_IRWXUGO);
	shp->shm_perm.cuid = shp->shm_perm.uid = current->euid;
	shp->shm_perm.cgid = shp->shm_perm.gid = current->egid;
	shp->shm_perm.seq = shm_seq;
	shp->shm_segsz = size;
	shp->shm_cpid = current->pid;
	shp->attaches = NULL;
	shp->shm_lpid = shp->shm_nattch = 0;
	shp->shm_atime = shp->shm_dtime = 0;
	shp->shm_ctime = CURRENT_TIME;
	shp->shm_npages = numpages;

	if (id > max_shmid)
		max_shmid = id;
	shm_segs[id] = shp;
	used_segs++;
	if (shm_lock)
		wake_up (&shm_lock);
	return id + (int)shm_seq*SHMMNI;
}

int sys_shmget (key_t key, int size, int shmflg)
{
	struct shmid_ds *shp;
	int id = 0;
	
	if (size < 0 || size > SHMMAX)
		return -EINVAL;
	if (key == IPC_PRIVATE) 
		return newseg(key, shmflg, size);
	if ((id = findkey (key)) == -1) {
		if (!(shmflg & IPC_CREAT))
			return -ENOENT;
		return newseg(key, shmflg, size);
	} 
	if ((shmflg & IPC_CREAT) && (shmflg & IPC_EXCL))
		return -EEXIST;
	shp = shm_segs[id];
	if (shp->shm_perm.mode & SHM_DEST)
		return -EIDRM;
	if (size > shp->shm_segsz)
		return -EINVAL;
	if (ipcperms (&shp->shm_perm, shmflg))
		return -EACCES;
	return shp->shm_perm.seq*SHMMNI + id;
}

/* 
 * Only called after testing nattch and SHM_DEST.
 * Here pages, pgtable and shmid_ds are freed.
 */
static void killseg (int id)
{
	struct shmid_ds *shp;
	int i, numpages;
	ulong page;

	shp = shm_segs[id];
	if (shp == IPC_NOID || shp == IPC_UNUSED) {
		printk ("shm nono: killseg called on unused seg id=%d\n", id);
		return;
	}
	shp->shm_perm.seq++;     /* for shmat */
	numpages = shp->shm_npages; 
	shm_seq++;
	shm_segs[id] = (struct shmid_ds *) IPC_UNUSED;
	used_segs--;
	if (id == max_shmid) 
		while (max_shmid && (shm_segs[--max_shmid] == IPC_UNUSED));
	if (!shp->shm_pages) {
		printk ("shm nono: killseg shp->pages=NULL. id=%d\n", id);
		return;
	}
	for (i=0; i< numpages ; i++) {
		if (!(page = shp->shm_pages[i]))
			continue;
		if (page & 1) {
			free_page (page & PAGE_MASK);
			shm_rss--;
		} else {
			swap_free (page);
			shm_swp--;
		}
	}
	kfree_s (shp->shm_pages, numpages * sizeof (ulong));
	shm_tot -= numpages;
	kfree_s (shp, sizeof (*shp));
	return;
}

int sys_shmctl (int shmid, int cmd, struct shmid_ds *buf)
{
	struct shmid_ds *shp, tbuf;
	struct ipc_perm *ipcp;
	int id, err;
	
	if (cmd < 0 || shmid < 0)
		return -EINVAL;
	if (cmd == IPC_SET) {
		if (!buf)
			return -EFAULT;
		err = verify_area (VERIFY_READ, buf, sizeof (*buf));
		if (err)
			return err;
		memcpy_fromfs (&tbuf, buf, sizeof (*buf));
	}

	switch (cmd) { /* replace with proc interface ? */
	case IPC_INFO: 
	{
		struct shminfo shminfo;
		if (!buf)
			return -EFAULT;
		shminfo.shmmni = SHMMNI;
		shminfo.shmmax = SHMMAX;
		shminfo.shmmin = SHMMIN;
		shminfo.shmall = SHMALL;
		shminfo.shmseg = SHMSEG;
		err = verify_area (VERIFY_WRITE, buf, sizeof (struct shminfo));
		if (err)
			return err;
		memcpy_tofs (buf, &shminfo, sizeof(struct shminfo));
		return max_shmid;
	}
	case SHM_INFO: 
	{ 
		struct shm_info shm_info;
		if (!buf)
			return -EFAULT;
		err = verify_area (VERIFY_WRITE, buf, sizeof (shm_info));
		if (err)
			return err;
		shm_info.used_ids = used_segs; 
		shm_info.shm_rss = shm_rss;
		shm_info.shm_tot = shm_tot;
		shm_info.shm_swp = shm_swp;
		shm_info.swap_attempts = swap_attempts;
		shm_info.swap_successes = swap_successes;
		memcpy_tofs (buf, &shm_info, sizeof(shm_info));
		return max_shmid;
	}
	case SHM_STAT:
		if (!buf)
			return -EFAULT;
		err = verify_area (VERIFY_WRITE, buf, sizeof (*shp));
		if (err)
			return err;
		if (shmid > max_shmid)
			return -EINVAL;
		shp = shm_segs[shmid];
		if (shp == IPC_UNUSED || shp == IPC_NOID)
			return -EINVAL;
		if (ipcperms (&shp->shm_perm, S_IRUGO))
			return -EACCES;
		id = shmid + shp->shm_perm.seq * SHMMNI; 
		memcpy_tofs (buf, shp, sizeof(*shp));
		return id;
	}
	
	shp = shm_segs[id = shmid % SHMMNI];
	if (shp == IPC_UNUSED || shp == IPC_NOID)
		return -EINVAL;
	ipcp = &shp->shm_perm;
	if (ipcp->seq != shmid / SHMMNI) 
		return -EIDRM;
	
	switch (cmd) {
	case SHM_UNLOCK:
		if (!suser())
			return -EPERM;
		if (!(ipcp->mode & SHM_LOCKED))
			return -EINVAL;
		ipcp->mode &= ~SHM_LOCKED;
		break;
	case SHM_LOCK:
/* Allow superuser to lock segment in memory */
/* Should the pages be faulted in here or leave it to user? */
/* need to determine interaction with current->swappable */
		if (!suser())
			return -EPERM;
		if (ipcp->mode & SHM_LOCKED)
			return -EINVAL;
		ipcp->mode |= SHM_LOCKED;
		break;
	case IPC_STAT:
		if (ipcperms (ipcp, S_IRUGO))
			return -EACCES;
		if (!buf)
			return -EFAULT;
		err = verify_area (VERIFY_WRITE, buf, sizeof (*shp));
		if (err)
			return err;
		memcpy_tofs (buf, shp, sizeof(*shp));
		break;
	case IPC_SET:
		if (suser() || current->euid == shp->shm_perm.uid ||
		    current->euid == shp->shm_perm.cuid) {
			ipcp->uid = tbuf.shm_perm.uid;
			ipcp->gid = tbuf.shm_perm.gid;
			ipcp->mode = (ipcp->mode & ~S_IRWXUGO)
				| (tbuf.shm_perm.mode & S_IRWXUGO);
			shp->shm_ctime = CURRENT_TIME;
			break;
		}
		return -EPERM;
	case IPC_RMID:
		if (suser() || current->euid == shp->shm_perm.uid ||
		    current->euid == shp->shm_perm.cuid) {
			shp->shm_perm.mode |= SHM_DEST;
			if (shp->shm_nattch <= 0) 
				killseg (id);
			break;
		}
		return -EPERM;
	default:
		return -EINVAL;
	}
	return 0;
}

/*
 * check range is unmapped, ensure page tables exist
 * mark page table entries with shm_sgn.
 * if remap != 0 the range is remapped.
 */
static int shm_map (struct shm_desc *shmd, int remap)
{
	unsigned long invalid = 0;
	unsigned long *page_table;
	unsigned long tmp, shm_sgn;
	unsigned long page_dir = shmd->task->tss.cr3;
	
	/* check that the range is unmapped and has page_tables */
	for (tmp = shmd->start; tmp < shmd->end; tmp += PAGE_SIZE) { 
		page_table = PAGE_DIR_OFFSET(page_dir,tmp);
		if (*page_table & PAGE_PRESENT) {
			page_table = (ulong *) (PAGE_MASK & *page_table);
			page_table += ((tmp >> PAGE_SHIFT) & (PTRS_PER_PAGE-1));
			if (*page_table) {
				if (!remap)
					return -EINVAL;
				if (*page_table & PAGE_PRESENT) {
					--current->rss;
					free_page (*page_table & PAGE_MASK);
				}
				else
					swap_free (*page_table);
				invalid++;
			}
			continue;
		}  
	      {
		unsigned long new_pt;
		if(!(new_pt = get_free_page(GFP_KERNEL)))	/* clearing needed?  SRB. */
			return -ENOMEM;
		*page_table = new_pt | PAGE_TABLE;
		tmp |= ((PAGE_SIZE << 10) - PAGE_SIZE);
	}}
	if (invalid)
		invalidate();

	/* map page range */
	shm_sgn = shmd->shm_sgn;
	for (tmp = shmd->start; tmp < shmd->end; tmp += PAGE_SIZE, 
	     shm_sgn += (1 << SHM_IDX_SHIFT)) { 
		page_table = PAGE_DIR_OFFSET(page_dir,tmp);
		page_table = (ulong *) (PAGE_MASK & *page_table);
		page_table += (tmp >> PAGE_SHIFT) & (PTRS_PER_PAGE-1);
		*page_table = shm_sgn;
	}
	return 0;
}

/* 
 * Fix shmaddr, allocate descriptor, map shm, add attach descriptor to lists.
 * raddr is needed to return addresses above 2Gig.
 * Specific attaches are allowed over the executable....
 */
int sys_shmat (int shmid, char *shmaddr, int shmflg, ulong *raddr)
{
	struct shmid_ds *shp;
	struct shm_desc *shmd;
	int err;
	unsigned int id;
	unsigned long addr;
	
	if (shmid < 0)
		return -EINVAL;

	if (raddr) {
		err = verify_area(VERIFY_WRITE, raddr, sizeof(long));
		if (err)
			return err;
	}

	shp = shm_segs[id = shmid % SHMMNI];
	if (shp == IPC_UNUSED || shp == IPC_NOID)
		return -EINVAL;

	if (!(addr = (ulong) shmaddr)) {
		if (shmflg & SHM_REMAP)
			return -EINVAL;
		/* set addr below  all current unspecified attaches */
		addr = SHM_RANGE_END; 
		for (shmd = current->shm; shmd; shmd = shmd->task_next) {
			if (shmd->start < SHM_RANGE_START)
				continue;
			if (addr >= shmd->start)
				addr = shmd->start;
		}
		addr = (addr - shp->shm_segsz) & PAGE_MASK;
	} else if (addr & (SHMLBA-1)) {
		if (shmflg & SHM_RND) 
			addr &= ~(SHMLBA-1);       /* round down */
		else
			return -EINVAL;
	}
	if ((addr > current->start_stack - 16384 - PAGE_SIZE*shp->shm_npages))
		return -EINVAL;
	if (shmflg & SHM_REMAP)
		for (shmd = current->shm; shmd; shmd = shmd->task_next) {
			if (addr >= shmd->start && addr < shmd->end)
				return -EINVAL;
			if (addr + shp->shm_segsz >= shmd->start && 
			    addr + shp->shm_segsz < shmd->end)
				return -EINVAL;
		}

	if (ipcperms(&shp->shm_perm, shmflg & SHM_RDONLY ? S_IRUGO : S_IRUGO|S_IWUGO))
		return -EACCES;
	if (shp->shm_perm.seq != shmid / SHMMNI) 
		return -EIDRM;

	shmd = (struct shm_desc *) kmalloc (sizeof(*shmd), GFP_KERNEL);
	if (!shmd)
		return -ENOMEM;
	if ((shp != shm_segs[id]) || (shp->shm_perm.seq != shmid / SHMMNI)) {
		kfree_s (shmd, sizeof (*shmd));
		return -EIDRM;
	}
	shmd->shm_sgn = (SHM_SWP_TYPE << 1) | (id << SHM_ID_SHIFT) |
		(shmflg & SHM_RDONLY ? SHM_READ_ONLY : 0);
       	shmd->start = addr;
	shmd->end = addr + shp->shm_npages * PAGE_SIZE;
	shmd->task = current;

	shp->shm_nattch++;            /* prevent destruction */
	if (addr < current->end_data) {
		iput (current->executable);
		current->executable = NULL;
/*		current->end_data = current->end_code = 0; */
	}

	if ((err = shm_map (shmd, shmflg & SHM_REMAP))) {
		if (--shp->shm_nattch <= 0 && shp->shm_perm.mode & SHM_DEST)
			killseg(id);
		kfree_s (shmd, sizeof (*shmd));
		return err;
	}
		
	shmd->task_next = current->shm;
	current->shm = shmd;
	shmd->seg_next = shp->attaches;
	shp->attaches = shmd;
	shp->shm_lpid = current->pid;
	shp->shm_atime = CURRENT_TIME;
	if (!raddr)
		return addr;
	put_fs_long (addr, raddr);
	return 0;
}

/*
 * remove the first attach descriptor from the list *shmdp.
 * free memory for segment if it is marked destroyed.
 * The descriptor is detached before the sleep in unmap_page_range.
 */
static void detach (struct shm_desc **shmdp)
{
 	struct shm_desc *shmd = *shmdp; 
  	struct shmid_ds *shp;
  	int id;
	
	id = (shmd->shm_sgn >> SHM_ID_SHIFT) & SHM_ID_MASK;
  	shp = shm_segs[id];
 	*shmdp = shmd->task_next;
 	for (shmdp = &shp->attaches; *shmdp; shmdp = &(*shmdp)->seg_next)
		if (*shmdp == shmd) {
			*shmdp = shmd->seg_next; 
			goto found; 
		}
 	printk("detach: shm segment (id=%d) attach list inconsistent\n",id);
	
 found:
	unmap_page_range (shmd->start, shp->shm_segsz); /* sleeps */
	kfree_s (shmd, sizeof (*shmd));
  	shp->shm_lpid = current->pid;
	shp->shm_dtime = CURRENT_TIME;
	if (--shp->shm_nattch <= 0 && shp->shm_perm.mode & SHM_DEST)
		killseg (id); /* sleeps */
  	return;
}

/*
 * detach and kill segment if marked destroyed.
 * The work is done in detach.
 */
int sys_shmdt (char *shmaddr)
{
	struct shm_desc *shmd, **shmdp;	
	
	for (shmdp = &current->shm; (shmd = *shmdp); shmdp=&shmd->task_next) { 
		if (shmd->start == (ulong) shmaddr) {
			detach (shmdp);
			return 0;
		}
	}
	return -EINVAL;
}

/* 
 * detach all attached segments. 
 */
void shm_exit (void)
{
 	while (current->shm) 
		detach(&current->shm);
  	return;
}

/* 
 * copy the parent shm descriptors and update nattch
 * parent is stuck in fork so an attach on each segment is assured.
 * copy_page_tables does the mapping.
 */
int shm_fork (struct task_struct *p1, struct task_struct *p2)
{
	struct shm_desc *shmd, *new_desc = NULL, *tmp;
	struct shmid_ds *shp;
	int id;

        if (!p1->shm)
		return 0;
	for (shmd = p1->shm; shmd; shmd = shmd->task_next) {
		tmp = (struct shm_desc *) kmalloc(sizeof(*tmp), GFP_KERNEL);
		if (!tmp) {
			while (new_desc) { 
				tmp = new_desc->task_next; 
				kfree_s (new_desc, sizeof (*new_desc)); 
				new_desc = tmp; 
			}
			free_page_tables (p2);
			return -ENOMEM;
		}
		*tmp = *shmd;
		tmp->task = p2;
		tmp->task_next = new_desc;
		new_desc = tmp;
	}
	p2->shm = new_desc;
	for (shmd = new_desc; shmd; shmd = shmd->task_next) {
		id = (shmd->shm_sgn >> SHM_ID_SHIFT) & SHM_ID_MASK;
		shp = shm_segs[id];
		if (shp == IPC_UNUSED) {
			printk("shm_fork: unused id=%d PANIC\n", id);
			return -ENOMEM;
		}
		shmd->seg_next = shp->attaches;
		shp->attaches = shmd;
		shp->shm_nattch++;
		shp->shm_atime = CURRENT_TIME;
		shp->shm_lpid = current->pid;
	}
	return 0;
}

/*
 * page not present ... go through shm_pages .. called from swap_in()
 */
void shm_no_page (unsigned long *ptent)
{
	unsigned long page;
	unsigned long code = *ptent;
	struct shmid_ds *shp;
	unsigned int id, idx;

	id = (code >> SHM_ID_SHIFT) & SHM_ID_MASK;
	if (id > max_shmid) {
		printk ("shm_no_page: id=%d too big. proc mem corruptedn", id);
		return;
	}
	shp = shm_segs[id];
	if (shp == IPC_UNUSED || shp == IPC_NOID) {
		printk ("shm_no_page: id=%d invalid. Race.\n", id);
		return;
	}
	idx = (code >> SHM_IDX_SHIFT) & SHM_IDX_MASK;
	if (idx >= shp->shm_npages) {
		printk ("shm_no_page : too large page index. id=%d\n", id);
		return;
	}

	if (!(shp->shm_pages[idx] & PAGE_PRESENT)) {
		if(!(page = get_free_page(GFP_KERNEL))) {
			oom(current);
			*ptent = BAD_PAGE | PAGE_ACCESSED | 7;
			return;
		}
		if (shp->shm_pages[idx] & PAGE_PRESENT) {
			free_page (page);
			goto done;
		}
		if (shp->shm_pages[idx]) {
			read_swap_page (shp->shm_pages[idx], (char *) page);
			if (shp->shm_pages[idx] & PAGE_PRESENT)  {
				free_page (page);
				goto done;
			}
			swap_free (shp->shm_pages[idx]);
			shm_swp--;
		}
		shm_rss++;
		shp->shm_pages[idx] = page | (PAGE_SHARED | PAGE_DIRTY);
	} else 
		--current->maj_flt;  /* was incremented in do_no_page */

done:
	current->min_flt++;
	page = shp->shm_pages[idx];
	if (code & SHM_READ_ONLY)           /* write-protect */
		page &= ~2;
	mem_map[MAP_NR(page)]++;
	*ptent = page;
	return;
}

/*
 * Goes through counter = (shm_rss << prio) present shm pages. 
 */
static unsigned long swap_id = 0; /* currently being swapped */
static unsigned long swap_idx = 0; /* next to swap */

int shm_swap (int prio)
{
	unsigned long page;
	struct shmid_ds *shp;
	struct shm_desc *shmd;
	unsigned int swap_nr;
	unsigned long id, idx, invalid = 0;
	int counter;

	counter = shm_rss >> prio;
	if (!counter || !(swap_nr = get_swap_page()))
		return 0;

 check_id:
	shp = shm_segs[swap_id];
	if (shp == IPC_UNUSED || shp == IPC_NOID || shp->shm_perm.mode & SHM_LOCKED ) {
		swap_idx = 0; 
		if (++swap_id > max_shmid)
			swap_id = 0;
		goto check_id;
	}
	id = swap_id;

 check_table:
	idx = swap_idx++; 
	if (idx  >= shp->shm_npages) { 
		swap_idx = 0;
		if (++swap_id > max_shmid)
			swap_id = 0;
		goto check_id;
	}

	page = shp->shm_pages[idx];
	if (!(page & PAGE_PRESENT))
		goto check_table;
	swap_attempts++;

	if (--counter < 0) { /* failed */
		if (invalid)
			invalidate();
		swap_free (swap_nr);
		return 0;
	}
	for (shmd = shp->attaches; shmd; shmd = shmd->seg_next) {
		unsigned long tmp, *pte;
		if ((shmd->shm_sgn >> SHM_ID_SHIFT & SHM_ID_MASK) != id) {
			printk ("shm_swap: id=%ld does not match shmd\n", id);
			continue;
		}
		tmp = shmd->start + (idx << PAGE_SHIFT);
		if (tmp >= shmd->end) {
			printk ("shm_swap: too large idx=%ld id=%ld PANIC\n",idx, id);
			continue;
		}
		pte = PAGE_DIR_OFFSET(shmd->task->tss.cr3,tmp);
		if (!(*pte & 1)) { 
			printk("shm_swap: bad pgtbl! id=%ld start=%lx idx=%ld\n", 
					id, shmd->start, idx);
			*pte = 0;
			continue;
		} 
		pte = (ulong *) (PAGE_MASK & *pte);
		pte += ((tmp >> PAGE_SHIFT) & (PTRS_PER_PAGE-1));
		tmp = *pte;
		if (!(tmp & PAGE_PRESENT))
			continue;
		if (tmp & PAGE_ACCESSED) {
			*pte &= ~PAGE_ACCESSED;
			continue;  
		}
		tmp = shmd->shm_sgn | idx << SHM_IDX_SHIFT;
		*pte = tmp;
		mem_map[MAP_NR(page)]--;
		shmd->task->rss--;
		invalid++;
	}

	if (mem_map[MAP_NR(page)] != 1) 
		goto check_table;
	page &= PAGE_MASK;
	shp->shm_pages[idx] = swap_nr;
	if (invalid)
		invalidate();
	write_swap_page (swap_nr, (char *) page);
	free_page (page);
	swap_successes++;
	shm_swp++;
	shm_rss--;
	return 1;
}
