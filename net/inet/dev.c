/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Interface (streams) handling functions.
 *
 * Version:	@(#)dev.c	1.0.19	05/31/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Mark Evans, <evansmp@uhura.aston.ac.uk>
 * 
 * Fixes:	
 *		Alan Cox:	check_addr returns a value for a wrong subnet
 *				ie not us but don't forward this!
 *		Alan Cox:	block timer if the inet_bh handler is running
 *		Alan Cox:	generic queue code added. A lot neater now
 *		C.E.Hawkins:	SIOCGIFCONF only reports 'upped' interfaces
 *		C.E.Hawkins:	IFF_PROMISC support
 *		Alan Cox:	Supports Donald Beckers new hardware 
 *				multicast layer, but not yet multicast lists.
 *		Alan Cox:	ip_addr_match problems with class A/B nets.
 *		C.E.Hawkins	IP 0.0.0.0 and also same net route fix. [FIXME: Ought to cause ICMP_REDIRECT]
 *		Alan Cox:	Removed bogus subnet check now the subnet code
 *				a) actually works for all A/B nets
 *				b) doesn't forward off the same interface.
 *		Alan Cox:	Multiple extra protocols
 *		Alan Cox:	Fixed ifconfig up of dud device setting the up flag
 *		Alan Cox:	Fixed verify_area errors
 *		Alan Cox:	Removed IP_SET_DEV as per Fred's comment. I hope this doesn't give
 *				anything away 8)
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#include <asm/segment.h>
#include <asm/system.h>
#include <asm/bitops.h>
#include <linux/config.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/in.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/if_ether.h>
#include "inet.h"
#include "dev.h"
#include "eth.h"
#include "ip.h"
#include "route.h"
#include "protocol.h"
#include "tcp.h"
#include "skbuff.h"
#include "sock.h"
#include "arp.h"
#ifdef CONFIG_AX25
#include "ax25.h"
#endif


#ifdef CONFIG_IPX

static struct packet_type ipx_8023_type = {
  NET16(ETH_P_802_3),
  0,
  ipx_rcv,
  NULL,
  NULL
};

static struct packet_type ipx_packet_type = {
  NET16(ETH_P_IPX),
  0,
  ipx_rcv,
  NULL,
  &ipx_8023_type
};

#endif

#ifdef CONFIG_AX25

static struct packet_type ax25_packet_type = {
  NET16(ETH_P_AX25),
  0,
  ax25_rcv,
  NULL,
#ifdef CONFIG_IPX
  &ipx_packet_type
#else
  NULL
#endif
};
#endif


static struct packet_type arp_packet_type = {
  NET16(ETH_P_ARP),
  0,		/* copy */
  arp_rcv,
  NULL,
#ifdef CONFIG_IPX
#ifndef CONFIG_AX25
  &ipx_packet_type
#else
  &ax25_packet_type
#endif
#else
#ifdef CONFIG_AX25
  &ax25_packet_type
#else
  NULL		/* next */
#endif
#endif
};


static struct packet_type ip_packet_type = {
  NET16(ETH_P_IP),
  0,		/* copy */
  ip_rcv,
  NULL,
  &arp_packet_type
};
   

struct packet_type *ptype_base = &ip_packet_type;
static struct sk_buff *volatile backlog = NULL;
static unsigned long ip_bcast = 0;


/* Return the lesser of the two values. */
static unsigned long
min(unsigned long a, unsigned long b)
{
  if (a < b) return(a);
  return(b);
}


/* Determine a default network mask, based on the IP address. */
static unsigned long
get_mask(unsigned long addr)
{
  unsigned long dst;

  if (addr == 0L) 
  	return(0L);	/* special case */

  dst = ntohl(addr);
  if (IN_CLASSA(dst)) 
  	return(htonl(IN_CLASSA_NET));
  if (IN_CLASSB(dst)) 
  	return(htonl(IN_CLASSB_NET));
  if (IN_CLASSC(dst)) 
  	return(htonl(IN_CLASSC_NET));
  
  /* Something else, probably a subnet. */
  return(0);
}


int
ip_addr_match(unsigned long me, unsigned long him)
{
  int i;
  unsigned long mask=0xFFFFFFFF;
  DPRINTF((DBG_DEV, "ip_addr_match(%s, ", in_ntoa(me)));
  DPRINTF((DBG_DEV, "%s)\n", in_ntoa(him)));

  if (me == him) 
  	return(1);
  for (i = 0; i < 4; i++, me >>= 8, him >>= 8, mask >>= 8) {
	if ((me & 0xFF) != (him & 0xFF)) {
		/*
		 * The only way this could be a match is for
		 * the rest of addr1 to be 0 or 255.
		 */
		if (me != 0 && me != mask) return(0);
		return(1);
	}
  }
  return(1);
}


/* Check the address for our address, broadcasts, etc. */
int chk_addr(unsigned long addr)
{
	struct device *dev;
	unsigned long mask;

	/* Accept both `all ones' and `all zeros' as BROADCAST. */
	if (addr == INADDR_ANY || addr == INADDR_BROADCAST)
		return IS_BROADCAST;

	mask = get_mask(addr);

	/* Accept all of the `loopback' class A net. */
	if ((addr & mask) == htonl(0x7F000000L))
		return IS_MYADDR;

	/* OK, now check the interface addresses. */
	for (dev = dev_base; dev != NULL; dev = dev->next) {
		if (!(dev->flags & IFF_UP))
			continue;
		if ((dev->pa_addr == 0)/* || (dev->flags&IFF_PROMISC)*/)
			return IS_MYADDR;
		/* Is it the exact IP address? */
		if (addr == dev->pa_addr)
			return IS_MYADDR;
		/* Is it our broadcast address? */
		if ((dev->flags & IFF_BROADCAST) && addr == dev->pa_brdaddr)
			return IS_BROADCAST;
		/* Nope. Check for a subnetwork broadcast. */
		if (((addr ^ dev->pa_addr) & dev->pa_mask) == 0) {
			if ((addr & ~dev->pa_mask) == 0)
				return IS_BROADCAST;
			if ((addr & ~dev->pa_mask) == ~dev->pa_mask)
				return IS_BROADCAST;
		}
		/* Nope. Check for Network broadcast. */
		if (((addr ^ dev->pa_addr) & mask) == 0) {
			if ((addr & ~mask) == 0)
				return IS_BROADCAST;
			if ((addr & ~mask) == ~mask)
				return IS_BROADCAST;
		}
	}
	return 0;		/* no match at all */
}


/*
 * Retrieve our own address.
 * Because the loopback address (127.0.0.1) is already recognized
 * automatically, we can use the loopback interface's address as
 * our "primary" interface.  This is the addressed used by IP et
 * al when it doesn't know which address to use (i.e. it does not
 * yet know from or to which interface to go...).
 */
unsigned long
my_addr(void)
{
  struct device *dev;

  for (dev = dev_base; dev != NULL; dev = dev->next) {
	if (dev->flags & IFF_LOOPBACK) return(dev->pa_addr);
  }
  return(0);
}


static int dev_nit=0; /* Number of network taps running */

/* Add a protocol ID to the list.  This will change soon. */
void
dev_add_pack(struct packet_type *pt)
{
  struct packet_type *p1;
  pt->next = ptype_base;

  /* Don't use copy counts on ETH_P_ALL. Instead keep a global
     count of number of these and use it and pt->copy to decide
     copies */
  pt->copy=0;
  if(pt->type==NET16(ETH_P_ALL))
  	dev_nit++;	/* I'd like a /dev/nit too one day 8) */
  else
  {
  	/* See if we need to copy it. */
  	for (p1 = ptype_base; p1 != NULL; p1 = p1->next) {
		if (p1->type == pt->type) {
			pt->copy = 1;
			break;
		}
	  }
  }
  
  /*
   *	NIT taps must go at the end or inet_bh will leak!
   */
   
  if(pt->type==NET16(ETH_P_ALL))
  {
  	pt->next=NULL;
  	if(ptype_base==NULL)
	  	ptype_base=pt;
	else
	{
		for(p1=ptype_base;p1->next!=NULL;p1=p1->next);
		p1->next=pt;
	}
  }
  else
  	ptype_base = pt;
}


/* Remove a protocol ID from the list.  This will change soon. */
void
dev_remove_pack(struct packet_type *pt)
{
  struct packet_type *lpt, *pt1;

  if (pt->type == NET16(ETH_P_ALL))
  	dev_nit--;
  if (pt == ptype_base) {
	ptype_base = pt->next;
	return;
  }

  lpt = NULL;
  for (pt1 = ptype_base; pt1->next != NULL; pt1 = pt1->next) {
	if (pt1->next == pt ) {
		cli();
		if (!pt->copy && lpt) 
			lpt->copy = 0;
		pt1->next = pt->next;
		sti();
		return;
	}

	if (pt1->next -> type == pt ->type && pt->type != NET16(ETH_P_ALL)) {
		lpt = pt1->next;
	}
  }
}


/* Find an interface in the list. This will change soon. */
struct device *
dev_get(char *name)
{
  struct device *dev;

  for (dev = dev_base; dev != NULL; dev = dev->next) {
	if (strcmp(dev->name, name) == 0) 
		return(dev);
  }
  return(NULL);
}


/* Find an interface that can handle addresses for a certain address. */
struct device * dev_check(unsigned long addr)
{
	struct device *dev;

	for (dev = dev_base; dev; dev = dev->next) {
		if (!(dev->flags & IFF_UP))
			continue;
		if (!(dev->flags & IFF_POINTOPOINT))
			continue;
		if (addr != dev->pa_dstaddr)
			continue;
		return dev;
	}
	for (dev = dev_base; dev; dev = dev->next) {
		if (!(dev->flags & IFF_UP))
			continue;
		if (dev->flags & IFF_POINTOPOINT)
			continue;
		if (dev->pa_mask & (addr ^ dev->pa_addr))
			continue;
		return dev;
	}
	return NULL;
}


/* Prepare an interface for use. */
int
dev_open(struct device *dev)
{
  int ret = 0;

  if (dev->open) 
  	ret = dev->open(dev);
  if (ret == 0) 
  	dev->flags |= (IFF_UP | IFF_RUNNING);

  return(ret);
}


/* Completely shutdown an interface. */
int
dev_close(struct device *dev)
{
  if (dev->flags != 0) {
  	int ct=0;
	dev->flags = 0;
	if (dev->stop) 
		dev->stop(dev);
	rt_flush(dev);
	dev->pa_addr = 0;
	dev->pa_dstaddr = 0;
	dev->pa_brdaddr = 0;
	dev->pa_mask = 0;
	/* Purge any queued packets when we down the link */
	while(ct<DEV_NUMBUFFS)
	{
		struct sk_buff *skb;
		while((skb=skb_dequeue(&dev->buffs[ct]))!=NULL)
			if(skb->free)
				kfree_skb(skb,FREE_WRITE);
		ct++;
	}
  }

  return(0);
}


/* Send (or queue for sending) a packet. */
void
dev_queue_xmit(struct sk_buff *skb, struct device *dev, int pri)
{
  int where = 0;		/* used to say if the packet should go	*/
				/* at the front or the back of the	*/
				/* queue.				*/

  DPRINTF((DBG_DEV, "dev_queue_xmit(skb=%X, dev=%X, pri = %d)\n",
							skb, dev, pri));

  if (dev == NULL) {
	printk("dev.c: dev_queue_xmit: dev = NULL\n");
	return;
  }
 
  IS_SKB(skb);
    
  skb->dev = dev;
  if (skb->next != NULL) {
	/* Make sure we haven't missed an interrupt. */
	dev->hard_start_xmit(NULL, dev);
	return;
  }

  if (pri < 0) {
	pri = -pri-1;
	where = 1;
  }

  if (pri >= DEV_NUMBUFFS) {
	printk("bad priority in dev_queue_xmit.\n");
	pri = 1;
  }

  if (dev->hard_start_xmit(skb, dev) == 0) {
	return;
  }

  /* Put skb into a bidirectional circular linked list. */
  DPRINTF((DBG_DEV, "dev_queue_xmit dev->buffs[%d]=%X\n",
					pri, dev->buffs[pri]));

  /* Interrupts should already be cleared by hard_start_xmit. */
  cli();
  skb->magic = DEV_QUEUE_MAGIC;
  if(where)
  	skb_queue_head(&dev->buffs[pri],skb);
  else
  	skb_queue_tail(&dev->buffs[pri],skb);
  skb->magic = DEV_QUEUE_MAGIC;
  sti();
}

/*
 * Receive a packet from a device driver and queue it for the upper
 * (protocol) levels.  It always succeeds.
 */
void
netif_rx(struct sk_buff *skb)
{
  /* Set any necessary flags. */
  skb->sk = NULL;
  skb->free = 1;
  
  /* and add it to the "backlog" queue. */
  IS_SKB(skb);
  skb_queue_tail(&backlog,skb);
   
  /* If any packet arrived, mark it for processing. */
  if (backlog != NULL) mark_bh(INET_BH);

  return;
}


/*
 * The old interface to fetch a packet from a device driver.
 * This function is the base level entry point for all drivers that
 * want to send a packet to the upper (protocol) levels.  It takes
 * care of de-multiplexing the packet to the various modules based
 * on their protocol ID.
 *
 * Return values:	1 <- exit I can't do any more
 *			0 <- feed me more (i.e. "done", "OK"). 
 */
int
dev_rint(unsigned char *buff, long len, int flags, struct device *dev)
{
  static int dropping = 0;
  struct sk_buff *skb = NULL;
  unsigned char *to;
  int amount, left;
  int len2;

  if (dev == NULL || buff == NULL || len <= 0) return(1);
  if (flags & IN_SKBUFF) {
	skb = (struct sk_buff *) buff;
  } else {
	if (dropping) {
	  if (backlog != NULL)
	      return(1);
	  printk("INET: dev_rint: no longer dropping packets.\n");
	  dropping = 0;
	}

	skb = alloc_skb(sizeof(*skb) + len, GFP_ATOMIC);
	if (skb == NULL) {
		printk("dev_rint: packet dropped on %s (no memory) !\n",
		       dev->name);
		dropping = 1;
		return(1);
	}
	skb->mem_len = sizeof(*skb) + len;
	skb->mem_addr = (struct sk_buff *) skb;

	/* First we copy the packet into a buffer, and save it for later. */
	to = skb->data;
	left = len;
	len2 = len;
	while (len2 > 0) {
		amount = min(len2, (unsigned long) dev->rmem_end -
						(unsigned long) buff);
		memcpy(to, buff, amount);
		len2 -= amount;
		left -= amount;
		buff += amount;
		to += amount;
		if ((unsigned long) buff == dev->rmem_end)
			buff = (unsigned char *) dev->rmem_start;
	}
  }
  skb->len = len;
  skb->dev = dev;
  skb->free = 1;

  netif_rx(skb);
  /* OK, all done. */
  return(0);
}


/* This routine causes all interfaces to try to send some data. */
void
dev_transmit(void)
{
  struct device *dev;

  for (dev = dev_base; dev != NULL; dev = dev->next) {
	if (!dev->tbusy) {
		dev_tint(dev);
	}
  }
}

static volatile char in_bh = 0;

int in_inet_bh()	/* Used by timer.c */
{
	return(in_bh==0?0:1);
}

/*
 * This function gets called periodically, to see if we can
 * process any data that came in from some interface.
 *
 */
void
inet_bh(void *tmp)
{
  struct sk_buff *skb;
  struct packet_type *ptype;
  unsigned short type;
  unsigned char flag = 0;
  int nitcount;

  /* Atomically check and mark our BUSY state. */
  if (set_bit(1, (void*)&in_bh))
      return;

  /* Can we send anything now? */
  dev_transmit();
  
  /* Any data left to process? */
  while((skb=skb_dequeue(&backlog))!=NULL)
  {
  	nitcount=dev_nit;
	flag=0;
	sti();
       /*
	* Bump the pointer to the next structure.
	* This assumes that the basic 'skb' pointer points to
	* the MAC header, if any (as indicated by its "length"
	* field).  Take care now!
	*/
       skb->h.raw = skb->data + skb->dev->hard_header_len;
       skb->len -= skb->dev->hard_header_len;

       /*
	* Fetch the packet protocol ID.  This is also quite ugly, as
	* it depends on the protocol driver (the interface itself) to
	* know what the type is, or where to get it from.  The Ethernet
	* interfaces fetch the ID from the two bytes in the Ethernet MAC
	* header (the h_proto field in struct ethhdr), but drivers like
	* SLIP and PLIP have no alternative but to force the type to be
	* IP or something like that.  Sigh- FvK
	*/
       type = skb->dev->type_trans(skb, skb->dev);

	/*
	 * We got a packet ID.  Now loop over the "known protocols"
	 * table (which is actually a linked list, but this will
	 * change soon if I get my way- FvK), and forward the packet
	 * to anyone who wants it.
	 */
	for (ptype = ptype_base; ptype != NULL; ptype = ptype->next) {
		if (ptype->type == type || ptype->type == NET16(ETH_P_ALL)) {
			struct sk_buff *skb2;

			if (ptype->type==NET16(ETH_P_ALL))
				nitcount--;
			if (ptype->copy || nitcount) {	/* copy if we need to	*/
				skb2 = alloc_skb(skb->mem_len, GFP_ATOMIC);
				if (skb2 == NULL) 
					continue;
				memcpy(skb2, (const void *) skb, skb->mem_len);
				skb2->mem_addr = skb2;
				skb2->h.raw = (unsigned char *)(
				    (unsigned long) skb2 +
				    (unsigned long) skb->h.raw -
				    (unsigned long) skb
				);
				skb2->free = 1;
			} else {
				skb2 = skb;
			}

			/* This used to be in the 'else' part, but then
			 * we don't have this flag set when we get a
			 * protocol that *does* require copying... -FvK
			 */
			flag = 1;

			/* Kick the protocol handler. */
			ptype->func(skb2, skb->dev, ptype);
		}
	}

	/*
	 * That's odd.  We got an unknown packet.  Who's using
	 * stuff like Novell or Amoeba on this network??
	 */
	if (!flag) {
		DPRINTF((DBG_DEV,
			"INET: unknown packet type 0x%04X (ignored)\n", type));
		skb->sk = NULL;
		kfree_skb(skb, FREE_WRITE);
	}

	/* Again, see if we can transmit anything now. */
	dev_transmit();
	cli();
  }
  in_bh = 0;
  sti();
  dev_transmit();
}


/*
 * This routine is called when an device driver (i.e. an
 * interface) is * ready to transmit a packet.
 */
 
void dev_tint(struct device *dev)
{
	int i;
	struct sk_buff *skb;
	
	for(i = 0;i < DEV_NUMBUFFS; i++) {
		while((skb=skb_dequeue(&dev->buffs[i]))!=NULL)
		{
			skb->magic = 0;
			skb->next = NULL;
			skb->prev = NULL;
			dev->queue_xmit(skb,dev,-i - 1);
			if (dev->tbusy)
				return;
		}
	}
}


/* Perform a SIOCGIFCONF call. */
static int
dev_ifconf(char *arg)
{
  struct ifconf ifc;
  struct ifreq ifr;
  struct device *dev;
  char *pos;
  int len;
  int err;

  /* Fetch the caller's info block. */
  err=verify_area(VERIFY_WRITE, arg, sizeof(struct ifconf));
  if(err)
  	return err;
  memcpy_fromfs(&ifc, arg, sizeof(struct ifconf));
  len = ifc.ifc_len;
  pos = ifc.ifc_buf;

  /* Loop over the interfaces, and write an info block for each. */
  for (dev = dev_base; dev != NULL; dev = dev->next) {
        if(!(dev->flags & IFF_UP))
        	continue;
	memset(&ifr, 0, sizeof(struct ifreq));
	strcpy(ifr.ifr_name, dev->name);
	(*(struct sockaddr_in *) &ifr.ifr_addr).sin_family = dev->family;
	(*(struct sockaddr_in *) &ifr.ifr_addr).sin_addr.s_addr = dev->pa_addr;

	/* Write this block to the caller's space. */
	memcpy_tofs(pos, &ifr, sizeof(struct ifreq));
	pos += sizeof(struct ifreq);
	len -= sizeof(struct ifreq);
	if (len < sizeof(struct ifreq)) break;
  }

  /* All done.  Write the updated control block back to the caller. */
  ifc.ifc_len = (pos - ifc.ifc_buf);
  ifc.ifc_req = (struct ifreq *) ifc.ifc_buf;
  memcpy_tofs(arg, &ifc, sizeof(struct ifconf));
  return(pos - arg);
}

/* Print device statistics. */
char *sprintf_stats(char *buffer, struct device *dev)
{
  char *pos = buffer;
  struct enet_statistics *stats = (dev->get_stats ? dev->get_stats(dev): NULL);

  if (stats)
    pos += sprintf(pos, "%6s:%7d %4d %4d %4d %4d %8d %4d %4d %4d %5d %4d\n",
		   dev->name,
		   stats->rx_packets, stats->rx_errors,
		   stats->rx_dropped + stats->rx_missed_errors,
		   stats->rx_fifo_errors,
		   stats->rx_length_errors + stats->rx_over_errors
		   + stats->rx_crc_errors + stats->rx_frame_errors,
		   stats->tx_packets, stats->tx_errors, stats->tx_dropped,
		   stats->tx_fifo_errors, stats->collisions,
		   stats->tx_carrier_errors + stats->tx_aborted_errors
		   + stats->tx_window_errors + stats->tx_heartbeat_errors);
  else
      pos += sprintf(pos, "%6s: No statistics available.\n", dev->name);

  return pos;
}

/* Called from the PROCfs module. */
int
dev_get_info(char *buffer)
{
  char *pos = buffer;
  struct device *dev;

  pos +=
      sprintf(pos,
	      "Inter-|   Receive                  |  Transmit\n"
	      " face |packets errs drop fifo frame|packets errs drop fifo colls carrier\n");
  for (dev = dev_base; dev != NULL; dev = dev->next) {
      pos = sprintf_stats(pos, dev);
  }
  return pos - buffer;
}

static inline int bad_mask(unsigned long mask, unsigned long addr)
{
	if (addr & (mask = ~mask))
		return 1;
	mask = ntohl(mask);
	if (mask & (mask+1))
		return 1;
	return 0;
}


/* Perform the SIOCxIFxxx calls. */
static int
dev_ifsioc(void *arg, unsigned int getset)
{
  struct ifreq ifr;
  struct device *dev;
  int ret;

  /* Fetch the caller's info block. */
  int err=verify_area(VERIFY_WRITE, arg, sizeof(struct ifreq));
  if(err)
  	return err;
  memcpy_fromfs(&ifr, arg, sizeof(struct ifreq));

  /* See which interface the caller is talking about. */
  if ((dev = dev_get(ifr.ifr_name)) == NULL) return(-EINVAL);

  switch(getset) {
	case SIOCGIFFLAGS:
		ifr.ifr_flags = dev->flags;
		memcpy_tofs(arg, &ifr, sizeof(struct ifreq));
		ret = 0;
		break;
	case SIOCSIFFLAGS:
		{
		  int old_flags = dev->flags;
		  dev->flags = ifr.ifr_flags & (
			IFF_UP | IFF_BROADCAST | IFF_DEBUG | IFF_LOOPBACK |
			IFF_POINTOPOINT | IFF_NOTRAILERS | IFF_RUNNING |
			IFF_NOARP | IFF_PROMISC | IFF_ALLMULTI);
			
		  if ( (old_flags & IFF_PROMISC) && ((dev->flags & IFF_PROMISC) == 0))
		  	dev->set_multicast_list(dev,0,NULL);
		  if ( (dev->flags & IFF_PROMISC) && ((old_flags & IFF_PROMISC) == 0))
		  	dev->set_multicast_list(dev,-1,NULL);
		  if ((old_flags & IFF_UP) && ((dev->flags & IFF_UP) == 0)) {
			ret = dev_close(dev);
		  } else
		  {
		      ret = (! (old_flags & IFF_UP) && (dev->flags & IFF_UP))
			? dev_open(dev) : 0;
		      if(ret<0)
		      	dev->flags&=~IFF_UP;	/* Didnt open so down the if */
		  }
	        }
		break;
	case SIOCGIFADDR:
		(*(struct sockaddr_in *)
		  &ifr.ifr_addr).sin_addr.s_addr = dev->pa_addr;
		(*(struct sockaddr_in *)
		  &ifr.ifr_addr).sin_family = dev->family;
		(*(struct sockaddr_in *)
		  &ifr.ifr_addr).sin_port = 0;
		memcpy_tofs(arg, &ifr, sizeof(struct ifreq));
		ret = 0;
		break;
	case SIOCSIFADDR:
		dev->pa_addr = (*(struct sockaddr_in *)
				 &ifr.ifr_addr).sin_addr.s_addr;
		dev->family = ifr.ifr_addr.sa_family;
		dev->pa_mask = get_mask(dev->pa_addr);
		dev->pa_brdaddr = dev->pa_addr | ~dev->pa_mask;
		ret = 0;
		break;
	case SIOCGIFBRDADDR:
		(*(struct sockaddr_in *)
		  &ifr.ifr_broadaddr).sin_addr.s_addr = dev->pa_brdaddr;
		(*(struct sockaddr_in *)
		  &ifr.ifr_broadaddr).sin_family = dev->family;
		(*(struct sockaddr_in *)
		  &ifr.ifr_broadaddr).sin_port = 0;
		memcpy_tofs(arg, &ifr, sizeof(struct ifreq));
		ret = 0;
		break;
	case SIOCSIFBRDADDR:
		dev->pa_brdaddr = (*(struct sockaddr_in *)
				    &ifr.ifr_broadaddr).sin_addr.s_addr;
		ret = 0;
		break;
	case SIOCGIFDSTADDR:
		(*(struct sockaddr_in *)
		  &ifr.ifr_dstaddr).sin_addr.s_addr = dev->pa_dstaddr;
		(*(struct sockaddr_in *)
		  &ifr.ifr_broadaddr).sin_family = dev->family;
		(*(struct sockaddr_in *)
		  &ifr.ifr_broadaddr).sin_port = 0;
		memcpy_tofs(arg, &ifr, sizeof(struct ifreq));
		ret = 0;
		break;
	case SIOCSIFDSTADDR:
		dev->pa_dstaddr = (*(struct sockaddr_in *)
				    &ifr.ifr_dstaddr).sin_addr.s_addr;
		ret = 0;
		break;
	case SIOCGIFNETMASK:
		(*(struct sockaddr_in *)
		  &ifr.ifr_netmask).sin_addr.s_addr = dev->pa_mask;
		(*(struct sockaddr_in *)
		  &ifr.ifr_netmask).sin_family = dev->family;
		(*(struct sockaddr_in *)
		  &ifr.ifr_netmask).sin_port = 0;
		memcpy_tofs(arg, &ifr, sizeof(struct ifreq));
		ret = 0;
		break;
	case SIOCSIFNETMASK: {
		unsigned long mask = (*(struct sockaddr_in *)
			&ifr.ifr_netmask).sin_addr.s_addr;
		ret = -EINVAL;
		if (bad_mask(mask,0))
			break;
		dev->pa_mask = mask;
		ret = 0;
		break;
	}
	case SIOCGIFMETRIC:
		ifr.ifr_metric = dev->metric;
		memcpy_tofs(arg, &ifr, sizeof(struct ifreq));
		ret = 0;
		break;
	case SIOCSIFMETRIC:
		dev->metric = ifr.ifr_metric;
		ret = 0;
		break;
	case SIOCGIFMTU:
		ifr.ifr_mtu = dev->mtu;
		memcpy_tofs(arg, &ifr, sizeof(struct ifreq));
		ret = 0;
		break;
	case SIOCSIFMTU:
		dev->mtu = ifr.ifr_mtu;
		ret = 0;
		break;
	case SIOCGIFMEM:
		printk("NET: ioctl(SIOCGIFMEM, 0x%08X)\n", (int)arg);
		ret = -EINVAL;
		break;
	case SIOCSIFMEM:
		printk("NET: ioctl(SIOCSIFMEM, 0x%08X)\n", (int)arg);
		ret = -EINVAL;
		break;
	case SIOCGIFHWADDR:
		memcpy(ifr.ifr_hwaddr,dev->dev_addr, MAX_ADDR_LEN);
		memcpy_tofs(arg,&ifr,sizeof(struct ifreq));
		ret=0;
		break;
	default:
		ret = -EINVAL;
  }
  return(ret);
}


/* This function handles all "interface"-type I/O control requests. */
int
dev_ioctl(unsigned int cmd, void *arg)
{
  struct iflink iflink;
  struct ddi_device *dev;

  switch(cmd) {
	case IP_SET_DEV:
		printk("Your network configuration program needs upgrading.\n");
		return -EINVAL;

	case SIOCGIFCONF:
		(void) dev_ifconf((char *) arg);
		return 0;

	case SIOCGIFFLAGS:
	case SIOCGIFADDR:
	case SIOCGIFDSTADDR:
	case SIOCGIFBRDADDR:
	case SIOCGIFNETMASK:
	case SIOCGIFMETRIC:
	case SIOCGIFMTU:
	case SIOCGIFMEM:
	case SIOCGIFHWADDR:
		return dev_ifsioc(arg, cmd);

	case SIOCSIFFLAGS:
	case SIOCSIFADDR:
	case SIOCSIFDSTADDR:
	case SIOCSIFBRDADDR:
	case SIOCSIFNETMASK:
	case SIOCSIFMETRIC:
	case SIOCSIFMTU:
	case SIOCSIFMEM:
		if (!suser())
			return -EPERM;
		return dev_ifsioc(arg, cmd);

	case SIOCSIFLINK:
		if (!suser())
			return -EPERM;
		memcpy_fromfs(&iflink, arg, sizeof(iflink));
		dev = ddi_map(iflink.id);
		if (dev == NULL)
			return -EINVAL;

		/* Now allocate an interface and connect it. */
		printk("AF_INET: DDI \"%s\" linked to stream \"%s\"\n",
						dev->name, iflink.stream);
		return 0;

	default:
		return -EINVAL;
  }
}


/* Initialize the DEV module. */
void
dev_init(void)
{
  struct device *dev, *dev2;

  /* Add the devices.
   * If the call to dev->init fails, the dev is removed
   * from the chain disconnecting the device until the
   * next reboot.
   */
  dev2 = NULL;
  for (dev = dev_base; dev != NULL; dev=dev->next) {
	if (dev->init && dev->init(dev)) {
		if (dev2 == NULL) dev_base = dev->next;
		  else dev2->next = dev->next;
	} else {
		dev2 = dev;
	}
  }

  /* Set up some IP addresses. */
  ip_bcast = in_aton("255.255.255.255");
}
