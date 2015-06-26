#ifndef _LINUX_MSG_H
#define _LINUX_MSG_H
#include <linux/ipc.h>

/* msgrcv options */
#define MSG_NOERROR     010000  /* no error if message is too big */
#define MSG_EXCEPT      020000  /* recv any msg except of specified type.*/


/* one msg structure for each message */
struct msg {
    struct msg *msg_next;   /* next message on queue */
    long  msg_type;          
    char *msg_spot;         /* message text address */
    short msg_ts;           /* message text size */
};

/* one msqid structure for each queue on the system */
struct msqid_ds {
    struct ipc_perm msg_perm;
    struct msg *msg_first;  /* first message on queue */
    struct msg *msg_last;   /* last message in queue */
    time_t msg_stime;       /* last msgsnd time */
    time_t msg_rtime;       /* last msgrcv time */
    time_t msg_ctime;       /* last change time */
    struct wait_queue *wwait;
    struct wait_queue *rwait;
    ushort msg_cbytes;      /* current number of bytes on queue */
    ushort msg_qnum;        /* number of messages in queue */
    ushort msg_qbytes;      /* max number of bytes on queue */
    ushort msg_lspid;       /* pid of last msgsnd */
    ushort msg_lrpid;       /* last receive pid */
};


/* message buffer for msgsnd and msgrcv calls */
struct msgbuf {
    long mtype;         /* type of message */
    char mtext[1];      /* message text */
};


struct msginfo {
    int msgpool;
    int msgmap; 
    int msgmax; 
    int msgmnb; 
    int msgmni; 
    int msgssz; 
    int msgtql; 
    ushort  msgseg; 
};

#define MSGMNI   128   /* <= 1K */     /* max # of msg queue identifiers */
#define MSGMAX  4056   /* <= 4056 */   /* max size of message (bytes) */
#define MSGMNB 16384   /* ? */        /* default max size of a message queue */

/* unused */
#define MSGPOOL (MSGMNI*MSGMNB/1024)  /* size in kilobytes of message pool */
#define MSGTQL  MSGMNB            /* number of system message headers */
#define MSGMAP  MSGMNB            /* number of entries in message map */
#define MSGSSZ  16                /* message segment size */
#define __MSGSEG ((MSGPOOL*1024)/ MSGSSZ) /* max no. of segments */
#define MSGSEG (__MSGSEG <= 0xffff ? __MSGSEG : 0xffff)

#ifdef __KERNEL__

/* ipcs ctl commands */
#define MSG_STAT 11
#define MSG_INFO 12

#endif /* __KERNEL__ */

#endif /* _LINUX_MSG_H */
