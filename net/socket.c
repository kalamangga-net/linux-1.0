/*
 * NET		An implementation of the SOCKET network access protocol.
 *
 * Version:	@(#)socket.c	1.0.5	05/25/93
 *
 * Authors:	Orest Zborowski, <obz@Kodak.COM>
 *		Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 * Fixes:
 *		Anonymous	:	NOTSOCK/BADF cleanup. Error fix in
 *					shutdown()
 *		Alan Cox	:	verify_area() fixes
 *
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */

#include <linux/config.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/stat.h>
#include <linux/socket.h>
#include <linux/fcntl.h>
#include <linux/net.h>
#include <linux/ddi.h>

#include <asm/system.h>
#include <asm/segment.h>

#undef SOCK_DEBUG

#ifdef SOCK_DEBUG
#include <stdarg.h>
#define DPRINTF(x) dprintf x
#else
#define DPRINTF(x) /**/
#endif

static int sock_lseek(struct inode *inode, struct file *file, off_t offset,
		      int whence);
static int sock_read(struct inode *inode, struct file *file, char *buf,
		     int size);
static int sock_write(struct inode *inode, struct file *file, char *buf,
		      int size);
static int sock_readdir(struct inode *inode, struct file *file,
			struct dirent *dirent, int count);
static void sock_close(struct inode *inode, struct file *file);
static int sock_select(struct inode *inode, struct file *file, int which, select_table *seltable);
static int sock_ioctl(struct inode *inode, struct file *file,
		      unsigned int cmd, unsigned long arg);


static struct file_operations socket_file_ops = {
  sock_lseek,
  sock_read,
  sock_write,
  sock_readdir,
  sock_select,
  sock_ioctl,
  NULL,			/* mmap */
  NULL,			/* no special open code... */
  sock_close
};

static struct socket sockets[NSOCKETS];
static struct wait_queue *socket_wait_free = NULL;
static struct proto_ops *pops[NPROTO];
static int net_debug = 0;

#define last_socket	(sockets + NSOCKETS - 1)

#ifdef SOCK_DEBUG
/* Module debugging. */
static void
dprintf(int level, char *fmt, ...)
{
  char buff[1024];
  va_list args;
  extern int vsprintf(char * buf, const char * fmt, va_list args);

  if (level == 0) return;
  va_start(args, fmt);
  vsprintf(buff, fmt, args);
  va_end(args);
  printk(buff);
}
#endif

/* Obtains the first available file descriptor and sets it up for use. */
static int
get_fd(struct inode *inode)
{
  int fd;
  struct file *file;

  /* Find a file descriptor suitable for return to the user. */
  file = get_empty_filp();
  if (!file) return(-1);
  for (fd = 0; fd < NR_OPEN; ++fd)
	if (!current->filp[fd]) break;
  if (fd == NR_OPEN) {
	file->f_count = 0;
	return(-1);
  }
  FD_CLR(fd, &current->close_on_exec);
  current->filp[fd] = file;
  file->f_op = &socket_file_ops;
  file->f_mode = 3;
  file->f_flags = 0;
  file->f_count = 1;
  file->f_inode = inode;
  if (inode) inode->i_count++;
  file->f_pos = 0;
  return(fd);
}


/*
 * Reverses the action of get_fd() by releasing the file. it closes
 * the descriptor, but makes sure it does nothing more. Called when
 * an incomplete socket must be closed, along with sock_release().
 */
static inline void
toss_fd(int fd)
{
  sys_close(fd);		/* the count protects us from iput */
}


struct socket *
socki_lookup(struct inode *inode)
{
  struct socket *sock;

  if ((sock = inode->i_socket) != NULL) {
	if (sock->state != SS_FREE && SOCK_INODE(sock) == inode)
		return sock;
	printk("socket.c: uhhuh. stale inode->i_socket pointer\n");
  }
  for (sock = sockets; sock <= last_socket; ++sock)
	if (sock->state != SS_FREE && SOCK_INODE(sock) == inode) {
		printk("socket.c: uhhuh. Found socket despite no inode->i_socket pointer\n");
		return(sock);
	}
  return(NULL);
}


static inline struct socket *
sockfd_lookup(int fd, struct file **pfile)
{
  struct file *file;

  if (fd < 0 || fd >= NR_OPEN || !(file = current->filp[fd])) return(NULL);
  if (pfile) *pfile = file;
  return(socki_lookup(file->f_inode));
}


static struct socket *
sock_alloc(int wait)
{
  struct socket *sock;

  while (1) {
	cli();
	for (sock = sockets; sock <= last_socket; ++sock) {
		if (sock->state == SS_FREE) {
			sock->state = SS_UNCONNECTED;
			sti();
			sock->flags = 0;
			sock->ops = NULL;
			sock->data = NULL;
			sock->conn = NULL;
			sock->iconn = NULL;

			/*
			 * This really shouldn't be necessary, but everything
			 * else depends on inodes, so we grab it.
			 * Sleeps are also done on the i_wait member of this
			 * inode.  The close system call will iput this inode
			 * for us.
			 */
			if (!(SOCK_INODE(sock) = get_empty_inode())) {
				printk("NET: sock_alloc: no more inodes\n");
				sock->state = SS_FREE;
				return(NULL);
			}
			SOCK_INODE(sock)->i_mode = S_IFSOCK;
			SOCK_INODE(sock)->i_uid = current->euid;
			SOCK_INODE(sock)->i_gid = current->egid;
			SOCK_INODE(sock)->i_socket = sock;

			sock->wait = &SOCK_INODE(sock)->i_wait;
			DPRINTF((net_debug,
				"NET: sock_alloc: sk 0x%x, ino 0x%x\n",
				       			sock, SOCK_INODE(sock)));
			return(sock);
		}
	}
	sti();
	if (!wait) return(NULL);
	DPRINTF((net_debug, "NET: sock_alloc: no free sockets, sleeping...\n"));
	interruptible_sleep_on(&socket_wait_free);
	if (current->signal & ~current->blocked) {
		DPRINTF((net_debug, "NET: sock_alloc: sleep was interrupted\n"));
		return(NULL);
	}
	DPRINTF((net_debug, "NET: sock_alloc: wakeup... trying again...\n"));
  }
}


static inline void
sock_release_peer(struct socket *peer)
{
  peer->state = SS_DISCONNECTING;
  wake_up_interruptible(peer->wait);
}


static void
sock_release(struct socket *sock)
{
  int oldstate;
  struct inode *inode;
  struct socket *peersock, *nextsock;

  DPRINTF((net_debug, "NET: sock_release: socket 0x%x, inode 0x%x\n",
						sock, SOCK_INODE(sock)));
  if ((oldstate = sock->state) != SS_UNCONNECTED)
			sock->state = SS_DISCONNECTING;

  /* Wake up anyone waiting for connections. */
  for (peersock = sock->iconn; peersock; peersock = nextsock) {
	nextsock = peersock->next;
	sock_release_peer(peersock);
  }

  /*
   * Wake up anyone we're connected to. First, we release the
   * protocol, to give it a chance to flush data, etc.
   */
  peersock = (oldstate == SS_CONNECTED) ? sock->conn : NULL;
  if (sock->ops) sock->ops->release(sock, peersock);
  if (peersock) sock_release_peer(peersock);
  inode = SOCK_INODE(sock);
  sock->state = SS_FREE;		/* this really releases us */
  wake_up_interruptible(&socket_wait_free);

  /* We need to do this. If sock alloc was called we already have an inode. */
  iput(inode);
}


static int
sock_lseek(struct inode *inode, struct file *file, off_t offset, int whence)
{
  DPRINTF((net_debug, "NET: sock_lseek: huh?\n"));
  return(-ESPIPE);
}


static int
sock_read(struct inode *inode, struct file *file, char *ubuf, int size)
{
  struct socket *sock;

  DPRINTF((net_debug, "NET: sock_read: buf=0x%x, size=%d\n", ubuf, size));
  if (!(sock = socki_lookup(inode))) {
	printk("NET: sock_read: can't find socket for inode!\n");
	return(-EBADF);
  }
  if (sock->flags & SO_ACCEPTCON) return(-EINVAL);
  return(sock->ops->read(sock, ubuf, size, (file->f_flags & O_NONBLOCK)));
}


static int
sock_write(struct inode *inode, struct file *file, char *ubuf, int size)
{
  struct socket *sock;

  DPRINTF((net_debug, "NET: sock_write: buf=0x%x, size=%d\n", ubuf, size));
  if (!(sock = socki_lookup(inode))) {
	printk("NET: sock_write: can't find socket for inode!\n");
	return(-EBADF);
  }
  if (sock->flags & SO_ACCEPTCON) return(-EINVAL);
  return(sock->ops->write(sock, ubuf, size,(file->f_flags & O_NONBLOCK)));
}


static int
sock_readdir(struct inode *inode, struct file *file, struct dirent *dirent,
	     int count)
{
  DPRINTF((net_debug, "NET: sock_readdir: huh?\n"));
  return(-EBADF);
}


int
sock_ioctl(struct inode *inode, struct file *file, unsigned int cmd,
	   unsigned long arg)
{
  struct socket *sock;

  DPRINTF((net_debug, "NET: sock_ioctl: inode=0x%x cmd=0x%x arg=%d\n",
							inode, cmd, arg));
  if (!(sock = socki_lookup(inode))) {
	printk("NET: sock_ioctl: can't find socket for inode!\n");
	return(-EBADF);
  }
  return(sock->ops->ioctl(sock, cmd, arg));
}


static int
sock_select(struct inode *inode, struct file *file, int sel_type, select_table * wait)
{
  struct socket *sock;

  DPRINTF((net_debug, "NET: sock_select: inode = 0x%x, kind = %s\n", inode,
       (sel_type == SEL_IN) ? "in" :
       (sel_type == SEL_OUT) ? "out" : "ex"));
  if (!(sock = socki_lookup(inode))) {
	printk("NET: sock_select: can't find socket for inode!\n");
	return(0);
  }

  /* We can't return errors to select, so its either yes or no. */
  if (sock->ops && sock->ops->select)
	return(sock->ops->select(sock, sel_type, wait));
  return(0);
}


void
sock_close(struct inode *inode, struct file *file)
{
  struct socket *sock;

  DPRINTF((net_debug, "NET: sock_close: inode=0x%x (cnt=%d)\n",
						inode, inode->i_count));

  /* It's possible the inode is NULL if we're closing an unfinished socket. */
  if (!inode) return;
  if (!(sock = socki_lookup(inode))) {
	printk("NET: sock_close: can't find socket for inode!\n");
	return;
  }
  sock_release(sock);
}


int
sock_awaitconn(struct socket *mysock, struct socket *servsock)
{
  struct socket *last;

  DPRINTF((net_debug,
	"NET: sock_awaitconn: trying to connect socket 0x%x to 0x%x\n",
							mysock, servsock));
  if (!(servsock->flags & SO_ACCEPTCON)) {
	DPRINTF((net_debug,
		"NET: sock_awaitconn: server not accepting connections\n"));
	return(-EINVAL);
  }

  /* Put ourselves on the server's incomplete connection queue. */
  mysock->next = NULL;
  cli();
  if (!(last = servsock->iconn)) servsock->iconn = mysock;
    else {
	while (last->next) last = last->next;
	last->next = mysock;
  }
  mysock->state = SS_CONNECTING;
  mysock->conn = servsock;
  sti();

  /*
   * Wake up server, then await connection. server will set state to
   * SS_CONNECTED if we're connected.
   */
  wake_up_interruptible(servsock->wait);
  if (mysock->state != SS_CONNECTED) {
	interruptible_sleep_on(mysock->wait);
	if (mysock->state != SS_CONNECTED &&
	    mysock->state != SS_DISCONNECTING) {
		/*
		 * if we're not connected we could have been
		 * 1) interrupted, so we need to remove ourselves
		 *    from the server list
		 * 2) rejected (mysock->conn == NULL), and have
		 *    already been removed from the list
		 */
		if (mysock->conn == servsock) {
			cli();
			if ((last = servsock->iconn) == mysock)
					servsock->iconn = mysock->next;
			else {
				while (last->next != mysock) last = last->next;
				last->next = mysock->next;
			}
			sti();
		}
		return(mysock->conn ? -EINTR : -EACCES);
	}
  }
  return(0);
}


/*
 * Perform the socket system call. we locate the appropriate
 * family, then create a fresh socket.
 */
static int
sock_socket(int family, int type, int protocol)
{
  int i, fd;
  struct socket *sock;
  struct proto_ops *ops;

  DPRINTF((net_debug,
	"NET: sock_socket: family = %d, type = %d, protocol = %d\n",
						family, type, protocol));

  /* Locate the correct protocol family. */
  for (i = 0; i < NPROTO; ++i) {
	if (pops[i] == NULL) continue;
	if (pops[i]->family == family) break;
  }
  if (i == NPROTO) {
	DPRINTF((net_debug, "NET: sock_socket: family not found\n"));
	return(-EINVAL);
  }
  ops = pops[i];

  /*
   * Check that this is a type that we know how to manipulate and
   * the protocol makes sense here. The family can still reject the
   * protocol later.
   */
  if ((type != SOCK_STREAM && type != SOCK_DGRAM &&
       type != SOCK_SEQPACKET && type != SOCK_RAW &&
       type != SOCK_PACKET) || protocol < 0)
							return(-EINVAL);

  /*
   * allocate the socket and allow the family to set things up. if
   * the protocol is 0, the family is instructed to select an appropriate
   * default.
   */
  if (!(sock = sock_alloc(1))) {
	printk("sock_socket: no more sockets\n");
	return(-EAGAIN);
  }
  sock->type = type;
  sock->ops = ops;
  if ((i = sock->ops->create(sock, protocol)) < 0) {
	sock_release(sock);
	return(i);
  }

  if ((fd = get_fd(SOCK_INODE(sock))) < 0) {
	sock_release(sock);
	return(-EINVAL);
  }

  return(fd);
}


static int
sock_socketpair(int family, int type, int protocol, unsigned long usockvec[2])
{
  int fd1, fd2, i;
  struct socket *sock1, *sock2;
  int er;

  DPRINTF((net_debug,
	"NET: sock_socketpair: family = %d, type = %d, protocol = %d\n",
							family, type, protocol));

  /*
   * Obtain the first socket and check if the underlying protocol
   * supports the socketpair call.
   */
  if ((fd1 = sock_socket(family, type, protocol)) < 0) return(fd1);
  sock1 = sockfd_lookup(fd1, NULL);
  if (!sock1->ops->socketpair) {
	sys_close(fd1);
	return(-EINVAL);
  }

  /* Now grab another socket and try to connect the two together. */
  if ((fd2 = sock_socket(family, type, protocol)) < 0) {
	sys_close(fd1);
	return(-EINVAL);
  }
  sock2 = sockfd_lookup(fd2, NULL);
  if ((i = sock1->ops->socketpair(sock1, sock2)) < 0) {
	sys_close(fd1);
	sys_close(fd2);
	return(i);
  }
  sock1->conn = sock2;
  sock2->conn = sock1;
  sock1->state = SS_CONNECTED;
  sock2->state = SS_CONNECTED;

  er=verify_area(VERIFY_WRITE, usockvec, 2 * sizeof(int));
  if(er)
  	return er;
  put_fs_long(fd1, &usockvec[0]);
  put_fs_long(fd2, &usockvec[1]);

  return(0);
}


/*
 * Bind a name to a socket. Nothing much to do here since its
 * the protocol's responsibility to handle the local address.
 */
static int
sock_bind(int fd, struct sockaddr *umyaddr, int addrlen)
{
  struct socket *sock;
  int i;

  DPRINTF((net_debug, "NET: sock_bind: fd = %d\n", fd));
  if (fd < 0 || fd >= NR_OPEN || current->filp[fd] == NULL)
								return(-EBADF);
  if (!(sock = sockfd_lookup(fd, NULL))) return(-ENOTSOCK);
  if ((i = sock->ops->bind(sock, umyaddr, addrlen)) < 0) {
	DPRINTF((net_debug, "NET: sock_bind: bind failed\n"));
	return(i);
  }
  return(0);
}


/*
 * Perform a listen. Basically, we allow the protocol to do anything
 * necessary for a listen, and if that works, we mark the socket as
 * ready for listening.
 */
static int
sock_listen(int fd, int backlog)
{
  struct socket *sock;

  DPRINTF((net_debug, "NET: sock_listen: fd = %d\n", fd));
  if (fd < 0 || fd >= NR_OPEN || current->filp[fd] == NULL)
								return(-EBADF);
  if (!(sock = sockfd_lookup(fd, NULL))) return(-ENOTSOCK);
  if (sock->state != SS_UNCONNECTED) {
	DPRINTF((net_debug, "NET: sock_listen: socket isn't unconnected\n"));
	return(-EINVAL);
  }
  if (sock->ops && sock->ops->listen) sock->ops->listen(sock, backlog);
  sock->flags |= SO_ACCEPTCON;
  return(0);
}


/*
 * For accept, we attempt to create a new socket, set up the link
 * with the client, wake up the client, then return the new
 * connected fd.
 */
static int
sock_accept(int fd, struct sockaddr *upeer_sockaddr, int *upeer_addrlen)
{
  struct file *file;
  struct socket *sock, *newsock;
  int i;

  DPRINTF((net_debug, "NET: sock_accept: fd = %d\n", fd));
  if (fd < 0 || fd >= NR_OPEN || ((file = current->filp[fd]) == NULL))
								return(-EBADF);
  
  if (!(sock = sockfd_lookup(fd, &file))) return(-ENOTSOCK);
  if (sock->state != SS_UNCONNECTED) {
	DPRINTF((net_debug, "NET: sock_accept: socket isn't unconnected\n"));
	return(-EINVAL);
  }
  if (!(sock->flags & SO_ACCEPTCON)) {
	DPRINTF((net_debug,
		"NET: sock_accept: socket not accepting connections!\n"));
	return(-EINVAL);
  }

  if (!(newsock = sock_alloc(0))) {
	printk("NET: sock_accept: no more sockets\n");
	return(-EAGAIN);
  }
  newsock->type = sock->type;
  newsock->ops = sock->ops;
  if ((i = sock->ops->dup(newsock, sock)) < 0) {
	sock_release(newsock);
	return(i);
  }

  i = newsock->ops->accept(sock, newsock, file->f_flags);
  if ( i < 0) {
	sock_release(newsock);
	return(i);
  }

  if ((fd = get_fd(SOCK_INODE(newsock))) < 0) {
	sock_release(newsock);
	return(-EINVAL);
  }

  DPRINTF((net_debug, "NET: sock_accept: connected socket 0x%x via 0x%x\n",
							sock, newsock));

  if (upeer_sockaddr)
	newsock->ops->getname(newsock, upeer_sockaddr, upeer_addrlen, 1);

  return(fd);
}


/* Attempt to connect to a socket with the server address. */
static int
sock_connect(int fd, struct sockaddr *uservaddr, int addrlen)
{
  struct socket *sock;
  struct file *file;
  int i;

  DPRINTF((net_debug, "NET: sock_connect: fd = %d\n", fd));
  if (fd < 0 || fd >= NR_OPEN || (file=current->filp[fd]) == NULL)
								return(-EBADF);
  
  if (!(sock = sockfd_lookup(fd, &file))) return(-ENOTSOCK);
  switch(sock->state) {
	case SS_UNCONNECTED:
		/* This is ok... continue with connect */
		break;
	case SS_CONNECTED:
		/* Socket is already connected */
		return -EISCONN;
	case SS_CONNECTING:
		/* Not yet connected... we will check this. */
		return(sock->ops->connect(sock, uservaddr,
					  addrlen, file->f_flags));
	default:
		DPRINTF((net_debug,
			"NET: sock_connect: socket not unconnected\n"));
		return(-EINVAL);
  }
  i = sock->ops->connect(sock, uservaddr, addrlen, file->f_flags);
  if (i < 0) {
	DPRINTF((net_debug, "NET: sock_connect: connect failed\n"));
	return(i);
  }
  return(0);
}


static int
sock_getsockname(int fd, struct sockaddr *usockaddr, int *usockaddr_len)
{
  struct socket *sock;

  DPRINTF((net_debug, "NET: sock_getsockname: fd = %d\n", fd));
  if (fd < 0 || fd >= NR_OPEN || current->filp[fd] == NULL)
								return(-EBADF);
  if (!(sock = sockfd_lookup(fd, NULL))) return(-ENOTSOCK);
  return(sock->ops->getname(sock, usockaddr, usockaddr_len, 0));
}


static int
sock_getpeername(int fd, struct sockaddr *usockaddr, int *usockaddr_len)
{
  struct socket *sock;

  DPRINTF((net_debug, "NET: sock_getpeername: fd = %d\n", fd));
  if (fd < 0 || fd >= NR_OPEN || current->filp[fd] == NULL)
			return(-EBADF);
  if (!(sock = sockfd_lookup(fd, NULL))) return(-ENOTSOCK);
  return(sock->ops->getname(sock, usockaddr, usockaddr_len, 1));
}


static int
sock_send(int fd, void * buff, int len, unsigned flags)
{
  struct socket *sock;
  struct file *file;

  DPRINTF((net_debug,
	"NET: sock_send(fd = %d, buff = %X, len = %d, flags = %X)\n",
       							fd, buff, len, flags));

  if (fd < 0 || fd >= NR_OPEN || ((file = current->filp[fd]) == NULL))
								return(-EBADF);
  if (!(sock = sockfd_lookup(fd, NULL))) return(-ENOTSOCK);

  return(sock->ops->send(sock, buff, len, (file->f_flags & O_NONBLOCK), flags));
}


static int
sock_sendto(int fd, void * buff, int len, unsigned flags,
	   struct sockaddr *addr, int addr_len)
{
  struct socket *sock;
  struct file *file;

  DPRINTF((net_debug,
	"NET: sock_sendto(fd = %d, buff = %X, len = %d, flags = %X,"
	 " addr=%X, alen = %d\n", fd, buff, len, flags, addr, addr_len));

  if (fd < 0 || fd >= NR_OPEN || ((file = current->filp[fd]) == NULL))
								return(-EBADF);
  if (!(sock = sockfd_lookup(fd, NULL))) return(-ENOTSOCK);

  return(sock->ops->sendto(sock, buff, len, (file->f_flags & O_NONBLOCK),
			   flags, addr, addr_len));
}


static int
sock_recv(int fd, void * buff, int len, unsigned flags)
{
  struct socket *sock;
  struct file *file;

  DPRINTF((net_debug,
	"NET: sock_recv(fd = %d, buff = %X, len = %d, flags = %X)\n",
							fd, buff, len, flags));

  if (fd < 0 || fd >= NR_OPEN || ((file = current->filp[fd]) == NULL))
								return(-EBADF);
  if (!(sock = sockfd_lookup(fd, NULL))) return(-ENOTSOCK);

  return(sock->ops->recv(sock, buff, len,(file->f_flags & O_NONBLOCK), flags));
}


static int
sock_recvfrom(int fd, void * buff, int len, unsigned flags,
	     struct sockaddr *addr, int *addr_len)
{
  struct socket *sock;
  struct file *file;

  DPRINTF((net_debug,
	"NET: sock_recvfrom(fd = %d, buff = %X, len = %d, flags = %X,"
	" addr=%X, alen=%X\n", fd, buff, len, flags, addr, addr_len));

  if (fd < 0 || fd >= NR_OPEN || ((file = current->filp[fd]) == NULL))
								return(-EBADF);
  if (!(sock = sockfd_lookup(fd, NULL))) return(-ENOTSOCK);

  return(sock->ops->recvfrom(sock, buff, len, (file->f_flags & O_NONBLOCK),
			     flags, addr, addr_len));
}


static int
sock_setsockopt(int fd, int level, int optname, char *optval, int optlen)
{
  struct socket *sock;
  struct file *file;
	
  DPRINTF((net_debug, "NET: sock_setsockopt(fd=%d, level=%d, optname=%d,\n",
							fd, level, optname));
  DPRINTF((net_debug, "                     optval = %X, optlen = %d)\n",
							optval, optlen));

  if (fd < 0 || fd >= NR_OPEN || ((file = current->filp[fd]) == NULL))
								return(-EBADF);
  if (!(sock = sockfd_lookup(fd, NULL))) return(-ENOTSOCK);

  return(sock->ops->setsockopt(sock, level, optname, optval, optlen));
}


static int
sock_getsockopt(int fd, int level, int optname, char *optval, int *optlen)
{
  struct socket *sock;
  struct file *file;

  DPRINTF((net_debug, "NET: sock_getsockopt(fd=%d, level=%d, optname=%d,\n",
						fd, level, optname));
  DPRINTF((net_debug, "                     optval = %X, optlen = %X)\n",
						optval, optlen));

  if (fd < 0 || fd >= NR_OPEN || ((file = current->filp[fd]) == NULL))
								return(-EBADF);
  if (!(sock = sockfd_lookup(fd, NULL))) return(-ENOTSOCK);
	    
  if (!sock->ops || !sock->ops->getsockopt) return(0);
  return(sock->ops->getsockopt(sock, level, optname, optval, optlen));
}


static int
sock_shutdown(int fd, int how)
{
  struct socket *sock;
  struct file *file;

  DPRINTF((net_debug, "NET: sock_shutdown(fd = %d, how = %d)\n", fd, how));

  if (fd < 0 || fd >= NR_OPEN || ((file = current->filp[fd]) == NULL))
								return(-EBADF);

  if (!(sock = sockfd_lookup(fd, NULL))) return(-ENOTSOCK);

  return(sock->ops->shutdown(sock, how));
}


int
sock_fcntl(struct file *filp, unsigned int cmd, unsigned long arg)
{
  struct socket *sock;

  sock = socki_lookup (filp->f_inode);
  if (sock != NULL && sock->ops != NULL && sock->ops->fcntl != NULL)
				return(sock->ops->fcntl(sock, cmd, arg));
  return(-EINVAL);
}


/*
 * System call vectors. Since I (RIB) want to rewrite sockets as streams,
 * we have this level of indirection. Not a lot of overhead, since more of
 * the work is done via read/write/select directly.
 */
asmlinkage int
sys_socketcall(int call, unsigned long *args)
{
  int er;
  switch(call) {
	case SYS_SOCKET:
		er=verify_area(VERIFY_READ, args, 3 * sizeof(long));
		if(er)
			return er;
		return(sock_socket(get_fs_long(args+0),
				   get_fs_long(args+1),
				   get_fs_long(args+2)));
	case SYS_BIND:
		er=verify_area(VERIFY_READ, args, 3 * sizeof(long));
		if(er)
			return er;
		return(sock_bind(get_fs_long(args+0),
				 (struct sockaddr *)get_fs_long(args+1),
				 get_fs_long(args+2)));
	case SYS_CONNECT:
		er=verify_area(VERIFY_READ, args, 3 * sizeof(long));
		if(er)
			return er;
		return(sock_connect(get_fs_long(args+0),
				    (struct sockaddr *)get_fs_long(args+1),
				    get_fs_long(args+2)));
	case SYS_LISTEN:
		er=verify_area(VERIFY_READ, args, 2 * sizeof(long));
		if(er)
			return er;
		return(sock_listen(get_fs_long(args+0),
				   get_fs_long(args+1)));
	case SYS_ACCEPT:
		er=verify_area(VERIFY_READ, args, 3 * sizeof(long));
		if(er)
			return er;
		return(sock_accept(get_fs_long(args+0),
				   (struct sockaddr *)get_fs_long(args+1),
				   (int *)get_fs_long(args+2)));
	case SYS_GETSOCKNAME:
		er=verify_area(VERIFY_READ, args, 3 * sizeof(long));
		if(er)
			return er;
		return(sock_getsockname(get_fs_long(args+0),
					(struct sockaddr *)get_fs_long(args+1),
					(int *)get_fs_long(args+2)));
	case SYS_GETPEERNAME:
		er=verify_area(VERIFY_READ, args, 3 * sizeof(long));
		if(er)
			return er;
		return(sock_getpeername(get_fs_long(args+0),
					(struct sockaddr *)get_fs_long(args+1),
					(int *)get_fs_long(args+2)));
	case SYS_SOCKETPAIR:
		er=verify_area(VERIFY_READ, args, 4 * sizeof(long));
		if(er)
			return er;
		return(sock_socketpair(get_fs_long(args+0),
				       get_fs_long(args+1),
				       get_fs_long(args+2),
				       (unsigned long *)get_fs_long(args+3)));
	case SYS_SEND:
		er=verify_area(VERIFY_READ, args, 4 * sizeof(unsigned long));
		if(er)
			return er;
		return(sock_send(get_fs_long(args+0),
				 (void *)get_fs_long(args+1),
				 get_fs_long(args+2),
				 get_fs_long(args+3)));
	case SYS_SENDTO:
		er=verify_area(VERIFY_READ, args, 6 * sizeof(unsigned long));
		if(er)
			return er;
		return(sock_sendto(get_fs_long(args+0),
				   (void *)get_fs_long(args+1),
				   get_fs_long(args+2),
				   get_fs_long(args+3),
				   (struct sockaddr *)get_fs_long(args+4),
				   get_fs_long(args+5)));
	case SYS_RECV:
		er=verify_area(VERIFY_READ, args, 4 * sizeof(unsigned long));
		if(er)
			return er;
		return(sock_recv(get_fs_long(args+0),
				 (void *)get_fs_long(args+1),
				 get_fs_long(args+2),
				 get_fs_long(args+3)));
	case SYS_RECVFROM:
		er=verify_area(VERIFY_READ, args, 6 * sizeof(unsigned long));
		if(er)
			return er;
		return(sock_recvfrom(get_fs_long(args+0),
				     (void *)get_fs_long(args+1),
				     get_fs_long(args+2),
				     get_fs_long(args+3),
				     (struct sockaddr *)get_fs_long(args+4),
				     (int *)get_fs_long(args+5)));
	case SYS_SHUTDOWN:
		er=verify_area(VERIFY_READ, args, 2* sizeof(unsigned long));
		if(er)
			return er;
		return(sock_shutdown(get_fs_long(args+0),
				     get_fs_long(args+1)));
	case SYS_SETSOCKOPT:
		er=verify_area(VERIFY_READ, args, 5*sizeof(unsigned long));
		if(er)
			return er;
		return(sock_setsockopt(get_fs_long(args+0),
				       get_fs_long(args+1),
				       get_fs_long(args+2),
				       (char *)get_fs_long(args+3),
				       get_fs_long(args+4)));
	case SYS_GETSOCKOPT:
		er=verify_area(VERIFY_READ, args, 5*sizeof(unsigned long));
		if(er)
			return er;
		return(sock_getsockopt(get_fs_long(args+0),
				       get_fs_long(args+1),
				       get_fs_long(args+2),
				       (char *)get_fs_long(args+3),
				       (int *)get_fs_long(args+4)));
	default:
		return(-EINVAL);
  }
}


static int
net_ioctl(unsigned int cmd, unsigned long arg)
{
  int er;
  switch(cmd) {
	case DDIOCSDBG:
		er=verify_area(VERIFY_READ, (void *)arg, sizeof(long));
		if(er)
			return er;
		net_debug = get_fs_long((long *)arg);
		if (net_debug != 0 && net_debug != 1) {
			net_debug = 0;
			return(-EINVAL);
		}
		return(0);
	default:
		return(-EINVAL);
  }
  /*NOTREACHED*/
  return(0);
}


/*
 * Handle the IOCTL system call for the NET devices.  This basically
 * means I/O control for the SOCKET layer (future expansions could be
 * a variable number of socket table entries, et al), and for the more
 * general protocols like ARP.  The latter currently lives in the INET
 * module, so we have to get ugly a tiny little bit.  Later... -FvK
 */
static int
net_fioctl(struct inode *inode, struct file *file,
	   unsigned int cmd, unsigned long arg)
{
  extern int arp_ioctl(unsigned int, void *);

  /* Dispatch on the minor device. */
  switch(MINOR(inode->i_rdev)) {
	case 0:		/* NET (SOCKET) */
		DPRINTF((net_debug, "NET: SOCKET level I/O control request.\n"));
		return(net_ioctl(cmd, arg));
#ifdef CONFIG_INET
	case 1:		/* ARP */
		DPRINTF((net_debug, "NET: ARP level I/O control request.\n"));
		return(arp_ioctl(cmd, (void *) arg));
#endif
	default:
		return(-ENODEV);
  }
  /*NOTREACHED*/
  return(-EINVAL);
}


static struct file_operations net_fops = {
  NULL,		/* LSEEK	*/
  NULL,		/* READ		*/
  NULL,		/* WRITE	*/
  NULL,		/* READDIR	*/
  NULL,		/* SELECT	*/
  net_fioctl,	/* IOCTL	*/
  NULL,		/* MMAP		*/
  NULL,		/* OPEN		*/
  NULL		/* CLOSE	*/
};


/*
 * This function is called by a protocol handler that wants to
 * advertise its address family, and have it linked into the
 * SOCKET module.
 */
int
sock_register(int family, struct proto_ops *ops)
{
  int i;

  cli();
  for(i = 0; i < NPROTO; i++) {
	if (pops[i] != NULL) continue;
	pops[i] = ops;
	pops[i]->family = family;
	sti();
	DPRINTF((net_debug, "NET: Installed protocol %d in slot %d (0x%X)\n",
						family, i, (long)ops));
	return(i);
  }
  sti();
  return(-ENOMEM);
}


void
sock_init(void)
{
  struct socket *sock;
  int i;

  /* Set up our SOCKET VFS major device. */
  if (register_chrdev(SOCKET_MAJOR, "socket", &net_fops) < 0) {
	printk("NET: cannot register major device %d!\n", SOCKET_MAJOR);
	return;
  }

  /* Release all sockets. */
  for (sock = sockets; sock <= last_socket; ++sock) sock->state = SS_FREE;

  /* Initialize all address (protocol) families. */
  for (i = 0; i < NPROTO; ++i) pops[i] = NULL;

  /* Initialize the DDI module. */
  ddi_init();

  /* Initialize the ARP module. */
#if 0
  arp_init();
#endif
}
