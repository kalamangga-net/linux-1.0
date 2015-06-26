#ifndef _LINUX_FCNTL_H
#define _LINUX_FCNTL_H

/* open/fcntl - O_SYNC isn't implemented yet */
#define O_ACCMODE	  0003
#define O_RDONLY	    00
#define O_WRONLY	    01
#define O_RDWR		    02
#define O_CREAT		  0100	/* not fcntl */
#define O_EXCL		  0200	/* not fcntl */
#define O_NOCTTY	  0400	/* not fcntl */
#define O_TRUNC		 01000	/* not fcntl */
#define O_APPEND	 02000
#define O_NONBLOCK	 04000
#define O_NDELAY	O_NONBLOCK
#define O_SYNC		010000

#define F_DUPFD		0	/* dup */
#define F_GETFD		1	/* get f_flags */
#define F_SETFD		2	/* set f_flags */
#define F_GETFL		3	/* more flags (cloexec) */
#define F_SETFL		4
#define F_GETLK		5
#define F_SETLK		6
#define F_SETLKW	7

#define F_SETOWN	8	/*  for sockets. */
#define F_GETOWN	9	/*  for sockets. */

/* for F_[GET|SET]FL */
#define FD_CLOEXEC	1	/* actually anything with low bit set goes */

#define F_RDLCK		0
#define F_WRLCK		1
#define F_UNLCK		2

/* For bsd flock () */
#define F_EXLCK		4	/* or 3 */
#define F_SHLCK		8	/* or 4 */

struct flock {
	short l_type;
	short l_whence;
	off_t l_start;
	off_t l_len;
	pid_t l_pid;
};

#endif
