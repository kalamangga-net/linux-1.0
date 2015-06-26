/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Pseudo-driver for the loopback interface.
 *
 * Version:	@(#)loopback.c	1.0.4b	08/16/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Donald Becker, <becker@super.org>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/socket.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/in.h>
#include <linux/if_ether.h>	/* For the statistics structure. */

#include <asm/system.h>
#include <asm/segment.h>
#include <asm/io.h>

#include "inet.h"
#include "dev.h"
#include "eth.h"
#include "ip.h"
#include "protocol.h"
#include "tcp.h"
#include "skbuff.h"
#include "sock.h"
#include "arp.h"


static int
loopback_xmit(struct sk_buff *skb, struct device *dev)
{
  struct enet_statistics *stats = (struct enet_statistics *)dev->priv;
  int done;

  DPRINTF((DBG_LOOPB, "loopback_xmit(dev=%X, skb=%X)\n", dev, skb));
  if (skb == NULL || dev == NULL) return(0);

  cli();
  if (dev->tbusy != 0) {
	sti();
	stats->tx_errors++;
	return(1);
  }
  dev->tbusy = 1;
  sti();

  done = dev_rint(skb->data, skb->len, 0, dev);
  if (skb->free) kfree_skb(skb, FREE_WRITE);

  while (done != 1) {
	done = dev_rint(NULL, 0, 0, dev);
  }
  stats->tx_packets++;

  dev->tbusy = 0;

#if 1
	__asm__("cmpl $0,_intr_count\n\t"
		"jne 1f\n\t"
		"movl _bh_active,%%eax\n\t"
		"testl _bh_mask,%%eax\n\t"
		"je 1f\n\t"
		"incl _intr_count\n\t"
		"call _do_bottom_half\n\t"
		"decl _intr_count\n"
		"1:"
		:
		:
		: "ax", "dx", "cx");
#endif

  return(0);
}

static struct enet_statistics *
get_stats(struct device *dev)
{
    return (struct enet_statistics *)dev->priv;
}

/* Initialize the rest of the LOOPBACK device. */
int
loopback_init(struct device *dev)
{
  dev->mtu		= 2000;			/* MTU			*/
  dev->tbusy		= 0;
  dev->hard_start_xmit	= loopback_xmit;
  dev->open		= NULL;
#if 1
  dev->hard_header	= eth_header;
  dev->add_arp		= NULL;
  dev->hard_header_len	= ETH_HLEN;		/* 14			*/
  dev->addr_len		= ETH_ALEN;		/* 6			*/
  dev->type		= ARPHRD_ETHER;		/* 0x0001		*/
  dev->type_trans	= eth_type_trans;
  dev->rebuild_header	= eth_rebuild_header;
#else
  dev->hard_header_length = 0;
  dev->add_arp		= NULL;
  dev->addr_len		= 0;
  dev->type		= 0;			/* loopback_type (0)	*/
  dev->hard_header	= NULL;
  dev->type_trans	= NULL;
  dev->rebuild_header	= NULL;
#endif
  dev->queue_xmit	= dev_queue_xmit;

  /* New-style flags. */
  dev->flags		= IFF_LOOPBACK;
  dev->family		= AF_INET;
  dev->pa_addr		= in_aton("127.0.0.1");
  dev->pa_brdaddr	= in_aton("127.255.255.255");
  dev->pa_mask		= in_aton("255.0.0.0");
  dev->pa_alen		= sizeof(unsigned long);
  dev->priv = kmalloc(sizeof(struct enet_statistics), GFP_KERNEL);
  memset(dev->priv, 0, sizeof(struct enet_statistics));
  dev->get_stats = get_stats;
  
  return(0);
};
