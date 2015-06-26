/*
 *  linux/fs/fcntl.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <asm/segment.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/string.h>

extern int fcntl_getlk(unsigned int, struct flock *);
extern int fcntl_setlk(unsigned int, unsigned int, struct flock *);
extern int sock_fcntl (struct file *, unsigned int cmd, unsigned long arg);

static int dupfd(unsigned int fd, unsigned int arg)
{
	if (fd >= NR_OPEN || !current->filp[fd])
		return -EBADF;
	if (arg >= NR_OPEN)
		return -EINVAL;
	while (arg < NR_OPEN)
		if (current->filp[arg])
			arg++;
		else
			break;
	if (arg >= NR_OPEN)
		return -EMFILE;
	FD_CLR(arg, &current->close_on_exec);
	(current->filp[arg] = current->filp[fd])->f_count++;
	return arg;
}

asmlinkage int sys_dup2(unsigned int oldfd, unsigned int newfd)
{
	if (oldfd >= NR_OPEN || !current->filp[oldfd])
		return -EBADF;
	if (newfd == oldfd)
		return newfd;
	/*
	 * errno's for dup2() are slightly different than for fcntl(F_DUPFD)
	 * for historical reasons.
	 */
	if (newfd > NR_OPEN)	/* historical botch - should have been >= */
		return -EBADF;	/* dupfd() would return -EINVAL */
#if 1
	if (newfd == NR_OPEN)
		return -EBADF;	/* dupfd() does return -EINVAL and that may
				 * even be the standard!  But that is too
				 * weird for now.
				 */
#endif
	sys_close(newfd);
	return dupfd(oldfd,newfd);
}

asmlinkage int sys_dup(unsigned int fildes)
{
	return dupfd(fildes,0);
}

asmlinkage int sys_fcntl(unsigned int fd, unsigned int cmd, unsigned long arg)
{	
	struct file * filp;

	if (fd >= NR_OPEN || !(filp = current->filp[fd]))
		return -EBADF;
	switch (cmd) {
		case F_DUPFD:
			return dupfd(fd,arg);
		case F_GETFD:
			return FD_ISSET(fd, &current->close_on_exec);
		case F_SETFD:
			if (arg&1)
				FD_SET(fd, &current->close_on_exec);
			else
				FD_CLR(fd, &current->close_on_exec);
			return 0;
		case F_GETFL:
			return filp->f_flags;
		case F_SETFL:
			filp->f_flags &= ~(O_APPEND | O_NONBLOCK);
			filp->f_flags |= arg & (O_APPEND | O_NONBLOCK);
			return 0;
		case F_GETLK:
			return fcntl_getlk(fd, (struct flock *) arg);
		case F_SETLK:
			return fcntl_setlk(fd, cmd, (struct flock *) arg);
		case F_SETLKW:
			return fcntl_setlk(fd, cmd, (struct flock *) arg);
		default:
			/* sockets need a few special fcntls. */
			if (S_ISSOCK (filp->f_inode->i_mode))
			  {
			     return (sock_fcntl (filp, cmd, arg));
			  }
			return -EINVAL;
	}
}
