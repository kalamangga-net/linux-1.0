/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		PACKET - implements raw packet sockets.
 *
 * Version:	@(#)packet.c	1.0.6	05/25/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 * Fixes:	
 *		Alan Cox	:	verify_area() now used correctly
 *		Alan Cox	:	new skbuff lists, look ma no backlogs!
 *		Alan Cox	:	tidied skbuff lists.
 *		Alan Cox	:	Now uses generic datagram routines I
 *					added. Also fixed the peek/read crash
 *					from all old Linux datagram code.
 *		Alan Cox	:	Uses the improved datagram code.
 *		Alan Cox	:	Added NULL's for socket options.
 *
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/fcntl.h>
#include <linux/socket.h>
#include <linux/in.h>
#include "inet.h"
#include "dev.h"
#include "ip.h"
#include "protocol.h"
#include "tcp.h"
#include "skbuff.h"
#include "sock.h"
#include <linux/errno.h>
#include <linux/timer.h>
#include <asm/system.h>
#include <asm/segment.h>
#include "udp.h"
#include "raw.h"


static unsigned long
min(unsigned long a, unsigned long b)
{
  if (a < b) return(a);
  return(b);
}


/* This should be the easiest of all, all we do is copy it into a buffer. */
int
packet_rcv(struct sk_buff *skb, struct device *dev,  struct packet_type *pt)
{
  struct sock *sk;

  sk = (struct sock *) pt->data;
  skb->dev = dev;
  skb->len += dev->hard_header_len;

  skb->sk = sk;

  /* Charge it too the socket. */
  if (sk->rmem_alloc + skb->mem_len >= sk->rcvbuf) {
	skb->sk = NULL;
	kfree_skb(skb, FREE_READ);
	return(0);
  }
  sk->rmem_alloc += skb->mem_len;
  skb_queue_tail(&sk->rqueue,skb);
  wake_up_interruptible(sk->sleep);
  release_sock(sk);
  return(0);
}


/* This will do terrible things if len + ipheader + devheader > dev->mtu */
static int
packet_sendto(struct sock *sk, unsigned char *from, int len,
	      int noblock, unsigned flags, struct sockaddr_in *usin,
	      int addr_len)
{
  struct sk_buff *skb;
  struct device *dev;
  struct sockaddr saddr;
  int err;

  /* Check the flags. */
  if (flags) return(-EINVAL);
  if (len < 0) return(-EINVAL);

  /* Get and verify the address. */
  if (usin) {
	if (addr_len < sizeof(saddr)) return(-EINVAL);
	err=verify_area(VERIFY_READ, usin, sizeof(saddr));
	if(err)
		return err;
	memcpy_fromfs(&saddr, usin, sizeof(saddr));
  } else
	return(-EINVAL);
	
  err=verify_area(VERIFY_READ,from,len);
  if(err)
  	return(err);
/* Find the device first to size check it */

  saddr.sa_data[13] = 0;
  dev = dev_get(saddr.sa_data);
  if (dev == NULL) {
	return(-ENXIO);
  }
  if(len>dev->mtu)
  	return -EMSGSIZE;

/* Now allocate the buffer, knowing 4K pagelimits wont break this line */  
  skb = sk->prot->wmalloc(sk, len+sizeof(*skb), 0, GFP_KERNEL);

  /* This shouldn't happen, but it could. */
  if (skb == NULL) {
	DPRINTF((DBG_PKT, "packet_sendto: write buffer full?\n"));
	return(-ENOMEM);
  }
  /* Fill it in */
  skb->mem_addr = skb;
  skb->mem_len = len + sizeof(*skb);
  skb->sk = sk;
  skb->free = 1;
  memcpy_fromfs(skb->data, from, len);
  skb->len = len;
  skb->next = NULL;
  skb->arp = 1;
  if (dev->flags & IFF_UP) dev->queue_xmit(skb, dev, sk->priority);
    else kfree_skb(skb, FREE_WRITE);
  return(len);
}


static int
packet_write(struct sock *sk, unsigned char *buff, 
	     int len, int noblock,  unsigned flags)
{
  return(packet_sendto(sk, buff, len, noblock, flags, NULL, 0));
}


static void
packet_close(struct sock *sk, int timeout)
{
  sk->inuse = 1;
  sk->state = TCP_CLOSE;
  dev_remove_pack((struct packet_type *)sk->pair);
  kfree_s((void *)sk->pair, sizeof(struct packet_type));
  sk->pair = NULL;
  release_sock(sk);
}


static int
packet_init(struct sock *sk)
{
  struct packet_type *p;

  p = (struct packet_type *) kmalloc(sizeof(*p), GFP_KERNEL);
  if (p == NULL) return(-ENOMEM);

  p->func = packet_rcv;
  p->type = sk->num;
  p->data = (void *)sk;
  dev_add_pack(p);
   
  /* We need to remember this somewhere. */
  sk->pair = (struct sock *)p;

  return(0);
}


/*
 * This should be easy, if there is something there
 * we return it, otherwise we block.
 */
int
packet_recvfrom(struct sock *sk, unsigned char *to, int len,
	        int noblock, unsigned flags, struct sockaddr_in *sin,
	        int *addr_len)
{
  int copied=0;
  struct sk_buff *skb;
  struct sockaddr *saddr;
  int err;

  saddr = (struct sockaddr *)sin;
  if (len == 0) return(0);
  if (len < 0) return(-EINVAL);

  if (sk->shutdown & RCV_SHUTDOWN) return(0);
  if (addr_len) {
	  err=verify_area(VERIFY_WRITE, addr_len, sizeof(*addr_len));
	  if(err)
	  	return err;
	  put_fs_long(sizeof(*saddr), addr_len);
  }
  
  err=verify_area(VERIFY_WRITE,to,len);
  if(err)
  	return err;
  skb=skb_recv_datagram(sk,flags,noblock,&err);
  if(skb==NULL)
  	return err;
  copied = min(len, skb->len);

  memcpy_tofs(to, skb->data, copied);	/* Don't use skb_copy_datagram here: We can't get frag chains */

  /* Copy the address. */
  if (saddr) {
	struct sockaddr addr;

	addr.sa_family = skb->dev->type;
	memcpy(addr.sa_data,skb->dev->name, 14);
	verify_area(VERIFY_WRITE, saddr, sizeof(*saddr));
	memcpy_tofs(saddr, &addr, sizeof(*saddr));
  }

  skb_free_datagram(skb);		/* Its either been used up, or its a peek_copy anyway */

  release_sock(sk);
  return(copied);
}


int
packet_read(struct sock *sk, unsigned char *buff,
	    int len, int noblock, unsigned flags)
{
  return(packet_recvfrom(sk, buff, len, noblock, flags, NULL, NULL));
}


struct proto packet_prot = {
  sock_wmalloc,
  sock_rmalloc,
  sock_wfree,
  sock_rfree,
  sock_rspace,
  sock_wspace,
  packet_close,
  packet_read,
  packet_write,
  packet_sendto,
  packet_recvfrom,
  ip_build_header,
  udp_connect,
  NULL,
  ip_queue_xmit,
  ip_retransmit,
  NULL,
  NULL,
  NULL, 
  datagram_select,
  NULL,
  packet_init,
  NULL,
  NULL,	/* No set/get socket options */
  NULL,
  128,
  0,
  {NULL,},
  "PACKET"
};
