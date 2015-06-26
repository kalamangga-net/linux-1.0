/*
 * UNIX		An implementation of the AF_UNIX network domain for the
 *		LINUX operating system.  UNIX is implemented using the
 *		BSD Socket interface as the means of communication with
 *		the user level.
 *
 * Version:	@(#)sock.c	1.0.5	05/25/93
 *
 * Authors:	Orest Zborowski, <obz@Kodak.COM>
 *		Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 * Fixes:
 *		Alan Cox	:	Verify Area
 *		NET2E Team	:	Page fault locks
 *	Dmitry Gorodchanin	:	/proc locking
 *
 * To Do:
 *	Some nice person is looking into Unix sockets done properly. NET3
 *	will replace all of this and include datagram sockets and socket
 *	options - so please stop asking me for them 8-)
 *
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or(at your option) any later version.
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/socket.h>
#include <linux/un.h>
#include <linux/fcntl.h>
#include <linux/termios.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <linux/fs.h>
#include <linux/ddi.h>
#include <linux/malloc.h>

#include <asm/system.h>
#include <asm/segment.h>

#include <stdarg.h>

#include "unix.h"

struct unix_proto_data unix_datas[NSOCKETS];
static int unix_debug = 0;


static int unix_proto_create(struct socket *sock, int protocol);
static int unix_proto_dup(struct socket *newsock, struct socket *oldsock);
static int unix_proto_release(struct socket *sock, struct socket *peer);
static int unix_proto_bind(struct socket *sock, struct sockaddr *umyaddr,
			   int sockaddr_len);
static int unix_proto_connect(struct socket *sock, struct sockaddr *uservaddr,
			      int sockaddr_len, int flags);
static int unix_proto_socketpair(struct socket *sock1, struct socket *sock2);
static int unix_proto_accept(struct socket *sock, struct socket *newsock, 
			     int flags);
static int unix_proto_getname(struct socket *sock, struct sockaddr *usockaddr,
			      int *usockaddr_len, int peer);
static int unix_proto_read(struct socket *sock, char *ubuf, int size,
			   int nonblock);
static int unix_proto_write(struct socket *sock, char *ubuf, int size,
			    int nonblock);
static int unix_proto_select(struct socket *sock, int sel_type, select_table * wait);
static int unix_proto_ioctl(struct socket *sock, unsigned int cmd,
			    unsigned long arg);
static int unix_proto_listen(struct socket *sock, int backlog);
static int unix_proto_send(struct socket *sock, void *buff, int len,
			    int nonblock, unsigned flags);
static int unix_proto_recv(struct socket *sock, void *buff, int len,
			    int nonblock, unsigned flags);
static int unix_proto_sendto(struct socket *sock, void *buff, int len,
			      int nonblock, unsigned flags,
			      struct sockaddr *addr, int addr_len);
static int unix_proto_recvfrom(struct socket *sock, void *buff, int len,
				int nonblock, unsigned flags,
				struct sockaddr *addr, int *addr_len);

static int unix_proto_shutdown(struct socket *sock, int how);

static int unix_proto_setsockopt(struct socket *sock, int level, int optname,
				  char *optval, int optlen);
static int unix_proto_getsockopt(struct socket *sock, int level, int optname,
				  char *optval, int *optlen);


static void
dprintf(int level, char *fmt, ...)
{
  va_list args;
  char *buff;
  extern int vsprintf(char * buf, const char * fmt, va_list args);

  if (level != unix_debug) return;

  buff = (char *) kmalloc(256, GFP_KERNEL);
  if (buff != NULL) {
	va_start(args, fmt);
	vsprintf(buff, fmt, args);
	va_end(args);
	printk(buff);
	kfree(buff);
  }
}


static inline int
min(int a, int b)
{
  if (a < b) return(a);
  return(b);
}


void
sockaddr_un_printk(struct sockaddr_un *sockun, int sockaddr_len)
{
  char buf[sizeof(sockun->sun_path) + 1];

  if (unix_debug == 0) return;

  sockaddr_len -= UN_PATH_OFFSET;
  if (sockun->sun_family != AF_UNIX)
	printk("UNIX: Badd addr family %d>\n", sockun->sun_family);
    else if (sockaddr_len <= 0 || sockaddr_len >= sizeof(buf))
	printk("UNIX: Bad addr len %d>\n", sockaddr_len);
    else {
	memcpy(buf, sockun->sun_path, sockaddr_len);
	buf[sockaddr_len] = '\0';
	printk("\"%s\"[%lu]\n", buf, sockaddr_len + UN_PATH_OFFSET);
  }
}
  

/* Support routines doing anti page fault locking 
 * FvK & Matt Dillon (borrowed From NET2E3)
 */

/*
 * Locking for unix-domain sockets.  We don't use the socket structure's
 * wait queue because it is allowed to 'go away' outside of our control,
 * whereas unix_proto_data structures stick around.
 */
void unix_lock(struct unix_proto_data *upd)
{
	while (upd->lock_flag)
		sleep_on(&upd->wait);
	upd->lock_flag = 1;
}


void unix_unlock(struct unix_proto_data *upd)
{
	upd->lock_flag = 0;
	wake_up(&upd->wait);
}

/* don't have to do anything. */
static int
unix_proto_listen(struct socket *sock, int backlog)
{
  return(0);
}


static int
unix_proto_setsockopt(struct socket *sock, int level, int optname,
		      char *optval, int optlen)
{
  return(-EOPNOTSUPP);
}


static int
unix_proto_getsockopt(struct socket *sock, int level, int optname,
		      char *optval, int *optlen)
{
  return(-EOPNOTSUPP);
}

static int
unix_proto_sendto(struct socket *sock, void *buff, int len, int nonblock, 
		  unsigned flags,  struct sockaddr *addr, int addr_len)
{
  return(-EOPNOTSUPP);
}     

static int
unix_proto_recvfrom(struct socket *sock, void *buff, int len, int nonblock, 
		    unsigned flags, struct sockaddr *addr, int *addr_len)
{
  return(-EOPNOTSUPP);
}     


static int
unix_proto_shutdown(struct socket *sock, int how)
{
  return(-EOPNOTSUPP);
}


/* This error needs to be checked. */
static int
unix_proto_send(struct socket *sock, void *buff, int len, int nonblock,
		unsigned flags)
{
  if (flags != 0) return(-EINVAL);
  return(unix_proto_write(sock, (char *) buff, len, nonblock));
}


/* This error needs to be checked. */
static int
unix_proto_recv(struct socket *sock, void *buff, int len, int nonblock,
		unsigned flags)
{
  if (flags != 0) return(-EINVAL);
  return(unix_proto_read(sock, (char *) buff, len, nonblock));
}


static struct unix_proto_data *
unix_data_lookup(struct sockaddr_un *sockun, int sockaddr_len,
		 struct inode *inode)
{
  struct unix_proto_data *upd;

  for(upd = unix_datas; upd <= last_unix_data; ++upd) {
	if (upd->refcnt > 0 && upd->socket &&
	    upd->socket->state == SS_UNCONNECTED &&
	    upd->sockaddr_un.sun_family == sockun->sun_family &&
	    upd->inode == inode) return(upd);
  }
  return(NULL);
}


static struct unix_proto_data *
unix_data_alloc(void)
{
  struct unix_proto_data *upd;

  cli();
  for(upd = unix_datas; upd <= last_unix_data; ++upd) {
	if (!upd->refcnt) {
		upd->refcnt = -1;	/* unix domain socket not yet initialised - bgm */
		sti();
		upd->socket = NULL;
		upd->sockaddr_len = 0;
		upd->sockaddr_un.sun_family = 0;
		upd->buf = NULL;
		upd->bp_head = upd->bp_tail = 0;
		upd->inode = NULL;
		upd->peerupd = NULL;
		return(upd);
	}
  }
  sti();
  return(NULL);
}


static inline void
unix_data_ref(struct unix_proto_data *upd)
{
  if (!upd) {
    dprintf(1, "UNIX: data_ref: upd = NULL\n");
    return;
  }
  ++upd->refcnt;
  dprintf(1, "UNIX: data_ref: refing data 0x%x(%d)\n", upd, upd->refcnt);
}


static void
unix_data_deref(struct unix_proto_data *upd)
{
  if (!upd) {
    dprintf(1, "UNIX: data_deref: upd = NULL\n");
    return;
  }
  if (upd->refcnt == 1) {
	dprintf(1, "UNIX: data_deref: releasing data 0x%x\n", upd);
	if (upd->buf) {
		free_page((unsigned long)upd->buf);
		upd->buf = NULL;
		upd->bp_head = upd->bp_tail = 0;
	}
  }
  --upd->refcnt;
}


/*
 * Upon a create, we allocate an empty protocol data,
 * and grab a page to buffer writes.
 */
static int
unix_proto_create(struct socket *sock, int protocol)
{
  struct unix_proto_data *upd;

  dprintf(1, "UNIX: create: socket 0x%x, proto %d\n", sock, protocol);
  if (protocol != 0) {
	dprintf(1, "UNIX: create: protocol != 0\n");
	return(-EINVAL);
  }
  if (!(upd = unix_data_alloc())) {
	printk("UNIX: create: can't allocate buffer\n");
	return(-ENOMEM);
  }
  if (!(upd->buf = (char*) get_free_page(GFP_USER))) {
	printk("UNIX: create: can't get page!\n");
	unix_data_deref(upd);
	return(-ENOMEM);
  }
  upd->protocol = protocol;
  upd->socket = sock;
  UN_DATA(sock) = upd;
  upd->refcnt = 1;	/* Now its complete - bgm */
  dprintf(1, "UNIX: create: allocated data 0x%x\n", upd);
  return(0);
}


static int
unix_proto_dup(struct socket *newsock, struct socket *oldsock)
{
  struct unix_proto_data *upd = UN_DATA(oldsock);

  return(unix_proto_create(newsock, upd->protocol));
}


static int
unix_proto_release(struct socket *sock, struct socket *peer)
{
  struct unix_proto_data *upd = UN_DATA(sock);

  dprintf(1, "UNIX: release: socket 0x%x, unix_data 0x%x\n", sock, upd);
  if (!upd) return(0);
  if (upd->socket != sock) {
	printk("UNIX: release: socket link mismatch!\n");
	return(-EINVAL);
  }
  if (upd->inode) {
	dprintf(1, "UNIX: release: releasing inode 0x%x\n", upd->inode);
	iput(upd->inode);
	upd->inode = NULL;
  }
  UN_DATA(sock) = NULL;
  upd->socket = NULL;
  if (upd->peerupd) unix_data_deref(upd->peerupd);
  unix_data_deref(upd);
  return(0);
}


/*
 * Bind a name to a socket.
 * This is where much of the work is done: we allocate a fresh page for
 * the buffer, grab the appropriate inode and set things up.
 *
 * FIXME: what should we do if an address is already bound?
 *	  Here we return EINVAL, but it may be necessary to re-bind.
 *	  I think thats what BSD does in the case of datagram sockets...
 */
static int
unix_proto_bind(struct socket *sock, struct sockaddr *umyaddr,
		int sockaddr_len)
{
  char fname[sizeof(((struct sockaddr_un *)0)->sun_path) + 1];
  struct unix_proto_data *upd = UN_DATA(sock);
  unsigned long old_fs;
  int i;
  int er;

  dprintf(1, "UNIX: bind: socket 0x%x, len=%d\n", sock, sockaddr_len);
  if (sockaddr_len <= UN_PATH_OFFSET ||
      sockaddr_len > sizeof(struct sockaddr_un)) {
	dprintf(1, "UNIX: bind: bad length %d\n", sockaddr_len);
	return(-EINVAL);
  }
  if (upd->sockaddr_len || upd->inode) {
	printk("UNIX: bind: already bound!\n");
	return(-EINVAL);
  }
  er=verify_area(VERIFY_WRITE, umyaddr, sockaddr_len);
  if(er)
  	return er;
  memcpy_fromfs(&upd->sockaddr_un, umyaddr, sockaddr_len);
  upd->sockaddr_un.sun_path[sockaddr_len-UN_PATH_OFFSET] = '\0';
  if (upd->sockaddr_un.sun_family != AF_UNIX) {
	dprintf(1, "UNIX: bind: family is %d, not AF_UNIX(%d)\n",
	       			upd->sockaddr_un.sun_family, AF_UNIX);
	return(-EINVAL);
  }

  memcpy(fname, upd->sockaddr_un.sun_path, sockaddr_len-UN_PATH_OFFSET);
  fname[sockaddr_len-UN_PATH_OFFSET] = '\0';
  old_fs = get_fs();
  set_fs(get_ds());
  i = do_mknod(fname, S_IFSOCK | S_IRWXUGO, 0);
  if (i == 0) i = open_namei(fname, 0, S_IFSOCK, &upd->inode, NULL);
  set_fs(old_fs);
  if (i < 0) {
	printk("UNIX: bind: can't open socket %s\n", fname);
	return(i);
  }
  upd->sockaddr_len = sockaddr_len;	/* now its legal */

  dprintf(1, "UNIX: bind: bound socket address: ");
  sockaddr_un_printk(&upd->sockaddr_un, upd->sockaddr_len);
  dprintf(1, "to inode 0x%x\n", upd->inode);
  return(0);
}


/*
 * Perform a connection. we can only connect to unix sockets
 * (I can't for the life of me find an application where that
 * wouldn't be the case!)
 */
static int
unix_proto_connect(struct socket *sock, struct sockaddr *uservaddr,
		   int sockaddr_len, int flags)
{
  char fname[sizeof(((struct sockaddr_un *)0)->sun_path) + 1];
  struct sockaddr_un sockun;
  struct unix_proto_data *serv_upd;
  struct inode *inode;
  unsigned long old_fs;
  int i;
  int er;

  dprintf(1, "UNIX: connect: socket 0x%x, servlen=%d\n", sock, sockaddr_len);

  if (sockaddr_len <= UN_PATH_OFFSET ||
      sockaddr_len > sizeof(struct sockaddr_un)) {
	dprintf(1, "UNIX: connect: bad length %d\n", sockaddr_len);
	return(-EINVAL);
  }
  if (sock->state == SS_CONNECTING) return(-EINPROGRESS);
  if (sock->state == SS_CONNECTED) return(-EISCONN);

  er=verify_area(VERIFY_READ, uservaddr, sockaddr_len);
  if(er)
  	return er;
  memcpy_fromfs(&sockun, uservaddr, sockaddr_len);
  sockun.sun_path[sockaddr_len-UN_PATH_OFFSET] = '\0';
  if (sockun.sun_family != AF_UNIX) {
	dprintf(1, "UNIX: connect: family is %d, not AF_UNIX(%d)\n",
	       					sockun.sun_family, AF_UNIX);
	return(-EINVAL);
  }

  /*
   * Try to open the name in the filesystem - this is how we
   * identify ourselves and our server. Note that we don't
   * hold onto the inode that long, just enough to find our
   * server. When we're connected, we mooch off the server.
   */
  memcpy(fname, sockun.sun_path, sockaddr_len-UN_PATH_OFFSET);
  fname[sockaddr_len-UN_PATH_OFFSET] = '\0';
  old_fs = get_fs();
  set_fs(get_ds());
  i = open_namei(fname, 0, S_IFSOCK, &inode, NULL);
  set_fs(old_fs);
  if (i < 0) {
	dprintf(1, "UNIX: connect: can't open socket %s\n", fname);
	return(i);
  }
  serv_upd = unix_data_lookup(&sockun, sockaddr_len, inode);
  iput(inode);
  if (!serv_upd) {
	dprintf(1, "UNIX: connect: can't locate peer %s at inode 0x%x\n",
								fname, inode);
	return(-EINVAL);
  }
  if ((i = sock_awaitconn(sock, serv_upd->socket)) < 0) {
	dprintf(1, "UNIX: connect: can't await connection\n");
	return(i);
  }
  if (sock->conn) {
	unix_data_ref(UN_DATA(sock->conn));
	UN_DATA(sock)->peerupd = UN_DATA(sock->conn); /* ref server */
  }
  return(0);
}


/*
 * To do a socketpair, we just connect the two datas, easy!
 * Since we always wait on the socket inode, they're no contention
 * for a wait area, and deadlock prevention in the case of a process
 * writing to itself is, ignored, in true unix fashion!
 */
static int
unix_proto_socketpair(struct socket *sock1, struct socket *sock2)
{
  struct unix_proto_data *upd1 = UN_DATA(sock1), *upd2 = UN_DATA(sock2);

  unix_data_ref(upd1);
  unix_data_ref(upd2);
  upd1->peerupd = upd2;
  upd2->peerupd = upd1;
  return(0);
}


/* On accept, we ref the peer's data for safe writes. */
static int
unix_proto_accept(struct socket *sock, struct socket *newsock, int flags)
{
  struct socket *clientsock;

  dprintf(1, "UNIX: accept: socket 0x%x accepted via socket 0x%x\n",
							sock, newsock);

  /*
   * If there aren't any sockets awaiting connection,
   * then wait for one, unless nonblocking.
   */
  while(!(clientsock = sock->iconn)) {
	if (flags & O_NONBLOCK) return(-EAGAIN);
	interruptible_sleep_on(sock->wait);
	if (current->signal & ~current->blocked) {
		dprintf(1, "UNIX: accept: sleep was interrupted\n");
		return(-ERESTARTSYS);
	}
  }

  /*
   * Great. Finish the connection relative to server and client,
   * wake up the client and return the new fd to the server.
   */
  sock->iconn = clientsock->next;
  clientsock->next = NULL;
  newsock->conn = clientsock;
  clientsock->conn = newsock;
  clientsock->state = SS_CONNECTED;
  newsock->state = SS_CONNECTED;
  unix_data_ref(UN_DATA(clientsock));
  UN_DATA(newsock)->peerupd	       = UN_DATA(clientsock);
  UN_DATA(newsock)->sockaddr_un        = UN_DATA(sock)->sockaddr_un;
  UN_DATA(newsock)->sockaddr_len       = UN_DATA(sock)->sockaddr_len;
  wake_up_interruptible(clientsock->wait);
  return(0);
}


/* Gets the current name or the name of the connected socket. */
static int
unix_proto_getname(struct socket *sock, struct sockaddr *usockaddr,
		   int *usockaddr_len, int peer)
{
  struct unix_proto_data *upd;
  int len;
  int er;

  dprintf(1, "UNIX: getname: socket 0x%x for %s\n", sock, peer?"peer":"self");
  if (peer) {
	if (sock->state != SS_CONNECTED) {
		dprintf(1, "UNIX: getname: socket not connected\n");
		return(-EINVAL);
	}
	upd = UN_DATA(sock->conn);
  } else
	upd = UN_DATA(sock);

  er=verify_area(VERIFY_WRITE, usockaddr_len, sizeof(*usockaddr_len));
  if(er)
  	return er;
  if ((len = get_fs_long(usockaddr_len)) <= 0) return(-EINVAL);
  if (len > upd->sockaddr_len) len = upd->sockaddr_len;
  if (len) {
	er=verify_area(VERIFY_WRITE, usockaddr, len);
	if(er)
		return er;
	memcpy_tofs(usockaddr, &upd->sockaddr_un, len);
  }
  put_fs_long(len, usockaddr_len);
  return(0);
}


/* We read from our own buf. */
static int
unix_proto_read(struct socket *sock, char *ubuf, int size, int nonblock)
{
  struct unix_proto_data *upd;
  int todo, avail;
  int er;

  if ((todo = size) <= 0) return(0);
  upd = UN_DATA(sock);
  while(!(avail = UN_BUF_AVAIL(upd))) {
	if (sock->state != SS_CONNECTED) {
		dprintf(1, "UNIX: read: socket not connected\n");
		return((sock->state == SS_DISCONNECTING) ? 0 : -EINVAL);
	}
	dprintf(1, "UNIX: read: no data available...\n");
	if (nonblock) return(-EAGAIN);
	interruptible_sleep_on(sock->wait);
	if (current->signal & ~current->blocked) {
		dprintf(1, "UNIX: read: interrupted\n");
		return(-ERESTARTSYS);
	}
  }

  /*
   * Copy from the read buffer into the user's buffer,
   * watching for wraparound. Then we wake up the writer.
   */
   
  unix_lock(upd);
  do {
	int part, cando;

	if (avail <= 0) {
		printk("UNIX: read: AVAIL IS NEGATIVE!!!\n");
		send_sig(SIGKILL, current, 1);
		return(-EPIPE);
	}

	if ((cando = todo) > avail) cando = avail;
	if (cando >(part = BUF_SIZE - upd->bp_tail)) cando = part;
	dprintf(1, "UNIX: read: avail=%d, todo=%d, cando=%d\n",
	       					avail, todo, cando);
	if((er=verify_area(VERIFY_WRITE,ubuf,cando))<0)
	{
		unix_unlock(upd);
		return er;
	}
	memcpy_tofs(ubuf, upd->buf + upd->bp_tail, cando);
	upd->bp_tail =(upd->bp_tail + cando) &(BUF_SIZE-1);
	ubuf += cando;
	todo -= cando;
	if (sock->state == SS_CONNECTED)
		wake_up_interruptible(sock->conn->wait);
	avail = UN_BUF_AVAIL(upd);
  } while(todo && avail);
  unix_unlock(upd);
  return(size - todo);
}


/*
 * We write to our peer's buf. When we connected we ref'd this
 * peer so we are safe that the buffer remains, even after the
 * peer has disconnected, which we check other ways.
 */
static int
unix_proto_write(struct socket *sock, char *ubuf, int size, int nonblock)
{
  struct unix_proto_data *pupd;
  int todo, space;
  int er;

  if ((todo = size) <= 0) return(0);
  if (sock->state != SS_CONNECTED) {
	dprintf(1, "UNIX: write: socket not connected\n");
	if (sock->state == SS_DISCONNECTING) {
		send_sig(SIGPIPE, current, 1);
		return(-EPIPE);
	}
	return(-EINVAL);
  }
  pupd = UN_DATA(sock)->peerupd;	/* safer than sock->conn */

  while(!(space = UN_BUF_SPACE(pupd))) {
	dprintf(1, "UNIX: write: no space left...\n");
	if (nonblock) return(-EAGAIN);
	interruptible_sleep_on(sock->wait);
	if (current->signal & ~current->blocked) {
		dprintf(1, "UNIX: write: interrupted\n");
		return(-ERESTARTSYS);
	}
	if (sock->state == SS_DISCONNECTING) {
		dprintf(1, "UNIX: write: disconnected(SIGPIPE)\n");
		send_sig(SIGPIPE, current, 1);
		return(-EPIPE);
	}
  }

  /*
   * Copy from the user's buffer to the write buffer,
   * watching for wraparound. Then we wake up the reader.
   */
   
  unix_lock(pupd);
  
  do {
	int part, cando;

	if (space <= 0) {
		printk("UNIX: write: SPACE IS NEGATIVE!!!\n");
		send_sig(SIGKILL, current, 1);
		return(-EPIPE);
	}

	/*
	 * We may become disconnected inside this loop, so watch
	 * for it (peerupd is safe until we close).
	 */
	if (sock->state == SS_DISCONNECTING) {
		send_sig(SIGPIPE, current, 1);
		unix_unlock(pupd);
		return(-EPIPE);
	}
	if ((cando = todo) > space) cando = space;
	if (cando >(part = BUF_SIZE - pupd->bp_head)) cando = part;
	dprintf(1, "UNIX: write: space=%d, todo=%d, cando=%d\n",
	       					space, todo, cando);
	er=verify_area(VERIFY_READ, ubuf, cando);
	if(er)
	{
		unix_unlock(pupd);
		return er;
	}
	memcpy_fromfs(pupd->buf + pupd->bp_head, ubuf, cando);
	pupd->bp_head =(pupd->bp_head + cando) &(BUF_SIZE-1);
	ubuf += cando;
	todo -= cando;
	if (sock->state == SS_CONNECTED)
		wake_up_interruptible(sock->conn->wait);
	space = UN_BUF_SPACE(pupd);
  } while(todo && space);
  unix_unlock(pupd);
  return(size - todo);
}


static int
unix_proto_select(struct socket *sock, int sel_type, select_table * wait)
{
  struct unix_proto_data *upd, *peerupd;

  /* Handle server sockets specially. */
  if (sock->flags & SO_ACCEPTCON) {
	if (sel_type == SEL_IN) {
		dprintf(1, "UNIX: select: %sconnections pending\n",
		       				sock->iconn ? "" : "no ");
		if (sock->iconn) return(1);
		select_wait(sock->wait, wait);
		return(sock->iconn ? 1 : 0);
	}
	dprintf(1, "UNIX: select: nothing else for server socket\n");
	select_wait(sock->wait, wait);
	return(0);
  }

  if (sel_type == SEL_IN) {
	upd = UN_DATA(sock);
	dprintf(1, "UNIX: select: there is%s data available\n",
	       					UN_BUF_AVAIL(upd) ? "" : " no");
	if (UN_BUF_AVAIL(upd))	/* even if disconnected */
			return(1);
	else if (sock->state != SS_CONNECTED) {
		dprintf(1, "UNIX: select: socket not connected(read EOF)\n");
		return(1);
	}
	select_wait(sock->wait,wait);
	return(0);
  }
  if (sel_type == SEL_OUT) {
	if (sock->state != SS_CONNECTED) {
		dprintf(1, "UNIX: select: socket not connected(write EOF)\n");
		return(1);
	}
	peerupd = UN_DATA(sock->conn);
	dprintf(1, "UNIX: select: there is%s space available\n",
	       				UN_BUF_SPACE(peerupd) ? "" : " no");
	if (UN_BUF_SPACE(peerupd) > 0) return(1);
	select_wait(sock->wait,wait);
	return(0);
  }

  /* SEL_EX */
  dprintf(1, "UNIX: select: there are no exceptions here?!\n");
  return(0);
}


static int
unix_proto_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
  struct unix_proto_data *upd, *peerupd;
  int er;

  upd = UN_DATA(sock);
  peerupd = (sock->state == SS_CONNECTED) ? UN_DATA(sock->conn) : NULL;

  switch(cmd) {
	case TIOCINQ:
		if (sock->flags & SO_ACCEPTCON) return(-EINVAL);
		er=verify_area(VERIFY_WRITE,(void *)arg, sizeof(unsigned long));
		if(er)
			return er;
		if (UN_BUF_AVAIL(upd) || peerupd)
			put_fs_long(UN_BUF_AVAIL(upd),(unsigned long *)arg);
		  else
			put_fs_long(0,(unsigned long *)arg);
		break;
	case TIOCOUTQ:
		if (sock->flags & SO_ACCEPTCON) return(-EINVAL);
		er=verify_area(VERIFY_WRITE,(void *)arg, sizeof(unsigned long));
		if(er)
			return er;
		if (peerupd) put_fs_long(UN_BUF_SPACE(peerupd),
				   		(unsigned long *)arg);
		  else
			put_fs_long(0,(unsigned long *)arg);
		break;
	default:
		return(-EINVAL);
  }
  return(0);
}


static int
unix_open(struct inode * inode, struct file * file)
{
  int minor;

  dprintf(1, "UNIX: open\n");
  minor = MINOR(inode->i_rdev);
  if (minor != 0) return(-ENODEV);

  return(0);
}


static void
unix_close(struct inode * inode, struct file * file)
{
  dprintf(1, "UNIX: close\n");
}


static int
unix_ioctl(struct inode *inode, struct file *file,
	 unsigned int cmd, unsigned long arg)
{
  int minor, ret;
  int er;

  dprintf(1, "UNIX: ioctl(0x%X, 0x%X)\n", cmd, arg);
  minor = MINOR(inode->i_rdev);
  if (minor != 0) return(-ENODEV);

  ret = -EINVAL;
  switch(cmd) {
	case DDIOCSDBG:
		er=verify_area(VERIFY_READ,(void *)arg, sizeof(int));
		if(er)
			return er;
		unix_debug = get_fs_long((int *)arg);
		if (unix_debug != 0 && unix_debug != 1) {
			unix_debug = 0;
			return(-EINVAL);
		}
		return(0);
	case SIOCSIFLINK:
		printk("UNIX: cannot link streams!\n");
		break;
	default:
		break;
  }
  return(ret);
}




static struct file_operations unix_fops = {
  NULL,		/* LSEEK	*/
  NULL,		/* READ		*/
  NULL,		/* WRITE	*/
  NULL,		/* READDIR	*/
  NULL,		/* SELECT	*/
  unix_ioctl,	/* IOCTL	*/
  NULL,		/* MMAP		*/
  unix_open,	/* OPEN		*/
  unix_close	/* CLOSE	*/
};


static struct proto_ops unix_proto_ops = {
  AF_UNIX,
  unix_proto_create,
  unix_proto_dup,
  unix_proto_release,
  unix_proto_bind,
  unix_proto_connect,
  unix_proto_socketpair,
  unix_proto_accept,
  unix_proto_getname,
  unix_proto_read,
  unix_proto_write,
  unix_proto_select,
  unix_proto_ioctl,
  unix_proto_listen,
  unix_proto_send,
  unix_proto_recv,
  unix_proto_sendto,
  unix_proto_recvfrom,
  unix_proto_shutdown,
  unix_proto_setsockopt,
  unix_proto_getsockopt,
  NULL				/* unix_proto_fcntl	*/
};


void
unix_proto_init(struct ddi_proto *pro)
{
  struct unix_proto_data *upd;

  dprintf(1, "%s: init: initializing...\n", pro->name);
  if (register_chrdev(AF_UNIX_MAJOR, "af_unix", &unix_fops) < 0) {
	printk("%s: cannot register major device %d!\n",
						pro->name, AF_UNIX_MAJOR);
	return;
  }

  /* Tell SOCKET that we are alive... */
  (void) sock_register(unix_proto_ops.family, &unix_proto_ops);

  for(upd = unix_datas; upd <= last_unix_data; ++upd) {
	upd->refcnt = 0;
  }
}
