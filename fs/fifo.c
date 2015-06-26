/*
 *  linux/fs/fifo.c
 *
 *  written by Paul H. Hargrove
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fcntl.h>

static int fifo_open(struct inode * inode,struct file * filp)
{
	int retval = 0;
	unsigned long page;

	switch( filp->f_mode ) {

	case 1:
	/*
	 *  O_RDONLY
	 *  POSIX.1 says that O_NONBLOCK means return with the FIFO
	 *  opened, even when there is no process writing the FIFO.
	 */
		filp->f_op = &connecting_fifo_fops;
		if (!PIPE_READERS(*inode)++)
			wake_up_interruptible(&PIPE_WAIT(*inode));
		if (!(filp->f_flags & O_NONBLOCK) && !PIPE_WRITERS(*inode)) {
			PIPE_RD_OPENERS(*inode)++;
			while (!PIPE_WRITERS(*inode)) {
				if (current->signal & ~current->blocked) {
					retval = -ERESTARTSYS;
					break;
				}
				interruptible_sleep_on(&PIPE_WAIT(*inode));
			}
			if (!--PIPE_RD_OPENERS(*inode))
				wake_up_interruptible(&PIPE_WAIT(*inode));
		}
		while (PIPE_WR_OPENERS(*inode))
			interruptible_sleep_on(&PIPE_WAIT(*inode));
		if (PIPE_WRITERS(*inode))
			filp->f_op = &read_fifo_fops;
		if (retval && !--PIPE_READERS(*inode))
			wake_up_interruptible(&PIPE_WAIT(*inode));
		break;
	
	case 2:
	/*
	 *  O_WRONLY
	 *  POSIX.1 says that O_NONBLOCK means return -1 with
	 *  errno=ENXIO when there is no process reading the FIFO.
	 */
		if ((filp->f_flags & O_NONBLOCK) && !PIPE_READERS(*inode)) {
			retval = -ENXIO;
			break;
		}
		filp->f_op = &write_fifo_fops;
		if (!PIPE_WRITERS(*inode)++)
			wake_up_interruptible(&PIPE_WAIT(*inode));
		if (!PIPE_READERS(*inode)) {
			PIPE_WR_OPENERS(*inode)++;
			while (!PIPE_READERS(*inode)) {
				if (current->signal & ~current->blocked) {
					retval = -ERESTARTSYS;
					break;
				}
				interruptible_sleep_on(&PIPE_WAIT(*inode));
			}
			if (!--PIPE_WR_OPENERS(*inode))
				wake_up_interruptible(&PIPE_WAIT(*inode));
		}
		while (PIPE_RD_OPENERS(*inode))
			interruptible_sleep_on(&PIPE_WAIT(*inode));
		if (retval && !--PIPE_WRITERS(*inode))
			wake_up_interruptible(&PIPE_WAIT(*inode));
		break;
	
	case 3:
	/*
	 *  O_RDWR
	 *  POSIX.1 leaves this case "undefined" when O_NONBLOCK is set.
	 *  This implementation will NEVER block on a O_RDWR open, since
	 *  the process can at least talk to itself.
	 */
		filp->f_op = &rdwr_fifo_fops;
		if (!PIPE_READERS(*inode)++)
			wake_up_interruptible(&PIPE_WAIT(*inode));
		while (PIPE_WR_OPENERS(*inode))
			interruptible_sleep_on(&PIPE_WAIT(*inode));
		if (!PIPE_WRITERS(*inode)++)
			wake_up_interruptible(&PIPE_WAIT(*inode));
		while (PIPE_RD_OPENERS(*inode))
			interruptible_sleep_on(&PIPE_WAIT(*inode));
		break;

	default:
		retval = -EINVAL;
	}
	if (retval || PIPE_BASE(*inode))
		return retval;
	page = __get_free_page(GFP_KERNEL);
	if (PIPE_BASE(*inode)) {
		free_page(page);
		return 0;
	}
	if (!page)
		return -ENOMEM;
	PIPE_LOCK(*inode) = 0;
	PIPE_START(*inode) = PIPE_LEN(*inode) = 0;
	PIPE_BASE(*inode) = (char *) page;
	return 0;
}

/*
 * Dummy default file-operations: the only thing this does
 * is contain the open that then fills in the correct operations
 * depending on the access mode of the file...
 */
static struct file_operations def_fifo_fops = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	fifo_open,		/* will set read or write pipe_fops */
	NULL,
	NULL
};

static struct inode_operations fifo_inode_operations = {
	&def_fifo_fops,		/* default file operations */
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

void init_fifo(struct inode * inode)
{
	inode->i_op = &fifo_inode_operations;
	inode->i_pipe = 1;
	PIPE_LOCK(*inode) = 0;
	PIPE_BASE(*inode) = NULL;
	PIPE_START(*inode) = PIPE_LEN(*inode) = 0;
	PIPE_RD_OPENERS(*inode) = PIPE_WR_OPENERS(*inode) = 0;
	PIPE_WAIT(*inode) = NULL;
	PIPE_READERS(*inode) = PIPE_WRITERS(*inode) = 0;
}
