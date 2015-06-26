/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions for the IP module.
 *
 * Version:	@(#)ip.h	1.0.2	05/07/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#ifndef _IP_H
#define _IP_H


#include <linux/ip.h>


#include "sock.h"	/* struct sock */

/* IP flags. */
#define IP_CE		0x8000		/* Flag: "Congestion"		*/
#define IP_DF		0x4000		/* Flag: "Don't Fragment"	*/
#define IP_MF		0x2000		/* Flag: "More Fragments"	*/
#define IP_OFFSET	0x1FFF		/* "Fragment Offset" part	*/

#define IP_FRAG_TIME	(30 * HZ)		/* fragment lifetime	*/


/* Describe an IP fragment. */
struct ipfrag {
  int		offset;		/* offset of fragment in IP datagram	*/
  int		end;		/* last byte of data in datagram	*/
  int		len;		/* length of this fragment		*/
  struct sk_buff *skb;			/* complete received fragment		*/
  unsigned char		*ptr;		/* pointer into real fragment data	*/
  struct ipfrag		*next;		/* linked list pointers			*/
  struct ipfrag		*prev;
};

/* Describe an entry in the "incomplete datagrams" queue. */
struct ipq	 {
  unsigned char		*mac;		/* pointer to MAC header		*/
  struct iphdr	*iph;		/* pointer to IP header			*/
  int		len;		/* total length of original datagram	*/
  short			ihlen;		/* length of the IP header		*/
  short 	maclen;		/* length of the MAC header		*/
  struct timer_list timer;	/* when will this queue expire?		*/
  struct ipfrag		*fragments;	/* linked list of received fragments	*/
  struct ipq	*next;		/* linked list pointers			*/
  struct ipq	*prev;
  struct device *dev;		/* Device - for icmp replies */
};


extern int		backoff(int n);

extern void		ip_print(struct iphdr *ip);
extern int		ip_ioctl(struct sock *sk, int cmd,
				 unsigned long arg);
extern void		ip_route_check(unsigned long daddr);
extern int		ip_build_header(struct sk_buff *skb,
					unsigned long saddr,
					unsigned long daddr,
					struct device **dev, int type,
					struct options *opt, int len,
					int tos,int ttl);
extern unsigned short	ip_compute_csum(unsigned char * buff, int len);
extern int		ip_rcv(struct sk_buff *skb, struct device *dev,
			       struct packet_type *pt);
extern void		ip_queue_xmit(struct sock *sk,
				      struct device *dev, struct sk_buff *skb,
				      int free);
extern void		ip_retransmit(struct sock *sk, int all);
extern void		ip_do_retransmit(struct sock *sk, int all);
extern int 		ip_setsockopt(struct sock *sk, int level, int optname, char *optval, int optlen);
extern int 		ip_getsockopt(struct sock *sk, int level, int optname, char *optval, int *optlen);

#endif	/* _IP_H */
