#ifndef _LINUX_IPC_H
#define _LINUX_IPC_H
#include <linux/types.h>

typedef int key_t; 		/* should go in <types.h> type for IPC key */
#define IPC_PRIVATE ((key_t) 0)  

struct ipc_perm
{
  key_t  key;
  ushort uid;   /* owner euid and egid */
  ushort gid;
  ushort cuid;  /* creator euid and egid */
  ushort cgid;
  ushort mode;  /* access modes see mode flags below */
  ushort seq;   /* sequence number */
};


/* resource get request flags */
#define IPC_CREAT  00001000   /* create if key is nonexistent */
#define IPC_EXCL   00002000   /* fail if key exists */
#define IPC_NOWAIT 00004000   /* return error on wait */


/* 
 * Control commands used with semctl, msgctl and shmctl 
 * see also specific commands in sem.h, msg.h and shm.h
 */
#define IPC_RMID 0     /* remove resource */
#define IPC_SET  1     /* set ipc_perm options */
#define IPC_STAT 2     /* get ipc_perm options */
#define IPC_INFO 3     /* see ipcs */

#ifdef __KERNEL__

/* special shmsegs[id], msgque[id] or semary[id]  values */
#define IPC_UNUSED	((void *) -1)
#define IPC_NOID	((void *) -2)		/* being allocated/destroyed */

/* 
 * These are used to wrap system calls. See ipc/util.c, libipc.c 
 */
struct ipc_kludge {
    struct msgbuf *msgp;
    long msgtyp;
};

#define SEMOP	 	1
#define SEMGET 		2
#define SEMCTL 		3
#define MSGSND 		11
#define MSGRCV 		12
#define MSGGET 		13
#define MSGCTL 		14
#define SHMAT 		21
#define SHMDT 		22
#define SHMGET 		23
#define SHMCTL 		24

#endif /* __KERNEL__ */

#endif /* _LINUX_IPC_H */


