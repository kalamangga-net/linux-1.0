/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions for the Interfaces handler.
 *
 * Version:	@(#)dev.h	1.0.10	08/12/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Corey Minyard <wf-rch!minyard@relay.EU.net>
 *		Donald J. Becker, <becker@super.org>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#ifndef _DEV_H
#define _DEV_H

#include <linux/if.h>
#include <linux/if_ether.h>


/* for future expansion when we will have different priorities. */
#define DEV_NUMBUFFS	3
#define MAX_ADDR_LEN	7
#define MAX_HEADER	18

#define IS_MYADDR	1		/* address is (one of) our own	*/
#define IS_LOOPBACK	2		/* address is for LOOPBACK	*/
#define IS_BROADCAST	3		/* address is a valid broadcast	*/
#define IS_INVBCAST	4		/* Wrong netmask bcast not for us */

/*
 * The DEVICE structure.
 * Actually, this whole structure is a big mistake.  It mixes I/O
 * data with strictly "high-level" data, and it has to know about
 * almost every data structure used in the INET module.  We will
 * gradually phase out this structure, and replace it with the
 * more general (but stolen :-) BSD "ifnet" structure. -FvK
 */
struct device {

  /*
   * This is the first field of the "visible" part of this structure
   * (i.e. as seen by users in the "Space.c" file).  It is the name
   * the interface.
   */
  char			  *name;

  /* I/O specific fields.  These will be moved to DDI soon. */
  unsigned long		  rmem_end;		/* shmem "recv" end	*/
  unsigned long		  rmem_start;		/* shmem "recv" start	*/
  unsigned long		  mem_end;		/* sahared mem end	*/
  unsigned long		  mem_start;		/* shared mem start	*/
  unsigned short	  base_addr;		/* device I/O address	*/
  unsigned char		  irq;			/* device IRQ number	*/

  /* Low-level status flags. */
  volatile unsigned char  start,		/* start an operation	*/
                          tbusy,		/* transmitter busy	*/
                          interrupt;		/* interrupt arrived	*/

  /*
   * Another mistake.
   * This points to the next device in the "dev" chain. It will
   * be moved to the "invisible" part of the structure as soon as
   * it has been cleaned up. -FvK
   */
  struct device		  *next;

  /* The device initialization function. Called only once. */
  int			  (*init)(struct device *dev);

  /* Some hardware also needs these fields, but they are not part of the
     usual set specified in Space.c. */
  unsigned char		  if_port;		/* Selectable AUI, TP,..*/
  unsigned char		  dma;			/* DMA channel		*/

  struct enet_statistics* (*get_stats)(struct device *dev);

  /*
   * This marks the end of the "visible" part of the structure. All
   * fields hereafter are internal to the system, and may change at
   * will (read: may be cleaned up at will).
   */

  /* These may be needed for future network-power-down code. */
  unsigned long		  trans_start;	/* Time (in jiffies) of last Tx	*/
  unsigned long		  last_rx;	/* Time of last Rx		*/

  unsigned short	  flags;	/* interface flags (a la BSD)	*/
  unsigned short	  family;	/* address family ID (AF_INET)	*/
  unsigned short	  metric;	/* routing metric (not used)	*/
  unsigned short	  mtu;		/* interface MTU value		*/
  unsigned short	  type;		/* interface hardware type	*/
  unsigned short	  hard_header_len;	/* hardware hdr length	*/
  void			  *priv;	/* pointer to private data	*/

  /* Interface address info. */
  unsigned char		  broadcast[MAX_ADDR_LEN];	/* hw bcast add	*/
  unsigned char		  dev_addr[MAX_ADDR_LEN];	/* hw address	*/
  unsigned char		  addr_len;	/* harfware address length	*/
  unsigned long		  pa_addr;	/* protocol address		*/
  unsigned long		  pa_brdaddr;	/* protocol broadcast addr	*/
  unsigned long		  pa_dstaddr;	/* protocol P-P other side addr	*/
  unsigned long		  pa_mask;	/* protocol netmask		*/
  unsigned short	  pa_alen;	/* protocol address length	*/

  /* Pointer to the interface buffers. */
  struct sk_buff	  *volatile buffs[DEV_NUMBUFFS];

  /* Pointers to interface service routines. */
  int			  (*open)(struct device *dev);
  int			  (*stop)(struct device *dev);
  int			  (*hard_start_xmit) (struct sk_buff *skb,
					      struct device *dev);
  int			  (*hard_header) (unsigned char *buff,
					  struct device *dev,
					  unsigned short type,
					  unsigned long daddr,
					  unsigned long saddr,
					  unsigned len);
  void			  (*add_arp) (unsigned long addr,
				      struct sk_buff *skb,
				      struct device *dev);
  void			  (*queue_xmit)(struct sk_buff *skb,
					struct device *dev, int pri);
  int			  (*rebuild_header)(void *eth, struct device *dev);
  unsigned short	  (*type_trans) (struct sk_buff *skb,
					 struct device *dev);
#define HAVE_MULTICAST			 
  void			  (*set_multicast_list)(struct device *dev,
  					 int num_addrs, void *addrs);
#define HAVE_SET_MAC_ADDR  		 
  int			  (*set_mac_address)(struct device *dev, void *addr);
};


struct packet_type {
  unsigned short	type;	/* This is really NET16(ether_type) other
				 * devices will have to translate
				 * appropriately.
				 */
  unsigned short	copy:1;
  int			(*func) (struct sk_buff *, struct device *,
				 struct packet_type *);
  void			*data;
  struct packet_type	*next;
};


/* Used by dev_rint */
#define IN_SKBUFF	1
#define DEV_QUEUE_MAGIC	0x17432895


extern struct device	*dev_base;
extern struct packet_type *ptype_base;


extern int		ip_addr_match(unsigned long addr1, unsigned long addr2);
extern int		chk_addr(unsigned long addr);
extern struct device	*dev_check(unsigned long daddr);
extern unsigned long	my_addr(void);

extern void		dev_add_pack(struct packet_type *pt);
extern void		dev_remove_pack(struct packet_type *pt);
extern struct device	*dev_get(char *name);
extern int		dev_open(struct device *dev);
extern int		dev_close(struct device *dev);
extern void		dev_queue_xmit(struct sk_buff *skb, struct device *dev,
				       int pri);
#define HAVE_NETIF_RX 1
extern void		netif_rx(struct sk_buff *skb);
/* The old interface to netif_rx(). */
extern int		dev_rint(unsigned char *buff, long len, int flags,
				 struct device * dev);
extern void		dev_transmit(void);
extern int		in_inet_bh(void);
extern void		inet_bh(void *tmp);
extern void		dev_tint(struct device *dev);
extern int		dev_get_info(char *buffer);
extern int		dev_ioctl(unsigned int cmd, void *);

extern void		dev_init(void);

#endif	/* _DEV_H */
