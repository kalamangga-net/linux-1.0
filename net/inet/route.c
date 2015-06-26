/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		ROUTE - implementation of the IP router.
 *
 * Version:	@(#)route.c	1.0.14	05/31/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 * Fixes:
 *		Alan Cox	:	Verify area fixes.
 *		Alan Cox	:	cli() protects routing changes
 *		Rui Oliveira	:	ICMP routing table updates
 *		(rco@di.uminho.pt)	Routing table insertion and update
 *		Linus Torvalds	:	Rewrote bits to be sensible
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#include <asm/segment.h>
#include <asm/system.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/errno.h>
#include <linux/in.h>
#include "inet.h"
#include "dev.h"
#include "ip.h"
#include "protocol.h"
#include "route.h"
#include "tcp.h"
#include "skbuff.h"
#include "sock.h"
#include "arp.h"
#include "icmp.h"


static struct rtable *rt_base = NULL;
static struct rtable *rt_loopback = NULL;

/* Dump the contents of a routing table entry. */
static void
rt_print(struct rtable *rt)
{
  if (rt == NULL || inet_debug != DBG_RT) return;

  printk("RT: %06lx NXT=%06lx FLAGS=0x%02x\n",
		(long) rt, (long) rt->rt_next, rt->rt_flags);
  printk("    TARGET=%s ", in_ntoa(rt->rt_dst));
  printk("GW=%s ", in_ntoa(rt->rt_gateway));
  printk("    DEV=%s USE=%ld REF=%d\n",
	(rt->rt_dev == NULL) ? "NONE" : rt->rt_dev->name,
	rt->rt_use, rt->rt_refcnt);
}


/*
 * Remove a routing table entry.
 */
static void rt_del(unsigned long dst)
{
	struct rtable *r, **rp;
	unsigned long flags;

	DPRINTF((DBG_RT, "RT: flushing for dst %s\n", in_ntoa(dst)));
	rp = &rt_base;
	save_flags(flags);
	cli();
	while((r = *rp) != NULL) {
		if (r->rt_dst != dst) {
			rp = &r->rt_next;
			continue;
		}
		*rp = r->rt_next;
		if (rt_loopback == r)
			rt_loopback = NULL;
		kfree_s(r, sizeof(struct rtable));
	} 
	restore_flags(flags);
}


/*
 * Remove all routing table entries for a device.
 */
void rt_flush(struct device *dev)
{
	struct rtable *r;
	struct rtable **rp;
	unsigned long flags;

	DPRINTF((DBG_RT, "RT: flushing for dev 0x%08lx (%s)\n", (long)dev, dev->name));
	rp = &rt_base;
	cli();
	save_flags(flags);
	while ((r = *rp) != NULL) {
		if (r->rt_dev != dev) {
			rp = &r->rt_next;
			continue;
		}
		*rp = r->rt_next;
		if (rt_loopback == r)
			rt_loopback = NULL;
		kfree_s(r, sizeof(struct rtable));
	} 
	restore_flags(flags);
}

/*
 * Used by 'rt_add()' when we can't get the netmask any other way..
 *
 * If the lower byte or two are zero, we guess the mask based on the
 * number of zero 8-bit net numbers, otherwise we use the "default"
 * masks judging by the destination address and our device netmask.
 */
static inline unsigned long default_mask(unsigned long dst)
{
	dst = ntohl(dst);
	if (IN_CLASSA(dst))
		return htonl(IN_CLASSA_NET);
	if (IN_CLASSB(dst))
		return htonl(IN_CLASSB_NET);
	return htonl(IN_CLASSC_NET);
}

static unsigned long guess_mask(unsigned long dst, struct device * dev)
{
	unsigned long mask;

	if (!dst)
		return 0;
	mask = default_mask(dst);
	if ((dst ^ dev->pa_addr) & mask)
		return mask;
	return dev->pa_mask;
}

static inline struct device * get_gw_dev(unsigned long gw)
{
	struct rtable * rt;

	for (rt = rt_base ; ; rt = rt->rt_next) {
		if (!rt)
			return NULL;
		if ((gw ^ rt->rt_dst) & rt->rt_mask)
			continue;
		/* gateways behind gateways are a no-no */
		if (rt->rt_flags & RTF_GATEWAY)
			return NULL;
		return rt->rt_dev;
	}
}

/*
 * rewrote rt_add(), as the old one was weird. Linus
 */
void rt_add(short flags, unsigned long dst, unsigned long mask,
	unsigned long gw, struct device *dev)
{
	struct rtable *r, *rt;
	struct rtable **rp;
	unsigned long cpuflags;

	if (flags & RTF_HOST) {
		mask = 0xffffffff;
	} else if (!mask) {
		if (!((dst ^ dev->pa_addr) & dev->pa_mask)) {
			mask = dev->pa_mask;
			flags &= ~RTF_GATEWAY;
			if (flags & RTF_DYNAMIC) {
				/*printk("Dynamic route to my own net rejected\n");*/
				return;
			}
		} else
			mask = guess_mask(dst, dev);
		dst &= mask;
	}
	if (gw == dev->pa_addr)
		flags &= ~RTF_GATEWAY;
	if (flags & RTF_GATEWAY) {
		/* don't try to add a gateway we can't reach.. */
		if (dev != get_gw_dev(gw))
			return;
		flags |= RTF_GATEWAY;
	} else
		gw = 0;
	/* Allocate an entry. */
	rt = (struct rtable *) kmalloc(sizeof(struct rtable), GFP_ATOMIC);
	if (rt == NULL) {
		DPRINTF((DBG_RT, "RT: no memory for new route!\n"));
		return;
	}
	memset(rt, 0, sizeof(struct rtable));
	rt->rt_flags = flags | RTF_UP;
	rt->rt_dst = dst;
	rt->rt_dev = dev;
	rt->rt_gateway = gw;
	rt->rt_mask = mask;
	rt->rt_mtu = dev->mtu;
	rt_print(rt);
	/*
	 * What we have to do is loop though this until we have
	 * found the first address which has a higher generality than
	 * the one in rt.  Then we can put rt in right before it.
	 */
	save_flags(cpuflags);
	cli();
	/* remove old route if we are getting a duplicate. */
	rp = &rt_base;
	while ((r = *rp) != NULL) {
		if (r->rt_dst != dst) {
			rp = &r->rt_next;
			continue;
		}
		*rp = r->rt_next;
		if (rt_loopback == r)
			rt_loopback = NULL;
		kfree_s(r, sizeof(struct rtable));
	}
	/* add the new route */
	rp = &rt_base;
	while ((r = *rp) != NULL) {
		if ((r->rt_mask & mask) != mask)
			break;
		rp = &r->rt_next;
	}
	rt->rt_next = r;
	*rp = rt;
	if (rt->rt_dev->flags & IFF_LOOPBACK)
		rt_loopback = rt;
	restore_flags(cpuflags);
	return;
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

static int rt_new(struct rtentry *r)
{
	int err;
	char * devname;
	struct device * dev = NULL;
	unsigned long flags, daddr, mask, gw;

	if ((devname = r->rt_dev) != NULL) {
		err = getname(devname, &devname);
		if (err)
			return err;
		dev = dev_get(devname);
		putname(devname);
		if (!dev)
			return -EINVAL;
	}

	if (r->rt_dst.sa_family != AF_INET)
		return -EAFNOSUPPORT;

	flags = r->rt_flags;
	daddr = ((struct sockaddr_in *) &r->rt_dst)->sin_addr.s_addr;
	mask = ((struct sockaddr_in *) &r->rt_genmask)->sin_addr.s_addr;
	gw = ((struct sockaddr_in *) &r->rt_gateway)->sin_addr.s_addr;

/* BSD emulation: Permits route add someroute gw one-of-my-addresses
   to indicate which iface. Not as clean as the nice Linux dev technique
   but people keep using it... */
	if (!dev && (flags & RTF_GATEWAY)) {
		struct device *dev2;
		for (dev2 = dev_base ; dev2 != NULL ; dev2 = dev2->next) {
			if ((dev2->flags & IFF_UP) && dev2->pa_addr == gw) {
				flags &= ~RTF_GATEWAY;
				dev = dev2;
				break;
			}
		}
	}

	if (bad_mask(mask, daddr))
		mask = 0;

	if (flags & RTF_HOST)
		mask = 0xffffffff;
	else if (mask && r->rt_genmask.sa_family != AF_INET)
		return -EAFNOSUPPORT;

	if (flags & RTF_GATEWAY) {
		if (r->rt_gateway.sa_family != AF_INET)
			return -EAFNOSUPPORT;
		if (!dev)
			dev = get_gw_dev(gw);
	} else if (!dev)
		dev = dev_check(daddr);

	if (dev == NULL)
		return -ENETUNREACH;

	rt_add(flags, daddr, mask, gw, dev);
	return 0;
}


static int rt_kill(struct rtentry *r)
{
	struct sockaddr_in *trg;

	trg = (struct sockaddr_in *) &r->rt_dst;
	rt_del(trg->sin_addr.s_addr);
	return 0;
}


/* Called from the PROCfs module. */
int
rt_get_info(char *buffer)
{
  struct rtable *r;
  char *pos;

  pos = buffer;

  pos += sprintf(pos,
		 "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\n");
  
  /* This isn't quite right -- r->rt_dst is a struct! */
  for (r = rt_base; r != NULL; r = r->rt_next) {
        pos += sprintf(pos, "%s\t%08lX\t%08lX\t%02X\t%d\t%lu\t%d\t%08lX\n",
		r->rt_dev->name, r->rt_dst, r->rt_gateway,
		r->rt_flags, r->rt_refcnt, r->rt_use, r->rt_metric,
		r->rt_mask);
  }
  return(pos - buffer);
}

/*
 * This is hackish, but results in better code. Use "-S" to see why.
 */
#define early_out ({ goto no_route; 1; })

struct rtable * rt_route(unsigned long daddr, struct options *opt)
{
	struct rtable *rt;

	for (rt = rt_base; rt != NULL || early_out ; rt = rt->rt_next) {
		if (!((rt->rt_dst ^ daddr) & rt->rt_mask))
			break;
		/* broadcast addresses can be special cases.. */
		if ((rt->rt_dev->flags & IFF_BROADCAST) &&
		     rt->rt_dev->pa_brdaddr == daddr)
			break;
	}
	if (daddr == rt->rt_dev->pa_addr) {
		if ((rt = rt_loopback) == NULL)
			goto no_route;
	}
	rt->rt_use++;
	return rt;
no_route:
	return NULL;
}

static int get_old_rtent(struct old_rtentry * src, struct rtentry * rt)
{
	int err;
	struct old_rtentry tmp;

	err=verify_area(VERIFY_READ, src, sizeof(*src));
	if (err)
		return err;
	memcpy_fromfs(&tmp, src, sizeof(*src));
	memset(rt, 0, sizeof(*rt));
	rt->rt_dst = tmp.rt_dst;
	rt->rt_gateway = tmp.rt_gateway;
	rt->rt_genmask.sa_family = AF_INET;
	((struct sockaddr_in *) &rt->rt_genmask)->sin_addr.s_addr = tmp.rt_genmask;
	rt->rt_flags = tmp.rt_flags;
	rt->rt_dev = tmp.rt_dev;
	return 0;
}

int rt_ioctl(unsigned int cmd, void *arg)
{
	int err;
	struct rtentry rt;

	switch(cmd) {
	case DDIOCSDBG:
		return dbg_ioctl(arg, DBG_RT);
	case SIOCADDRTOLD:
	case SIOCDELRTOLD:
		if (!suser())
			return -EPERM;
		err = get_old_rtent((struct old_rtentry *) arg, &rt);
		if (err)
			return err;
		return (cmd == SIOCDELRTOLD) ? rt_kill(&rt) : rt_new(&rt);
	case SIOCADDRT:
	case SIOCDELRT:
		if (!suser())
			return -EPERM;
		err=verify_area(VERIFY_READ, arg, sizeof(struct rtentry));
		if (err)
			return err;
		memcpy_fromfs(&rt, arg, sizeof(struct rtentry));
		return (cmd == SIOCDELRT) ? rt_kill(&rt) : rt_new(&rt);
	}

	return -EINVAL;
}
