/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions for the ARP protocol module.
 *
 * Version:	@(#)arp.h	1.0.6	05/21/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#ifndef _ARP_H
#define _ARP_H

#define ARP_TABLE_SIZE	16		/* size of ARP table		*/
#define ARP_TIMEOUT	30000		/* five minutes			*/
#define ARP_RES_TIME	250		/* 2.5 seconds			*/

#define ARP_MAX_TRIES	3		/* max # of tries to send ARP	*/
#define ARP_QUEUE_MAGIC	0x0432447A	/* magic # for queues		*/


/* This structure defines the ARP mapping cache. */
struct arp_table {
  struct arp_table		*next;
  volatile unsigned long	last_used;
  unsigned int			flags;
#if 1
  unsigned long			ip;
#else
  unsigned char			pa[MAX_ADDR_LEN];
  unsigned char			plen;
  unsigned char			ptype;
#endif
  unsigned char			ha[MAX_ADDR_LEN];
  unsigned char			hlen;
  unsigned char			htype;
};


/* This is also used in "sock.c" and "tcp.c" - YUCK! - FvK */
extern struct sk_buff *arp_q;


extern void	arp_destroy(unsigned long paddr);
extern int	arp_rcv(struct sk_buff *skb, struct device *dev,
			struct packet_type *pt);
extern int	arp_find(unsigned char *haddr, unsigned long paddr,
			 struct device *dev, unsigned long saddr);
extern void	arp_add(unsigned long addr, unsigned char *haddr,
			struct device *dev);
extern void	arp_add_broad(unsigned long addr, struct device *dev);
extern void	arp_queue(struct sk_buff *skb);
extern int	arp_get_info(char *buffer);
extern int	arp_ioctl(unsigned int cmd, void *arg);
extern void	arp_destroy_maybe(unsigned long paddr);

#endif	/* _ARP_H */
