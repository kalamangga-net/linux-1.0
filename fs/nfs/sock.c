/*
 *  linux/fs/nfs/sock.c
 *
 *  Copyright (C) 1992, 1993  Rick Sladkey
 *
 *  low-level nfs remote procedure call interface
 */

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/nfs_fs.h>
#include <linux/errno.h>
#include <linux/socket.h>
#include <linux/fcntl.h>
#include <asm/segment.h>
#include <linux/in.h>
#include <linux/net.h>


extern struct socket *socki_lookup(struct inode *inode);

#define _S(nr) (1<<((nr)-1))

/*
 * We violate some modularity principles here by poking around
 * in some socket internals.  Besides having to call socket
 * functions from kernel-space instead of user space, the socket
 * interface does not lend itself well to being cleanly called
 * without a file descriptor.  Since the nfs calls can run on
 * behalf of any process, the superblock maintains a file pointer
 * to the server socket.
 */

static int do_nfs_rpc_call(struct nfs_server *server, int *start, int *end)
{
	struct file *file;
	struct inode *inode;
	struct socket *sock;
	unsigned short fs;
	int result;
	int xid;
	int len;
	select_table wait_table;
	struct select_table_entry entry;
	int (*select) (struct inode *, struct file *, int, select_table *);
	int init_timeout, max_timeout;
	int timeout;
	int retrans;
	int major_timeout_seen;
	char *server_name;
	int n;
	int addrlen;
	unsigned long old_mask;

	xid = start[0];
	len = ((char *) end) - ((char *) start);
	file = server->file;
	inode = file->f_inode;
	select = file->f_op->select;
	sock = socki_lookup(inode);
	if (!sock) {
		printk("nfs_rpc_call: socki_lookup failed\n");
		return -EBADF;
	}
	init_timeout = server->timeo;
	max_timeout = NFS_MAX_RPC_TIMEOUT*HZ/10;
	retrans = server->retrans;
	major_timeout_seen = 0;
	server_name = server->hostname;
	old_mask = current->blocked;
	current->blocked |= ~(_S(SIGKILL)
#if 0
		| _S(SIGSTOP)
#endif
		| ((server->flags & NFS_MOUNT_INTR)
		? ((current->sigaction[SIGINT - 1].sa_handler == SIG_DFL
			? _S(SIGINT) : 0)
		| (current->sigaction[SIGQUIT - 1].sa_handler == SIG_DFL
			? _S(SIGQUIT) : 0))
		: 0));
	fs = get_fs();
	set_fs(get_ds());
	for (n = 0, timeout = init_timeout; ; n++, timeout <<= 1) {
		result = sock->ops->send(sock, (void *) start, len, 0, 0);
		if (result < 0) {
			printk("nfs_rpc_call: send error = %d\n", result);
			break;
		}
	re_select:
		wait_table.nr = 0;
		wait_table.entry = &entry;
		current->state = TASK_INTERRUPTIBLE;
		if (!select(inode, file, SEL_IN, &wait_table)
		    && !select(inode, file, SEL_IN, NULL)) {
			if (timeout > max_timeout)
				timeout = max_timeout;
			current->timeout = jiffies + timeout;
			schedule();
			remove_wait_queue(entry.wait_address, &entry.wait);
			current->state = TASK_RUNNING;
			if (current->signal & ~current->blocked) {
				current->timeout = 0;
				result = -ERESTARTSYS;
				break;
			}
			if (!current->timeout) {
				if (n < retrans)
					continue;
				if (server->flags & NFS_MOUNT_SOFT) {
					printk("NFS server %s not responding, "
						"timed out\n", server_name);
					result = -EIO;
					break;
				}
				n = 0;
				timeout = init_timeout;
				init_timeout <<= 1;
				if (!major_timeout_seen) {
					printk("NFS server %s not responding, "
						"still trying\n", server_name);
				}
				major_timeout_seen = 1;
				continue;
			}
			else
				current->timeout = 0;
		}
		else if (wait_table.nr)
			remove_wait_queue(entry.wait_address, &entry.wait);
		current->state = TASK_RUNNING;
		addrlen = 0;
		result = sock->ops->recvfrom(sock, (void *) start, PAGE_SIZE, 1, 0,
			NULL, &addrlen);
		if (result < 0) {
			if (result == -EAGAIN) {
#if 0
				printk("nfs_rpc_call: bad select ready\n");
#endif
				goto re_select;
			}
			if (result == -ECONNREFUSED) {
#if 0
				printk("nfs_rpc_call: server playing coy\n");
#endif
				goto re_select;
			}
			if (result != -ERESTARTSYS) {
				printk("nfs_rpc_call: recv error = %d\n",
					-result);
			}
			break;
		}
		if (*start == xid) {
			if (major_timeout_seen)
				printk("NFS server %s OK\n", server_name);
			break;
		}
#if 0
		printk("nfs_rpc_call: XID mismatch\n");
#endif
	}
	current->blocked = old_mask;
	set_fs(fs);
	return result;
}

/*
 * For now we lock out other simulaneous nfs calls for the same filesytem
 * because we are single-threaded and don't want to get mismatched
 * RPC replies.
 */

int nfs_rpc_call(struct nfs_server *server, int *start, int *end)
{
	int result;

	while (server->lock)
		sleep_on(&server->wait);
	server->lock = 1;
	result = do_nfs_rpc_call(server, start, end);
	server->lock = 0;
	wake_up(&server->wait);
	return result;
}

