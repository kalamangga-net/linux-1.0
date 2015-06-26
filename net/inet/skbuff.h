/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions for the 'struct sk_buff' memory handlers.
 *
 * Version:	@(#)skbuff.h	1.0.4	05/20/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Corey Minyard <wf-rch!minyard@relay.EU.net>
 *
 * Fixes:
 *		Alan Cox		: 	Volatiles (this makes me unhappy - we want proper asm linked list stuff)
 *		Alan Cox		:	Declaration for new primitives
 *		Alan Cox		:	Fraglist support (idea by Donald Becker)
 *		Alan Cox		:	'users' counter. Combines with datagram changes to avoid skb_peek_copy
 *						being used.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#ifndef _SKBUFF_H
#define _SKBUFF_H
#include <linux/malloc.h>

#ifdef CONFIG_IPX
#include "ipx.h"
#endif

#define HAVE_ALLOC_SKB		/* For the drivers to know */


#define FREE_READ	1
#define FREE_WRITE	0


struct sk_buff {
  unsigned long			magic_debug_cookie;
  struct sk_buff		*volatile next;
  struct sk_buff		*volatile prev;
  struct sk_buff		*volatile link3;
  struct sk_buff		*volatile* list;
  struct sock			*sk;
  volatile unsigned long	when;	/* used to compute rtt's	*/
  struct device			*dev;
  void				*mem_addr;
  union {
	struct tcphdr	*th;
	struct ethhdr	*eth;
	struct iphdr	*iph;
	struct udphdr	*uh;
	struct arphdr	*arp;
	unsigned char	*raw;
	unsigned long	seq;
#ifdef CONFIG_IPX	
	ipx_packet	*ipx;
#endif	
  } h;
  struct iphdr		*ip_hdr;		/* For IPPROTO_RAW */
  unsigned long			mem_len;
  unsigned long 		len;
  unsigned long			fraglen;
  struct sk_buff		*fraglist;	/* Fragment list */
  unsigned long			truesize;
  unsigned long 		saddr;
  unsigned long 		daddr;
  int				magic;
  volatile char 		acked,
				used,
				free,
				arp;
  unsigned char			tries,lock;	/* Lock is now unused */
  unsigned short		users;		/* User count - see datagram.c (and soon seqpacket.c/stream.c) */
  unsigned long			padding[0];
  unsigned char			data[0];
};

#define SK_WMEM_MAX	8192
#define SK_RMEM_MAX	32767

#define SK_FREED_SKB	0x0DE2C0DE
#define SK_GOOD_SKB	0xDEC0DED1

extern void			print_skb(struct sk_buff *);
extern void			kfree_skb(struct sk_buff *skb, int rw);
extern void			skb_queue_head(struct sk_buff * volatile *list,struct sk_buff *buf);
extern void			skb_queue_tail(struct sk_buff * volatile *list,struct sk_buff *buf);
extern struct sk_buff *		skb_dequeue(struct sk_buff * volatile *list);
extern void 			skb_insert(struct sk_buff *old,struct sk_buff *newsk);
extern void			skb_append(struct sk_buff *old,struct sk_buff *newsk);
extern void			skb_unlink(struct sk_buff *buf);
extern void 			skb_new_list_head(struct sk_buff *volatile* list);
extern struct sk_buff *		skb_peek(struct sk_buff * volatile *list);
extern struct sk_buff *		skb_peek_copy(struct sk_buff * volatile *list);
extern struct sk_buff *		alloc_skb(unsigned int size, int priority);
extern void			kfree_skbmem(void *mem, unsigned size);
extern void			skb_kept_by_device(struct sk_buff *skb);
extern void			skb_device_release(struct sk_buff *skb, int mode);
extern int			skb_device_locked(struct sk_buff *skb);
extern void 			skb_check(struct sk_buff *skb,int, char *);
#define IS_SKB(skb)	skb_check((skb),__LINE__,__FILE__)

extern struct sk_buff *		skb_recv_datagram(struct sock *sk,unsigned flags,int noblock, int *err);
extern int			datagram_select(struct sock *sk, int sel_type, select_table *wait);
extern void			skb_copy_datagram(struct sk_buff *from, int offset, char *to,int size);
extern void			skb_free_datagram(struct sk_buff *skb);
#endif	/* _SKBUFF_H */
