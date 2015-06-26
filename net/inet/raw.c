/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		RAW - implementation of IP "raw" sockets.
 *
 * Version:	@(#)raw.c	1.0.4	05/25/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 * Fixes:
 *		Alan Cox	:	verify_area() fixed up
 *		Alan Cox	:	ICMP error handling
 *		Alan Cox	:	EMSGSIZE if you send too big a packet
 *		Alan Cox	: 	Now uses generic datagrams and shared skbuff
 *					library. No more peek crashes, no more backlogs
 *		Alan Cox	:	Checks sk->broadcast.
 *		Alan Cox	:	Uses skb_free_datagram/skb_copy_datagram
 *		Alan Cox	:	Raw passes ip options too
 *		Alan Cox	:	Setsocketopt added
 *		Alan Cox	:	Fixed error return for broadcasts
 *		Alan Cox	:	Removed wake_up calls
 *		Alan Cox	:	Use ttl/tos
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#include <asm/system.h>
#include <asm/segment.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/kernel.h>
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
#include "icmp.h"
#include "udp.h"


static unsigned long
min(unsigned long a, unsigned long b)
{
  if (a < b) return(a);
  return(b);
}


/* raw_err gets called by the icmp module. */
void
raw_err (int err, unsigned char *header, unsigned long daddr,
	 unsigned long saddr, struct inet_protocol *protocol)
{
  struct sock *sk;
   
  DPRINTF((DBG_RAW, "raw_err(err=%d, hdr=%X, daddr=%X, saddr=%X, protocl=%X)\n",
		err, header, daddr, saddr, protocol));

  if (protocol == NULL) return;
  sk = (struct sock *) protocol->data;
  if (sk == NULL) return;

  /* This is meaningless in raw sockets. */
  if (err & 0xff00 == (ICMP_SOURCE_QUENCH << 8)) {
	if (sk->cong_window > 1) sk->cong_window = sk->cong_window/2;
	return;
  }

  sk->err = icmp_err_convert[err & 0xff].errno;
  sk->error_report(sk);
  
  return;
}


/*
 * This should be the easiest of all, all we do is\
 * copy it into a buffer.
 */
int
raw_rcv(struct sk_buff *skb, struct device *dev, struct options *opt,
	unsigned long daddr, unsigned short len, unsigned long saddr,
	int redo, struct inet_protocol *protocol)
{
  struct sock *sk;

  DPRINTF((DBG_RAW, "raw_rcv(skb=%X, dev=%X, opt=%X, daddr=%X,\n"
	   "         len=%d, saddr=%X, redo=%d, protocol=%X)\n",
	   skb, dev, opt, daddr, len, saddr, redo, protocol));

  if (skb == NULL) return(0);
  if (protocol == NULL) {
	kfree_skb(skb, FREE_READ);
	return(0);
  }
  sk = (struct sock *) protocol->data;
  if (sk == NULL) {
	kfree_skb(skb, FREE_READ);
	return(0);
  }

  /* Now we need to copy this into memory. */
  skb->sk = sk;
  skb->len = len + skb->ip_hdr->ihl*sizeof(long);
  skb->h.raw = (unsigned char *) skb->ip_hdr;
  skb->dev = dev;
  skb->saddr = daddr;
  skb->daddr = saddr;

  /* Charge it too the socket. */
  if (sk->rmem_alloc + skb->mem_len >= sk->rcvbuf) {
	skb->sk = NULL;
	kfree_skb(skb, FREE_READ);
	return(0);
  }
  sk->rmem_alloc += skb->mem_len;
  skb_queue_tail(&sk->rqueue,skb);
  sk->data_ready(sk,skb->len);
  release_sock(sk);
  return(0);
}


/* This will do terrible things if len + ipheader + devheader > dev->mtu */
static int
raw_sendto(struct sock *sk, unsigned char *from, int len,
	   int noblock,
	   unsigned flags, struct sockaddr_in *usin, int addr_len)
{
  struct sk_buff *skb;
  struct device *dev=NULL;
  struct sockaddr_in sin;
  int tmp;
  int err;

  DPRINTF((DBG_RAW, "raw_sendto(sk=%X, from=%X, len=%d, noblock=%d, flags=%X,\n"
	   "            usin=%X, addr_len = %d)\n", sk, from, len, noblock,
	   flags, usin, addr_len));

  /* Check the flags. */
  if (flags) return(-EINVAL);
  if (len < 0) return(-EINVAL);

  err=verify_area(VERIFY_READ,from,len);
  if(err)
  	return err;
  /* Get and verify the address. */
  if (usin) {
	if (addr_len < sizeof(sin)) return(-EINVAL);
	err=verify_area (VERIFY_READ, usin, sizeof (sin));
	if(err)
		return err;
	memcpy_fromfs(&sin, usin, sizeof(sin));
	if (sin.sin_family && sin.sin_family != AF_INET) return(-EINVAL);
  } else {
	if (sk->state != TCP_ESTABLISHED) return(-EINVAL);
	sin.sin_family = AF_INET;
	sin.sin_port = sk->protocol;
	sin.sin_addr.s_addr = sk->daddr;
  }
  if (sin.sin_port == 0) sin.sin_port = sk->protocol;
  
  if (sk->broadcast == 0 && chk_addr(sin.sin_addr.s_addr)==IS_BROADCAST)
  	return -EACCES;

  sk->inuse = 1;
  skb = NULL;
  while (skb == NULL) {
  	if(sk->err!=0)
  	{
  		err= -sk->err;
  		sk->err=0;
  		release_sock(sk);
  		return(err);
  	}
  	
	skb = sk->prot->wmalloc(sk,
			len+sizeof(*skb) + sk->prot->max_header,
			0, GFP_KERNEL);
	if (skb == NULL) {
		int tmp;

		DPRINTF((DBG_RAW, "raw_sendto: write buffer full?\n"));
		if (noblock) 
			return(-EAGAIN);
		tmp = sk->wmem_alloc;
		release_sock(sk);
		cli();
		if (tmp <= sk->wmem_alloc) {
			interruptible_sleep_on(sk->sleep);
			if (current->signal & ~current->blocked) {
				sti();
				return(-ERESTARTSYS);
			}
		}
		sk->inuse = 1;
		sti();
	}
  }
  skb->mem_addr = skb;
  skb->mem_len = len + sizeof(*skb) +sk->prot->max_header;
  skb->sk = sk;

  skb->free = 1; /* these two should be unecessary. */
  skb->arp = 0;

  tmp = sk->prot->build_header(skb, sk->saddr, 
			       sin.sin_addr.s_addr, &dev,
			       sk->protocol, sk->opt, skb->mem_len, sk->ip_tos,sk->ip_ttl);
  if (tmp < 0) {
	DPRINTF((DBG_RAW, "raw_sendto: error building ip header.\n"));
	kfree_skb(skb,FREE_WRITE);
	release_sock(sk);
	return(tmp);
  }

  /* verify_area(VERIFY_WRITE, from, len);*/
  memcpy_fromfs(skb->data + tmp, from, len);

  /* If we are using IPPROTO_RAW, we need to fill in the source address in
     the IP header */

  if(sk->protocol==IPPROTO_RAW) {
    unsigned char *buff;
    struct iphdr *iph;

    buff = skb->data;
    buff += tmp;
    iph = (struct iphdr *)buff;
    iph->saddr = sk->saddr;
  }

  skb->len = tmp + len;
  
  if(dev!=NULL && skb->len > 4095)
  {
  	kfree_skb(skb, FREE_WRITE);
  	release_sock(sk);
  	return(-EMSGSIZE);
  }
  
  sk->prot->queue_xmit(sk, dev, skb, 1);
  release_sock(sk);
  return(len);
}


static int
raw_write(struct sock *sk, unsigned char *buff, int len, int noblock,
	   unsigned flags)
{
  return(raw_sendto(sk, buff, len, noblock, flags, NULL, 0));
}


static void
raw_close(struct sock *sk, int timeout)
{
  sk->inuse = 1;
  sk->state = TCP_CLOSE;

  DPRINTF((DBG_RAW, "raw_close: deleting protocol %d\n",
	   ((struct inet_protocol *)sk->pair)->protocol));

  if (inet_del_protocol((struct inet_protocol *)sk->pair) < 0)
		DPRINTF((DBG_RAW, "raw_close: del_protocol failed.\n"));
  kfree_s((void *)sk->pair, sizeof (struct inet_protocol));
  sk->pair = NULL;
  release_sock(sk);
}


static int
raw_init(struct sock *sk)
{
  struct inet_protocol *p;

  p = (struct inet_protocol *) kmalloc(sizeof (*p), GFP_KERNEL);
  if (p == NULL) return(-ENOMEM);

  p->handler = raw_rcv;
  p->protocol = sk->protocol;
  p->data = (void *)sk;
  p->err_handler = raw_err;
  p->name="USER";
  p->frag_handler = NULL;	/* For now */
  inet_add_protocol(p);
   
  /* We need to remember this somewhere. */
  sk->pair = (struct sock *)p;

  DPRINTF((DBG_RAW, "raw init added protocol %d\n", sk->protocol));

  return(0);
}


/*
 * This should be easy, if there is something there
 * we return it, otherwise we block.
 */
int
raw_recvfrom(struct sock *sk, unsigned char *to, int len,
	     int noblock, unsigned flags, struct sockaddr_in *sin,
	     int *addr_len)
{
  int copied=0;
  struct sk_buff *skb;
  int err;

  DPRINTF((DBG_RAW, "raw_recvfrom (sk=%X, to=%X, len=%d, noblock=%d, flags=%X,\n"
	   "              sin=%X, addr_len=%X)\n",
		sk, to, len, noblock, flags, sin, addr_len));

  if (len == 0) return(0);
  if (len < 0) return(-EINVAL);

  if (sk->shutdown & RCV_SHUTDOWN) return(0);
  if (addr_len) {
	err=verify_area(VERIFY_WRITE, addr_len, sizeof(*addr_len));
	if(err)
		return err;
	put_fs_long(sizeof(*sin), addr_len);
  }
  if(sin)
  {
  	err=verify_area(VERIFY_WRITE, sin, sizeof(*sin));
	if(err)
		return err;
  }
  
  err=verify_area(VERIFY_WRITE,to,len);
  if(err)
  	return err;

  skb=skb_recv_datagram(sk,flags,noblock,&err);
  if(skb==NULL)
  	return err;

  copied = min(len, skb->len);
  
  skb_copy_datagram(skb, 0, to, copied);

  /* Copy the address. */
  if (sin) {
	struct sockaddr_in addr;

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = skb->daddr;
	memcpy_tofs(sin, &addr, sizeof(*sin));
  }

  skb_free_datagram(skb);
  release_sock(sk);
  return (copied);
}


int
raw_read (struct sock *sk, unsigned char *buff, int len, int noblock,
	  unsigned flags)
{
  return(raw_recvfrom(sk, buff, len, noblock, flags, NULL, NULL));
}


struct proto raw_prot = {
  sock_wmalloc,
  sock_rmalloc,
  sock_wfree,
  sock_rfree,
  sock_rspace,
  sock_wspace,
  raw_close,
  raw_read,
  raw_write,
  raw_sendto,
  raw_recvfrom,
  ip_build_header,
  udp_connect,
  NULL,
  ip_queue_xmit,
  ip_retransmit,
  NULL,
  NULL,
  raw_rcv,
  datagram_select,
  NULL,
  raw_init,
  NULL,
  ip_setsockopt,
  ip_getsockopt,
  128,
  0,
  {NULL,},
  "RAW"
};
