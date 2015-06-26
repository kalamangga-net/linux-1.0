/*
 *  linux/fs/pipe.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <asm/segment.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/fcntl.h>
#include <linux/termios.h>


/* We don't use the head/tail construction any more. Now we use the start/len*/
/* contruction providing full use of PIPE_BUF (multiple of PAGE_SIZE) */
/* Florian Coosmann (FGC)                                ^ current = 1       */
/* Additionally, we now use locking technique. This prevents race condition  */
/* in case of paging and multiple read/write on the same pipe. (FGC)         */


static int pipe_read(struct inode * inode, struct file * filp, char * buf, int count)
{
	int chars = 0, size = 0, read = 0;
        char *pipebuf;

	if (filp->f_flags & O_NONBLOCK) {
		if (PIPE_LOCK(*inode))
			return -EAGAIN;
		if (PIPE_EMPTY(*inode))
			if (PIPE_WRITERS(*inode))
				return -EAGAIN;
			else
				return 0;
	} else while (PIPE_EMPTY(*inode) || PIPE_LOCK(*inode)) {
		if (PIPE_EMPTY(*inode)) {
			if (!PIPE_WRITERS(*inode))
				return 0;
		}
		if (current->signal & ~current->blocked)
			return -ERESTARTSYS;
		interruptible_sleep_on(&PIPE_WAIT(*inode));
	}
	PIPE_LOCK(*inode)++;
	while (count>0 && (size = PIPE_SIZE(*inode))) {
		chars = PIPE_MAX_RCHUNK(*inode);
		if (chars > count)
			chars = count;
		if (chars > size)
			chars = size;
		read += chars;
                pipebuf = PIPE_BASE(*inode)+PIPE_START(*inode);
		PIPE_START(*inode) += chars;
		PIPE_START(*inode) &= (PIPE_BUF-1);
		PIPE_LEN(*inode) -= chars;
		count -= chars;
		memcpy_tofs(buf, pipebuf, chars );
		buf += chars;
	}
	PIPE_LOCK(*inode)--;
	wake_up_interruptible(&PIPE_WAIT(*inode));
	if (read)
		return read;
	if (PIPE_WRITERS(*inode))
		return -EAGAIN;
	return 0;
}
	
static int pipe_write(struct inode * inode, struct file * filp, char * buf, int count)
{
	int chars = 0, free = 0, written = 0;
	char *pipebuf;

	if (!PIPE_READERS(*inode)) { /* no readers */
		send_sig(SIGPIPE,current,0);
		return -EPIPE;
	}
/* if count <= PIPE_BUF, we have to make it atomic */
	if (count <= PIPE_BUF)
		free = count;
	else
		free = 1; /* can't do it atomically, wait for any free space */
	while (count>0) {
		while ((PIPE_FREE(*inode) < free) || PIPE_LOCK(*inode)) {
			if (!PIPE_READERS(*inode)) { /* no readers */
				send_sig(SIGPIPE,current,0);
				return written? :-EPIPE;
			}
			if (current->signal & ~current->blocked)
				return written? :-ERESTARTSYS;
			if (filp->f_flags & O_NONBLOCK)
				return written? :-EAGAIN;
			interruptible_sleep_on(&PIPE_WAIT(*inode));
		}
		PIPE_LOCK(*inode)++;
		while (count>0 && (free = PIPE_FREE(*inode))) {
			chars = PIPE_MAX_WCHUNK(*inode);
			if (chars > count)
				chars = count;
			if (chars > free)
				chars = free;
                        pipebuf = PIPE_BASE(*inode)+PIPE_END(*inode);
			written += chars;
			PIPE_LEN(*inode) += chars;
			count -= chars;
			memcpy_fromfs(pipebuf, buf, chars );
			buf += chars;
		}
		PIPE_LOCK(*inode)--;
		wake_up_interruptible(&PIPE_WAIT(*inode));
		free = 1;
	}
	return written;
}

static int pipe_lseek(struct inode * inode, struct file * file, off_t offset, int orig)
{
	return -ESPIPE;
}

static int pipe_readdir(struct inode * inode, struct file * file, struct dirent * de, int count)
{
	return -ENOTDIR;
}

static int bad_pipe_rw(struct inode * inode, struct file * filp, char * buf, int count)
{
	return -EBADF;
}

static int pipe_ioctl(struct inode *pino, struct file * filp,
	unsigned int cmd, unsigned long arg)
{
	int error;

	switch (cmd) {
		case FIONREAD:
			error = verify_area(VERIFY_WRITE, (void *) arg,4);
			if (!error)
				put_fs_long(PIPE_SIZE(*pino),(unsigned long *) arg);
			return error;
		default:
			return -EINVAL;
	}
}

static int pipe_select(struct inode * inode, struct file * filp, int sel_type, select_table * wait)
{
	switch (sel_type) {
		case SEL_IN:
			if (!PIPE_EMPTY(*inode) || !PIPE_WRITERS(*inode))
				return 1;
			select_wait(&PIPE_WAIT(*inode), wait);
			return 0;
		case SEL_OUT:
			if (!PIPE_FULL(*inode) || !PIPE_READERS(*inode))
				return 1;
			select_wait(&PIPE_WAIT(*inode), wait);
			return 0;
		case SEL_EX:
			if (!PIPE_READERS(*inode) || !PIPE_WRITERS(*inode))
				return 1;
			select_wait(&inode->i_wait,wait);
			return 0;
	}
	return 0;
}

/*
 * Arggh. Why does SunOS have to have different select() behaviour
 * for pipes and fifos? Hate-Hate-Hate. See difference in SEL_IN..
 */
static int fifo_select(struct inode * inode, struct file * filp, int sel_type, select_table * wait)
{
	switch (sel_type) {
		case SEL_IN:
			if (!PIPE_EMPTY(*inode))
				return 1;
			select_wait(&PIPE_WAIT(*inode), wait);
			return 0;
		case SEL_OUT:
			if (!PIPE_FULL(*inode) || !PIPE_READERS(*inode))
				return 1;
			select_wait(&PIPE_WAIT(*inode), wait);
			return 0;
		case SEL_EX:
			if (!PIPE_READERS(*inode) || !PIPE_WRITERS(*inode))
				return 1;
			select_wait(&inode->i_wait,wait);
			return 0;
	}
	return 0;
}

/*
 * The 'connect_xxx()' functions are needed for named pipes when
 * the open() code hasn't guaranteed a connection (O_NONBLOCK),
 * and we need to act differently until we do get a writer..
 */
static int connect_read(struct inode * inode, struct file * filp, char * buf, int count)
{
	while (!PIPE_SIZE(*inode)) {
		if (PIPE_WRITERS(*inode))
			break;
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		wake_up_interruptible(& PIPE_WAIT(*inode));
		if (current->signal & ~current->blocked)
			return -ERESTARTSYS;
		interruptible_sleep_on(& PIPE_WAIT(*inode));
	}
	filp->f_op = &read_fifo_fops;
	return pipe_read(inode,filp,buf,count);
}

static int connect_select(struct inode * inode, struct file * filp, int sel_type, select_table * wait)
{
	switch (sel_type) {
		case SEL_IN:
			if (!PIPE_EMPTY(*inode)) {
				filp->f_op = &read_fifo_fops;
				return 1;
			}
			select_wait(&PIPE_WAIT(*inode), wait);
			return 0;
		case SEL_OUT:
			if (!PIPE_FULL(*inode))
				return 1;
			select_wait(&PIPE_WAIT(*inode), wait);
			return 0;
		case SEL_EX:
			if (!PIPE_READERS(*inode) || !PIPE_WRITERS(*inode))
				return 1;
			select_wait(&inode->i_wait,wait);
			return 0;
	}
	return 0;
}

/*
 * Ok, these three routines NOW keep track of readers/writers,
 * Linus previously did it with inode->i_count checking.
 */
static void pipe_read_release(struct inode * inode, struct file * filp)
{
	PIPE_READERS(*inode)--;
	wake_up_interruptible(&PIPE_WAIT(*inode));
}

static void pipe_write_release(struct inode * inode, struct file * filp)
{
	PIPE_WRITERS(*inode)--;
	wake_up_interruptible(&PIPE_WAIT(*inode));
}

static void pipe_rdwr_release(struct inode * inode, struct file * filp)
{
	PIPE_READERS(*inode)--;
	PIPE_WRITERS(*inode)--;
	wake_up_interruptible(&PIPE_WAIT(*inode));
}

/*
 * The file_operations structs are not static because they
 * are also used in linux/fs/fifo.c to do operations on fifo's.
 */
struct file_operations connecting_fifo_fops = {
	pipe_lseek,
	connect_read,
	bad_pipe_rw,
	pipe_readdir,
	connect_select,
	pipe_ioctl,
	NULL,		/* no mmap on pipes.. surprise */
	NULL,		/* no special open code */
	pipe_read_release,
	NULL
};

struct file_operations read_fifo_fops = {
	pipe_lseek,
	pipe_read,
	bad_pipe_rw,
	pipe_readdir,
	fifo_select,
	pipe_ioctl,
	NULL,		/* no mmap on pipes.. surprise */
	NULL,		/* no special open code */
	pipe_read_release,
	NULL
};

struct file_operations write_fifo_fops = {
	pipe_lseek,
	bad_pipe_rw,
	pipe_write,
	pipe_readdir,
	fifo_select,
	pipe_ioctl,
	NULL,		/* mmap */
	NULL,		/* no special open code */
	pipe_write_release,
	NULL
};

struct file_operations rdwr_fifo_fops = {
	pipe_lseek,
	pipe_read,
	pipe_write,
	pipe_readdir,
	fifo_select,
	pipe_ioctl,
	NULL,		/* mmap */
	NULL,		/* no special open code */
	pipe_rdwr_release,
	NULL
};

struct file_operations read_pipe_fops = {
	pipe_lseek,
	pipe_read,
	bad_pipe_rw,
	pipe_readdir,
	pipe_select,
	pipe_ioctl,
	NULL,		/* no mmap on pipes.. surprise */
	NULL,		/* no special open code */
	pipe_read_release,
	NULL
};

struct file_operations write_pipe_fops = {
	pipe_lseek,
	bad_pipe_rw,
	pipe_write,
	pipe_readdir,
	pipe_select,
	pipe_ioctl,
	NULL,		/* mmap */
	NULL,		/* no special open code */
	pipe_write_release,
	NULL
};

struct file_operations rdwr_pipe_fops = {
	pipe_lseek,
	pipe_read,
	pipe_write,
	pipe_readdir,
	pipe_select,
	pipe_ioctl,
	NULL,		/* mmap */
	NULL,		/* no special open code */
	pipe_rdwr_release,
	NULL
};

struct inode_operations pipe_inode_operations = {
	&rdwr_pipe_fops,
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

asmlinkage int sys_pipe(unsigned long * fildes)
{
	struct inode * inode;
	struct file * f[2];
	int fd[2];
	int i,j;

	j = verify_area(VERIFY_WRITE,fildes,8);
	if (j)
		return j;
	for(j=0 ; j<2 ; j++)
		if (!(f[j] = get_empty_filp()))
			break;
	if (j==1)
		f[0]->f_count--;
	if (j<2)
		return -ENFILE;
	j=0;
	for(i=0;j<2 && i<NR_OPEN;i++)
		if (!current->filp[i]) {
			current->filp[ fd[j]=i ] = f[j];
			j++;
		}
	if (j==1)
		current->filp[fd[0]]=NULL;
	if (j<2) {
		f[0]->f_count--;
		f[1]->f_count--;
		return -EMFILE;
	}
	if (!(inode=get_pipe_inode())) {
		current->filp[fd[0]] = NULL;
		current->filp[fd[1]] = NULL;
		f[0]->f_count--;
		f[1]->f_count--;
		return -ENFILE;
	}
	f[0]->f_inode = f[1]->f_inode = inode;
	f[0]->f_pos = f[1]->f_pos = 0;
	f[0]->f_flags = O_RDONLY;
	f[0]->f_op = &read_pipe_fops;
	f[0]->f_mode = 1;		/* read */
	f[1]->f_flags = O_WRONLY;
	f[1]->f_op = &write_pipe_fops;
	f[1]->f_mode = 2;		/* write */
	put_fs_long(fd[0],0+fildes);
	put_fs_long(fd[1],1+fildes);
	return 0;
}
