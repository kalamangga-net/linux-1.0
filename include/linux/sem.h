#ifndef _LINUX_SEM_H
#define _LINUX_SEM_H
#include <linux/ipc.h>

/* semop flags */
#define SEM_UNDO        010000  /* undo the operation on exit */

/* semctl Command Definitions. */
#define GETPID  11       /* get sempid */
#define GETVAL  12       /* get semval */
#define GETALL  13       /* get all semval's */
#define GETNCNT 14       /* get semncnt */
#define GETZCNT 15       /* get semzcnt */
#define SETVAL  16       /* set semval */
#define SETALL  17       /* set all semval's */

/* One semid data structure for each set of semaphores in the system. */
struct semid_ds {
  struct ipc_perm sem_perm;       /* permissions .. see ipc.h */
  time_t          sem_otime;      /* last semop time */
  time_t          sem_ctime;      /* last change time */
  struct sem      *sem_base;      /* ptr to first semaphore in array */
  struct wait_queue *eventn;
  struct wait_queue *eventz;
  struct sem_undo  *undo;	  /* undo requests on this array */
  ushort          sem_nsems;      /* no. of semaphores in array */
};


/* One semaphore structure for each semaphore in the system. */
struct sem {
  short   sempid;         /* pid of last operation */
  ushort  semval;         /* current value */
  ushort  semncnt;        /* num procs awaiting increase in semval */
  ushort  semzcnt;        /* num procs awaiting semval = 0 */
};

/* semop system calls takes an array of these.*/
struct sembuf {
  ushort  sem_num;        /* semaphore index in array */
  short   sem_op;         /* semaphore operation */
  short   sem_flg;        /* operation flags */
};

/* arg for semctl system calls. */
union semun {
  int val;               /* value for SETVAL */
  struct semid_ds *buf;  /* buffer for IPC_STAT & IPC_SET */
  ushort *array;         /* array for GETALL & SETALL */
};


struct  seminfo {
    int semmap; 
    int semmni; 
    int semmns; 
    int semmnu; 
    int semmsl; 
    int semopm; 
    int semume; 
    int semusz; 
    int semvmx; 
    int semaem; 
};

#define SEMMNI  128             /* ?  max # of semaphore identifiers */
#define SEMMSL  32              /* <= 512 max num of semaphores per id */
#define SEMMNS  (SEMMNI*SEMMSL) /* ? max # of semaphores in system */
#define SEMOPM  32	        /* ~ 100 max num of ops per semop call */
#define SEMVMX  32767           /* semaphore maximum value */

/* unused */
#define SEMUME  SEMOPM          /* max num of undo entries per process */
#define SEMMNU  SEMMNS          /* num of undo structures system wide */
#define SEMAEM  (SEMVMX >> 1)   /* adjust on exit max value */
#define SEMMAP  SEMMNS          /* # of entries in semaphore map */
#define SEMUSZ  20		/* sizeof struct sem_undo */ 

#ifdef __KERNEL__
/* ipcs ctl cmds */
#define SEM_STAT 18	
#define SEM_INFO 19

/* per process undo requests */
/* this gets linked into the task_struct */
struct sem_undo {
    struct sem_undo *proc_next;
    struct sem_undo *id_next;
    int    semid;
    short  semadj; 		/* semval adjusted by exit */
    ushort sem_num; 		/* semaphore index in array semid */
};      

#endif /* __KERNEL__ */

#endif /* _LINUX_SEM_H */
