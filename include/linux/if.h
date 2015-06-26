/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Global definitions for the INET interface module.
 *
 * Version:	@(#)if.h	1.0.2	04/18/93
 *
 * Authors:	Original taken from Berkeley UNIX 4.3, (c) UCB 1982-1988
 *		Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#ifndef _LINUX_IF_H
#define _LINUX_IF_H

#include <linux/types.h>		/* for "caddr_t" et al		*/
#include <linux/socket.h>		/* for "struct sockaddr" et al	*/


/* Structure defining a queue for a network interface. */
struct ifnet {
  char		*if_name;		/* name, e.g. ``en'' or ``lo''	*/
  short		if_unit;		/* sub-unit for device driver	*/
  short		if_mtu;			/* maximum transmission unit	*/
  short		if_flags;		/* up/down, broadcast, etc.	*/
  short		if_timer;		/* time 'til if_watchdog called	*/
  int		if_metric;		/* routing metric (not used)	*/
  struct	ifaddr *if_addrlist;	/* linked list of addrs per if	*/
  struct	ifqueue {
#ifdef not_yet_in_linux
	struct mbuf	*ifq_head;
	struct mbuf	*ifq_tail;
	int		ifq_len;
	int		ifq_maxlen;
	int		ifq_drops;
#endif
  } if_snd;				/* output queue			*/

  /* Procedure handles. */
  int		(*if_init)();		/* init routine			*/
  int		(*if_output)();		/* output routine		*/
  int		(*if_ioctl)();		/* ioctl routine		*/
  int		(*if_reset)();		/* bus reset routine		*/
  int		(*if_watchdog)();	/* timer routine		*/

  /* Generic interface statistics. */
  int		if_ipackets;		/* packets recv'd on interface	*/
  int		if_ierrors;		/* input errors on interface	*/
  int		if_opackets;		/* packets sent on interface	*/
  int		if_oerrors;		/* output errors on interface	*/
  int		if_collisions;		/* collisions on CSMA i'faces	*/

  /* Linked list: pointer to next interface. */
  struct ifnet	*if_next;
};

/* Standard interface flags. */
#define	IFF_UP		0x1		/* interface is up		*/
#define	IFF_BROADCAST	0x2		/* broadcast address valid	*/
#define	IFF_DEBUG	0x4		/* turn on debugging		*/
#define	IFF_LOOPBACK	0x8		/* is a loopback net		*/
#define	IFF_POINTOPOINT	0x10		/* interface is has p-p link	*/
#define	IFF_NOTRAILERS	0x20		/* avoid use of trailers	*/
#define	IFF_RUNNING	0x40		/* resources allocated		*/
#define	IFF_NOARP	0x80		/* no ARP protocol		*/

/* These are not yet used: */
#define	IFF_PROMISC	0x100		/* recve all packets		*/
#define	IFF_ALLMULTI	0x200		/* recve all multicast packets	*/


/*
 * The ifaddr structure contains information about one address
 * of an interface.  They are maintained by the different address
 * families, are allocated and attached when an address is set,
 * and are linked together so all addresses for an interface can
 * be located.
 */
struct ifaddr {
  struct sockaddr	ifa_addr;	/* address of interface		*/
  union {
	struct sockaddr	ifu_broadaddr;
	struct sockaddr	ifu_dstaddr;
  } ifa_ifu;
  struct iface		*ifa_ifp;	/* back-pointer to interface	*/
  struct ifaddr		*ifa_next;	/* next address for interface	*/
};
#define	ifa_broadaddr	ifa_ifu.ifu_broadaddr	/* broadcast address	*/
#define	ifa_dstaddr	ifa_ifu.ifu_dstaddr	/* other end of link	*/

/*
 * Interface request structure used for socket
 * ioctl's.  All interface ioctl's must have parameter
 * definitions which begin with ifr_name.  The
 * remainder may be interface specific.
 */
struct ifreq {
#define IFHWADDRLEN	6
#define	IFNAMSIZ	16
	union
	{
		char	ifrn_name[IFNAMSIZ];		/* if name, e.g. "en0" */
		char	ifrn_hwaddr[IFHWADDRLEN];
	} ifr_ifrn;
	
	union {
		struct	sockaddr ifru_addr;
		struct	sockaddr ifru_dstaddr;
		struct	sockaddr ifru_broadaddr;
		struct	sockaddr ifru_netmask;
		short	ifru_flags;
		int	ifru_metric;
		int	ifru_mtu;
		caddr_t	ifru_data;
	} ifr_ifru;
};

#define ifr_name	ifr_ifrn.ifrn_name	/* interface name 	*/
#define ifr_hwaddr	ifr_ifrn.ifrn_hwaddr	/* interface hardware   */
#define	ifr_addr	ifr_ifru.ifru_addr	/* address		*/
#define	ifr_dstaddr	ifr_ifru.ifru_dstaddr	/* other end of p-p lnk	*/
#define	ifr_broadaddr	ifr_ifru.ifru_broadaddr	/* broadcast address	*/
#define	ifr_netmask	ifr_ifru.ifru_netmask	/* interface net mask	*/
#define	ifr_flags	ifr_ifru.ifru_flags	/* flags		*/
#define	ifr_metric	ifr_ifru.ifru_metric	/* metric		*/
#define	ifr_mtu		ifr_ifru.ifru_mtu	/* mtu			*/
#define	ifr_data	ifr_ifru.ifru_data	/* for use by interface	*/

/*
 * Structure used in SIOCGIFCONF request.
 * Used to retrieve interface configuration
 * for machine (useful for programs which
 * must know all networks accessible).
 */
struct ifconf {
	int	ifc_len;			/* size of buffer	*/
	union {
		caddr_t	ifcu_buf;
		struct	ifreq *ifcu_req;
	} ifc_ifcu;
};
#define	ifc_buf	ifc_ifcu.ifcu_buf		/* buffer address	*/
#define	ifc_req	ifc_ifcu.ifcu_req		/* array of structures	*/


/* BSD UNIX expects to find these here, so here we go: */
#include <linux/if_arp.h>
#include <linux/route.h>

#endif /* _NET_IF_H */
