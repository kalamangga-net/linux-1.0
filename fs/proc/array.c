/*
 *  linux/fs/proc/array.c
 *
 *  Copyright (C) 1992  by Linus Torvalds
 *  based on ideas by Darren Senn
 *
 *  stat,statm extensions by Michael K. Johnson, johnsonm@stolaf.edu
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/kernel_stat.h>
#include <linux/tty.h>
#include <linux/user.h>
#include <linux/a.out.h>
#include <linux/string.h>
#include <linux/mman.h>

#include <asm/segment.h>
#include <asm/io.h>

#define LOAD_INT(x) ((x) >> FSHIFT)
#define LOAD_FRAC(x) LOAD_INT(((x) & (FIXED_1-1)) * 100)

#ifdef CONFIG_DEBUG_MALLOC
int get_malloc(char * buffer);
#endif

static int read_core(struct inode * inode, struct file * file,char * buf, int count)
{
	unsigned long p = file->f_pos;
	int read;
	int count1;
	char * pnt;
	struct user dump;

	memset(&dump, 0, sizeof(struct user));
	dump.magic = CMAGIC;
	dump.u_dsize = high_memory >> 12;

	if (count < 0)
		return -EINVAL;
	if (p >= high_memory + PAGE_SIZE)
		return 0;
	if (count > high_memory + PAGE_SIZE - p)
		count = high_memory + PAGE_SIZE - p;
	read = 0;

	if (p < sizeof(struct user) && count > 0) {
		count1 = count;
		if (p + count1 > sizeof(struct user))
			count1 = sizeof(struct user)-p;
		pnt = (char *) &dump + p;
		memcpy_tofs(buf,(void *) pnt, count1);
		buf += count1;
		p += count1;
		count -= count1;
		read += count1;
	}

	while (p < 2*PAGE_SIZE && count > 0) {
		put_fs_byte(0,buf);
		buf++;
		p++;
		count--;
		read++;
	}
	memcpy_tofs(buf,(void *) (p - PAGE_SIZE),count);
	read += count;
	file->f_pos += read;
	return read;
}

static int get_loadavg(char * buffer)
{
	int a, b, c;

	a = avenrun[0] + (FIXED_1/200);
	b = avenrun[1] + (FIXED_1/200);
	c = avenrun[2] + (FIXED_1/200);
	return sprintf(buffer,"%d.%02d %d.%02d %d.%02d\n",
		LOAD_INT(a), LOAD_FRAC(a),
		LOAD_INT(b), LOAD_FRAC(b),
		LOAD_INT(c), LOAD_FRAC(c));
}

static int get_kstat(char * buffer)
{
        return sprintf(buffer,	"cpu  %u %u %u %lu\n"
        			"disk %u %u %u %u\n"
        			"page %u %u\n"
        			"swap %u %u\n"
        			"intr %u\n"
        			"ctxt %u\n"
        			"btime %lu\n",
                kstat.cpu_user,
                kstat.cpu_nice,
                kstat.cpu_system,
                jiffies - (kstat.cpu_user + kstat.cpu_nice + kstat.cpu_system),
                kstat.dk_drive[0],
                kstat.dk_drive[1],
                kstat.dk_drive[2],
                kstat.dk_drive[3],
                kstat.pgpgin,
                kstat.pgpgout,
                kstat.pswpin,
                kstat.pswpout,
                kstat.interrupts,
                kstat.context_swtch,
                xtime.tv_sec - jiffies / HZ);
}


static int get_uptime(char * buffer)
{
	unsigned long uptime;
	unsigned long idle;

	uptime = jiffies;
	idle = task[0]->utime + task[0]->stime;
	return sprintf(buffer,"%lu.%02lu %lu.%02lu\n",
		uptime / HZ,
		uptime % HZ,
		idle / HZ,
		idle % HZ);
}

static int get_meminfo(char * buffer)
{
	struct sysinfo i;

	si_meminfo(&i);
	si_swapinfo(&i);
	return sprintf(buffer, "        total:   used:    free:   shared:  buffers:\n"
		"Mem:  %8lu %8lu %8lu %8lu %8lu\n"
		"Swap: %8lu %8lu %8lu\n",
		i.totalram, i.totalram-i.freeram, i.freeram, i.sharedram, i.bufferram,
		i.totalswap, i.totalswap-i.freeswap, i.freeswap);
}

static int get_version(char * buffer)
{
	extern char *linux_banner;

	strcpy(buffer, linux_banner);
	return strlen(buffer);
}

static struct task_struct ** get_task(pid_t pid)
{
	struct task_struct ** p;

	p = task;
	while (++p < task+NR_TASKS) {
		if (*p && (*p)->pid == pid)
			return p;
	}
	return NULL;
}

static unsigned long get_phys_addr(struct task_struct ** p, unsigned long ptr)
{
	unsigned long page;

	if (!p || !*p || ptr >= TASK_SIZE)
		return 0;
	page = *PAGE_DIR_OFFSET((*p)->tss.cr3,ptr);
	if (!(page & 1))
		return 0;
	page &= PAGE_MASK;
	page += PAGE_PTR(ptr);
	page = *(unsigned long *) page;
	if (!(page & 1))
		return 0;
	page &= PAGE_MASK;
	page += ptr & ~PAGE_MASK;
	return page;
}

static int get_array(struct task_struct ** p, unsigned long start, unsigned long end, char * buffer)
{
	unsigned long addr;
	int size = 0, result = 0;
	char c;

	if (start >= end)
		return result;
	for (;;) {
		addr = get_phys_addr(p, start);
		if (!addr)
			return result;
		do {
			c = *(char *) addr;
			if (!c)
				result = size;
			if (size < PAGE_SIZE)
				buffer[size++] = c;
			else
				return result;
			addr++;
			start++;
			if (start >= end)
				return result;
		} while (!(addr & ~PAGE_MASK));
	}
}

static int get_env(int pid, char * buffer)
{
	struct task_struct ** p = get_task(pid);

	if (!p || !*p)
		return 0;
	return get_array(p, (*p)->env_start, (*p)->env_end, buffer);
}

static int get_arg(int pid, char * buffer)
{
	struct task_struct ** p = get_task(pid);

	if (!p || !*p)
		return 0;
	return get_array(p, (*p)->arg_start, (*p)->arg_end, buffer);
}

static unsigned long get_wchan(struct task_struct *p)
{
	unsigned long ebp, eip;
	unsigned long stack_page;
	int count = 0;

	if (!p || p == current || p->state == TASK_RUNNING)
		return 0;
	stack_page = p->kernel_stack_page;
	if (!stack_page)
		return 0;
	ebp = p->tss.ebp;
	do {
		if (ebp < stack_page || ebp >= 4092+stack_page)
			return 0;
		eip = *(unsigned long *) (ebp+4);
		if ((void *)eip != sleep_on &&
		    (void *)eip != interruptible_sleep_on)
			return eip;
		ebp = *(unsigned long *) ebp;
	} while (count++ < 16);
	return 0;
}

#define	KSTK_EIP(stack)	(((unsigned long *)stack)[1019])
#define	KSTK_ESP(stack)	(((unsigned long *)stack)[1022])

static int get_stat(int pid, char * buffer)
{
	struct task_struct ** p = get_task(pid);
	unsigned long sigignore=0, sigcatch=0, bit=1, wchan;
	unsigned long vsize, eip, esp;
	int i,tty_pgrp;
	char state;

	if (!p || !*p)
		return 0;
	if ((*p)->state < 0 || (*p)->state > 5)
		state = '.';
	else
		state = "RSDZTD"[(*p)->state];
	eip = esp = 0;
	vsize = (*p)->kernel_stack_page;
	if (vsize) {
		eip = KSTK_EIP(vsize);
		esp = KSTK_ESP(vsize);
		vsize = (*p)->brk - (*p)->start_code + PAGE_SIZE-1;
		if (esp)
			vsize += TASK_SIZE - esp;
	}
	wchan = get_wchan(*p);
	for(i=0; i<32; ++i) {
		switch((int) (*p)->sigaction[i].sa_handler) {
		case 1: sigignore |= bit; break;
		case 0: break;
		default: sigcatch |= bit;
		} bit <<= 1;
	}
	tty_pgrp = (*p)->tty;
	if (tty_pgrp > 0 && tty_table[tty_pgrp])
		tty_pgrp = tty_table[tty_pgrp]->pgrp;
	else
		tty_pgrp = -1;
	return sprintf(buffer,"%d (%s) %c %d %d %d %d %d %lu %lu \
%lu %lu %lu %ld %ld %ld %ld %ld %ld %lu %lu %ld %lu %u %u %lu %lu %lu %lu %lu %lu \
%lu %lu %lu %lu\n",
		pid,
		(*p)->comm,
		state,
		(*p)->p_pptr->pid,
		(*p)->pgrp,
		(*p)->session,
		(*p)->tty,
		tty_pgrp,
		(*p)->flags,
		(*p)->min_flt,
		(*p)->cmin_flt,
		(*p)->maj_flt,
		(*p)->cmaj_flt,
		(*p)->utime,
		(*p)->stime,
		(*p)->cutime,
		(*p)->cstime,
		(*p)->counter,  /* this is the kernel priority ---
				   subtract 30 in your user-level program. */
		(*p)->priority, /* this is the nice value ---
				   subtract 15 in your user-level program. */
		(*p)->timeout,
		(*p)->it_real_value,
		(*p)->start_time,
		vsize,
		(*p)->rss, /* you might want to shift this left 3 */
		(*p)->rlim[RLIMIT_RSS].rlim_cur,
		(*p)->start_code,
		(*p)->end_code,
		(*p)->start_stack,
		esp,
		eip,
		(*p)->signal,
		(*p)->blocked,
		sigignore,
		sigcatch,
		wchan);
}

static int get_statm(int pid, char * buffer)
{
	struct task_struct ** p = get_task(pid);
	int i, tpag;
	int size=0, resident=0, share=0, trs=0, lrs=0, drs=0, dt=0;
	unsigned long ptbl, *buf, *pte, *pagedir, map_nr;

	if (!p || !*p)
		return 0;
	tpag = (*p)->end_code / PAGE_SIZE;
	if ((*p)->state != TASK_ZOMBIE) {
	  pagedir = (unsigned long *) (*p)->tss.cr3;
	  for (i = 0; i < 0x300; ++i) {
	    if ((ptbl = pagedir[i]) == 0) {
	      tpag -= PTRS_PER_PAGE;
	      continue;
	    }
	    buf = (unsigned long *)(ptbl & PAGE_MASK);
	    for (pte = buf; pte < (buf + PTRS_PER_PAGE); ++pte) {
	      if (*pte != 0) {
		++size;
		if (*pte & 1) {
		  ++resident;
		  if (tpag > 0)
		    ++trs;
		  else
		    ++drs;
		  if (i >= 15 && i < 0x2f0) {
		    ++lrs;
		    if (*pte & 0x40)
		      ++dt;
		    else
		      --drs;
		  }
		  map_nr = MAP_NR(*pte);
		  if (map_nr < (high_memory / PAGE_SIZE) && mem_map[map_nr] > 1)
		    ++share;
		}
	      }
	      --tpag;
	    }
	  }
	}
	return sprintf(buffer,"%d %d %d %d %d %d %d\n",
		       size, resident, share, trs, lrs, drs, dt);
}

static int get_maps(int pid, char *buf)
{
	int sz = 0;
	struct task_struct **p = get_task(pid);
	struct vm_area_struct *map;

	if (!p || !*p)
		return 0;

	for(map = (*p)->mmap; map != NULL; map = map->vm_next) {
		char str[7], *cp = str;
		int prot = map->vm_page_prot;
		int perms, flags;
		int end = sz + 80;	/* Length of line */
		dev_t dev;
		unsigned long ino;

		/*
		 * This tries to get an "rwxsp" string out of silly
		 * intel page permissions.  The vm_area_struct should
		 * probably have the original mmap args preserved.
		 */
		
		flags = perms = 0;

		if ((prot & PAGE_READONLY) == PAGE_READONLY)
			perms |= PROT_READ | PROT_EXEC;
		if (prot & (PAGE_COW|PAGE_RW)) {
			perms |= PROT_WRITE | PROT_READ;
			flags = prot & PAGE_COW ? MAP_PRIVATE : MAP_SHARED;
		}

		*cp++ = perms & PROT_READ ? 'r' : '-';
		*cp++ = perms & PROT_WRITE ? 'w' : '-';
		*cp++ = perms & PROT_EXEC ? 'x' : '-';
		*cp++ = flags & MAP_SHARED ? 's' : '-';
		*cp++ = flags & MAP_PRIVATE ? 'p' : '-';
		*cp++ = 0;
		
		if (end >= PAGE_SIZE) {
			sprintf(buf+sz, "...\n");
			break;
		}
		
		if (map->vm_inode != NULL) {
			dev = map->vm_inode->i_dev;
			ino = map->vm_inode->i_ino;
		} else {
			dev = 0;
			ino = 0;
		}

		sz += sprintf(buf+sz, "%08lx-%08lx %s %08lx %02x:%02x %lu\n",
			      map->vm_start, map->vm_end, str, map->vm_offset,
			      MAJOR(dev),MINOR(dev), ino);
		if (sz > end) {
			printk("get_maps: end(%d) < sz(%d)\n", end, sz);
			break;
		}
	}
	
	return sz;
}

extern int get_module_list(char *);

static int array_read(struct inode * inode, struct file * file,char * buf, int count)
{
	char * page;
	int length;
	int end;
	unsigned int type, pid;

	if (count < 0)
		return -EINVAL;
	if (!(page = (char*) __get_free_page(GFP_KERNEL)))
		return -ENOMEM;
	type = inode->i_ino;
	pid = type >> 16;
	type &= 0x0000ffff;
	switch (type) {
		case 2:
			length = get_loadavg(page);
			break;
		case 3:
			length = get_uptime(page);
			break;
		case 4:
			length = get_meminfo(page);
			break;
		case 6:
			length = get_version(page);
			break;
		case 9:
			length = get_env(pid, page);
			break;
		case 10:
			length = get_arg(pid, page);
			break;
		case 11:
			length = get_stat(pid, page);
			break;
		case 12:
			length = get_statm(pid, page);
			break;
#ifdef CONFIG_DEBUG_MALLOC
		case 13:
			length = get_malloc(page);
			break;
#endif
		case 14:
			free_page((unsigned long) page);
			return read_core(inode, file, buf, count);
		case 15:
			length = get_maps(pid, page);
			break;
		case 16:
			length = get_module_list(page);
			break;
		case 17:
			length = get_kstat(page);
			break;
		default:
			free_page((unsigned long) page);
			return -EBADF;
	}
	if (file->f_pos >= length) {
		free_page((unsigned long) page);
		return 0;
	}
	if (count + file->f_pos > length)
		count = length - file->f_pos;
	end = count + file->f_pos;
	memcpy_tofs(buf, page + file->f_pos, count);
	free_page((unsigned long) page);
	file->f_pos = end;
	return count;
}

static struct file_operations proc_array_operations = {
	NULL,		/* array_lseek */
	array_read,
	NULL,		/* array_write */
	NULL,		/* array_readdir */
	NULL,		/* array_select */
	NULL,		/* array_ioctl */
	NULL,		/* mmap */
	NULL,		/* no special open code */
	NULL,		/* no special release code */
	NULL		/* can't fsync */
};

struct inode_operations proc_array_inode_operations = {
	&proc_array_operations,	/* default base directory file-ops */
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
	NULL,			/* bmap */
	NULL,			/* truncate */
	NULL			/* permission */
};
