/*
 * linux/ipc/sem.c
 * Copyright (C) 1992 Krishna Balasubramanian 
 */

#include <linux/errno.h>
#include <asm/segment.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/sem.h>
#include <linux/ipc.h>
#include <linux/stat.h>
#include <linux/malloc.h>

extern int ipcperms (struct ipc_perm *ipcp, short semflg);
static int newary (key_t, int, int);
static int findkey (key_t key);
static void freeary (int id);

static struct semid_ds *semary[SEMMNI];
static int used_sems = 0, used_semids = 0;                    
static struct wait_queue *sem_lock = NULL;
static int max_semid = 0;

static unsigned short sem_seq = 0;

void sem_init (void)
{
	int i=0;
	
	sem_lock = NULL;
	used_sems = used_semids = max_semid = sem_seq = 0;
	for (i=0; i < SEMMNI; i++)
		semary[i] = (struct semid_ds *) IPC_UNUSED;
	return;
}

static int findkey (key_t key)
{
	int id;
	struct semid_ds *sma;
	
	for (id=0; id <= max_semid; id++) {
		while ((sma = semary[id]) == IPC_NOID) 
			interruptible_sleep_on (&sem_lock);
		if (sma == IPC_UNUSED)
			continue;
		if (key == sma->sem_perm.key)
			return id;
	}
	return -1;
}

static int newary (key_t key, int nsems, int semflg)
{
	int id;
	struct semid_ds *sma;
	struct ipc_perm *ipcp;
	int size;

	if (!nsems)
		return -EINVAL;
	if (used_sems + nsems > SEMMNS)
		return -ENOSPC;
	for (id=0; id < SEMMNI; id++) 
		if (semary[id] == IPC_UNUSED) {
			semary[id] = (struct semid_ds *) IPC_NOID;
			goto found;
		}
	return -ENOSPC;
found:
	size = sizeof (*sma) + nsems * sizeof (struct sem);
	used_sems += nsems;
	sma = (struct semid_ds *) kmalloc (size, GFP_KERNEL);
	if (!sma) {
		semary[id] = (struct semid_ds *) IPC_UNUSED;
		used_sems -= nsems;
		if (sem_lock)
			wake_up (&sem_lock);
		return -ENOMEM;
	}
	memset (sma, 0, size);
	sma->sem_base = (struct sem *) &sma[1];
	ipcp = &sma->sem_perm;
	ipcp->mode = (semflg & S_IRWXUGO);
	ipcp->key = key;
	ipcp->cuid = ipcp->uid = current->euid;
	ipcp->gid = ipcp->cgid = current->egid;
	ipcp->seq = sem_seq;
	sma->eventn = sma->eventz = NULL;
	sma->sem_nsems = nsems;
	sma->sem_ctime = CURRENT_TIME;
        if (id > max_semid)
		max_semid = id;
	used_semids++;
	semary[id] = sma;
	if (sem_lock)
		wake_up (&sem_lock);
	return (int) sem_seq * SEMMNI + id;
}

int sys_semget (key_t key, int nsems, int semflg)
{
	int id;
	struct semid_ds *sma;
	
	if (nsems < 0  || nsems > SEMMSL)
		return -EINVAL;
	if (key == IPC_PRIVATE) 
		return newary(key, nsems, semflg);
	if ((id = findkey (key)) == -1) {  /* key not used */
		if (!(semflg & IPC_CREAT))
			return -ENOENT;
		return newary(key, nsems, semflg);
	}
	if (semflg & IPC_CREAT && semflg & IPC_EXCL)
		return -EEXIST;
	sma = semary[id];
	if (nsems > sma->sem_nsems)
		return -EINVAL;
	if (ipcperms(&sma->sem_perm, semflg))
		return -EACCES;
	return sma->sem_perm.seq*SEMMNI + id;
} 

static void freeary (int id)
{
	struct semid_ds *sma = semary[id];
	struct sem_undo *un;

	sma->sem_perm.seq++;
	sem_seq++;
	used_sems -= sma->sem_nsems;
	if (id == max_semid)
		while (max_semid && (semary[--max_semid] == IPC_UNUSED));
	semary[id] = (struct semid_ds *) IPC_UNUSED;
	used_semids--;
	for (un=sma->undo; un; un=un->id_next)
	        un->semadj = 0;
	while (sma->eventz || sma->eventn) {
		if (sma->eventz)
			wake_up (&sma->eventz);
		if (sma->eventn)
			wake_up (&sma->eventn);
		schedule();
	}
	kfree_s (sma, sizeof (*sma) + sma->sem_nsems * sizeof (struct sem));
	return;
}

int sys_semctl (int semid, int semnum, int cmd, void *arg)
{
	int i, id, val = 0;
	struct semid_ds *sma, *buf = NULL, tbuf;
	struct ipc_perm *ipcp;
	struct sem *curr;
	struct sem_undo *un;
	ushort nsems, *array = NULL;
	ushort sem_io[SEMMSL];
	
	if (semid < 0 || semnum < 0 || cmd < 0)
		return -EINVAL;

	switch (cmd) {
	case IPC_INFO: 
	case SEM_INFO: 
	{
		struct seminfo seminfo, *tmp;
		if (!arg || ! (tmp = (struct seminfo *) get_fs_long((int *)arg)))
			return -EFAULT;
		seminfo.semmni = SEMMNI;
		seminfo.semmns = SEMMNS;
		seminfo.semmsl = SEMMSL;
		seminfo.semopm = SEMOPM;
		seminfo.semvmx = SEMVMX;
		seminfo.semmnu = SEMMNU; 
		seminfo.semmap = SEMMAP; 
		seminfo.semume = SEMUME;
		seminfo.semusz = SEMUSZ;
		seminfo.semaem = SEMAEM;
		if (cmd == SEM_INFO) {
			seminfo.semusz = used_semids;
			seminfo.semaem = used_sems;
		}
		i= verify_area(VERIFY_WRITE, tmp, sizeof(struct seminfo));
		if (i)
			return i;
		memcpy_tofs (tmp, &seminfo, sizeof(struct seminfo));
		return max_semid;
	}

	case SEM_STAT:
		if (!arg || ! (buf = (struct semid_ds *) get_fs_long((int *) arg)))
			return -EFAULT;
		i = verify_area (VERIFY_WRITE, buf, sizeof (*sma));
		if (i)
			return i;
		if (semid > max_semid)
			return -EINVAL;
		sma = semary[semid];
		if (sma == IPC_UNUSED || sma == IPC_NOID)
			return -EINVAL;
		if (ipcperms (&sma->sem_perm, S_IRUGO))
			return -EACCES;
		id = semid + sma->sem_perm.seq * SEMMNI; 
		memcpy_tofs (buf, sma, sizeof(*sma));
		return id;
	}

	id = semid % SEMMNI;
	sma = semary [id];
	if (sma == IPC_UNUSED || sma == IPC_NOID)
		return -EINVAL;
	ipcp = &sma->sem_perm;
	nsems = sma->sem_nsems;
	if (ipcp->seq != semid / SEMMNI)
		return -EIDRM;
	if (semnum >= nsems)
		return -EINVAL;
	curr = &sma->sem_base[semnum];

	switch (cmd) {
	case GETVAL:
	case GETPID:
	case GETNCNT:
	case GETZCNT:
	case GETALL:
		if (ipcperms (ipcp, S_IRUGO))
			return -EACCES;
		switch (cmd) {
		case GETVAL : return curr->semval; 
		case GETPID : return curr->sempid;
		case GETNCNT: return curr->semncnt;
		case GETZCNT: return curr->semzcnt;
		case GETALL:
			if (!arg || ! (array = (ushort *) get_fs_long((int *) arg)))
				return -EFAULT;
			i = verify_area (VERIFY_WRITE, array, nsems* sizeof(short));
			if (i)
				return i;
		}
		break;
	case SETVAL: 
		if (!arg)
			return -EFAULT;
		if ((val = (int) get_fs_long ((int *) arg))  > SEMVMX || val < 0) 
			return -ERANGE;
		break;
	case IPC_RMID:
		if (suser() || current->euid == ipcp->cuid || 
		    current->euid == ipcp->uid) {
			freeary (id); 
			return 0;
		}
		return -EPERM;
	case SETALL: /* arg is a pointer to an array of ushort */
		if (!arg || ! (array = (ushort *) get_fs_long ((int *) arg)) )
			return -EFAULT;
		if ((i = verify_area (VERIFY_READ, array, sizeof tbuf)))
			return i;
		memcpy_fromfs (sem_io, array, nsems*sizeof(ushort));
		for (i=0; i< nsems; i++)
			if (sem_io[i] > SEMVMX)
				return -ERANGE;
		break;
	case IPC_STAT:
		if (!arg || !(buf = (struct semid_ds *) get_fs_long((int *) arg))) 
			return -EFAULT;
		if ((i = verify_area (VERIFY_WRITE, arg, sizeof tbuf)))
			return i;
		break;
	case IPC_SET:
		if (!arg || !(buf = (struct semid_ds *) get_fs_long((int *) arg))) 
			return -EFAULT;
		if ((i = verify_area (VERIFY_READ, buf, sizeof tbuf)))
			return i;
		memcpy_fromfs (&tbuf, buf, sizeof tbuf);
		break;
	}
	
	if (semary[id] == IPC_UNUSED || semary[id] == IPC_NOID)
		return -EIDRM;
	if (ipcp->seq != semid / SEMMNI)
		return -EIDRM;
	
	switch (cmd) {
	case GETALL:
		if (ipcperms (ipcp, S_IRUGO))
			return -EACCES;
		for (i=0; i< sma->sem_nsems; i++)
			sem_io[i] = sma->sem_base[i].semval;
		memcpy_tofs (array, sem_io, nsems*sizeof(ushort));
		break;
	case SETVAL:
		if (ipcperms (ipcp, S_IWUGO))
			return -EACCES;
		for (un = sma->undo; un; un = un->id_next)
			if (semnum == un->sem_num)
				un->semadj = 0;
		sma->sem_ctime = CURRENT_TIME;
		curr->semval = val;
		if (sma->eventn)
			wake_up (&sma->eventn);
		if (sma->eventz)
			wake_up (&sma->eventz);
		break;
	case IPC_SET:
		if (suser() || current->euid == ipcp->cuid || 
		    current->euid == ipcp->uid) {
			ipcp->uid = tbuf.sem_perm.uid;
			ipcp->gid = tbuf.sem_perm.gid;
			ipcp->mode = (ipcp->mode & ~S_IRWXUGO)
				| (tbuf.sem_perm.mode & S_IRWXUGO);
			sma->sem_ctime = CURRENT_TIME;
			return 0;
		}
		return -EPERM;
	case IPC_STAT:
		if (ipcperms (ipcp, S_IRUGO))
			return -EACCES;
		memcpy_tofs (buf, sma, sizeof (*sma));
		break;
	case SETALL:
		if (ipcperms (ipcp, S_IWUGO))
			return -EACCES;
		for (i=0; i<nsems; i++) 
			sma->sem_base[i].semval = sem_io[i];
		for (un = sma->undo; un; un = un->id_next)
			un->semadj = 0;
		if (sma->eventn)
			wake_up (&sma->eventn);
		if (sma->eventz)
			wake_up (&sma->eventz);
		sma->sem_ctime = CURRENT_TIME;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

int sys_semop (int semid, struct sembuf *tsops, unsigned nsops)
{
	int i, id;
	struct semid_ds *sma;
	struct sem *curr = NULL;
	struct sembuf sops[SEMOPM], *sop;
	struct sem_undo *un;
	int undos = 0, alter = 0, semncnt = 0, semzcnt = 0;
	
	if (nsops < 1 || semid < 0)
		return -EINVAL;
	if (nsops > SEMOPM)
		return -E2BIG;
	if (!tsops) 
		return -EFAULT;
	memcpy_fromfs (sops, tsops, nsops * sizeof(*tsops));  
	id = semid % SEMMNI;
	if ((sma = semary[id]) == IPC_UNUSED || sma == IPC_NOID)
		return -EINVAL;
	for (i=0; i<nsops; i++) { 
		sop = &sops[i];
		if (sop->sem_num > sma->sem_nsems)
			return -EFBIG;
		if (sop->sem_flg & SEM_UNDO)
			undos++;
		if (sop->sem_op) {
			alter++;
			if (sop->sem_op > 0)
				semncnt ++;
		}
	}
	if (ipcperms(&sma->sem_perm, alter ? S_IWUGO : S_IRUGO))
		return -EACCES;
	/* 
	 * ensure every sop with undo gets an undo structure 
	 */
	if (undos) {
		for (i=0; i<nsops; i++) {
			if (!(sops[i].sem_flg & SEM_UNDO))
				continue;
			for (un = current->semun; un; un = un->proc_next) 
				if ((un->semid == semid) && 
				    (un->sem_num == sops[i].sem_num))
					break;
			if (un)
				continue;
			un = (struct sem_undo *) 
				kmalloc (sizeof(*un), GFP_ATOMIC);
			if (!un)
				return -ENOMEM; /* freed on exit */
			un->semid = semid;
			un->semadj = 0;
			un->sem_num = sops[i].sem_num;
			un->proc_next = current->semun;
			current->semun = un;
			un->id_next = sma->undo;
			sma->undo = un;
		}
	}
	
 slept:
	if (sma->sem_perm.seq != semid / SEMMNI) 
		return -EIDRM;
	for (i=0; i<nsops; i++) {
		sop = &sops[i];
		curr = &sma->sem_base[sop->sem_num];
		if (sop->sem_op + curr->semval > SEMVMX)
			return -ERANGE;
		if (!sop->sem_op && curr->semval) { 
			if (sop->sem_flg & IPC_NOWAIT)
				return -EAGAIN;
			if (current->signal & ~current->blocked) 
				return -EINTR;
			curr->semzcnt++;
			interruptible_sleep_on (&sma->eventz);
			curr->semzcnt--;
			goto slept;
		}
		if ((sop->sem_op + curr->semval < 0) ) { 
			if (sop->sem_flg & IPC_NOWAIT)
				return -EAGAIN;
			if (current->signal & ~current->blocked)
				return -EINTR;
			curr->semncnt++;
			interruptible_sleep_on (&sma->eventn);
			curr->semncnt--;
			goto slept;
		}
	}
	
	for (i=0; i<nsops; i++) {
		sop = &sops[i];
		curr = &sma->sem_base[sop->sem_num];
		curr->sempid = current->pid;
		if (!(curr->semval += sop->sem_op))
			semzcnt++;
		if (!(sop->sem_flg & SEM_UNDO))
			continue;
		for (un = current->semun; un; un = un->proc_next) 
			if ((un->semid == semid) && 
			    (un->sem_num == sop->sem_num))
				break;
		if (!un) {
			printk ("semop : no undo for op %d\n", i);
			continue;
		}
		un->semadj -= sop->sem_op;
	}
	sma->sem_otime = CURRENT_TIME; 
       	if (semncnt && sma->eventn)
		wake_up(&sma->eventn);
	if (semzcnt && sma->eventz)
		wake_up(&sma->eventz);
	return curr->semval;
}

/*
 * add semadj values to semaphores, free undo structures.
 * undo structures are not freed when semaphore arrays are destroyed
 * so some of them may be out of date.
 */
void sem_exit (void)
{
	struct sem_undo *u, *un = NULL, **up, **unp;
	struct semid_ds *sma;
	struct sem *sem = NULL;
	
	for (up = &current->semun; (u = *up); *up = u->proc_next, kfree(u)) {
		sma = semary[u->semid % SEMMNI];
		if (sma == IPC_UNUSED || sma == IPC_NOID) 
			continue;
		if (sma->sem_perm.seq != u->semid / SEMMNI)
			continue;
		for (unp = &sma->undo; (un = *unp); unp = &un->id_next) {
			if (u == un) 
				goto found;
		}
		printk ("sem_exit undo list error id=%d\n", u->semid);
		break;
found:
		*unp = un->id_next;
		if (!un->semadj)
			continue;
		while (1) {
			if (sma->sem_perm.seq != un->semid / SEMMNI)
				break;
			sem = &sma->sem_base[un->sem_num];
			if (sem->semval + un->semadj >= 0) {
				sem->semval += un->semadj;
				sem->sempid = current->pid;
				sma->sem_otime = CURRENT_TIME;
				if (un->semadj > 0 && sma->eventn)
					wake_up (&sma->eventn);
				if (!sem->semval && sma->eventz)
					wake_up (&sma->eventz);
				break;
			} 
			if (current->signal & ~current->blocked)
				break;
			sem->semncnt++;
			interruptible_sleep_on (&sma->eventn);
			sem->semncnt--;
		}
	}
	current->semun = NULL;
	return;
}
