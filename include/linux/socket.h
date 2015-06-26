#ifndef _LINUX_SOCKET_H
#define _LINUX_SOCKET_H

#include <linux/sockios.h>		/* the SIOCxxx I/O controls	*/


struct sockaddr {
  unsigned short	sa_family;	/* address family, AF_xxx	*/
  char			sa_data[14];	/* 14 bytes of protocol address	*/
};

struct linger {
  int 			l_onoff;	/* Linger active		*/
  int			l_linger;	/* How long to linger for	*/
};

/* Socket types. */
#define SOCK_STREAM	1		/* stream (connection) socket	*/
#define SOCK_DGRAM	2		/* datagram (conn.less) socket	*/
#define SOCK_RAW	3		/* raw socket			*/
#define SOCK_RDM	4		/* reliably-delivered message	*/
#define SOCK_SEQPACKET	5		/* sequential packet socket	*/
#define SOCK_PACKET	10		/* linux specific way of	*/
					/* getting packets at the dev	*/
					/* level.  For writing rarp and	*/
					/* other similiar things on the	*/
					/* user level.			*/

/* Supported address families. */
#define AF_UNSPEC	0
#define AF_UNIX		1
#define AF_INET		2
#define AF_AX25		3
#define AF_IPX		4

/* Protocol families, same as address families. */
#define PF_UNIX		AF_UNIX
#define PF_INET		AF_INET
#define PF_AX25		AF_AX25
#define PF_IPX		AF_IPX

/* Flags we can use with send/ and recv. */
#define MSG_OOB		1
#define MSG_PEEK	2

/* Setsockoptions(2) level. Thanks to BSD these must match IPPROTO_xxx */
#define SOL_SOCKET	1
#define SOL_IP		0
#define SOL_IPX		256
#define SOL_AX25	257
#define SOL_TCP		6
#define SOL_UDP		17

/* For setsockoptions(2) */
#define SO_DEBUG	1
#define SO_REUSEADDR	2
#define SO_TYPE		3
#define SO_ERROR	4
#define SO_DONTROUTE	5
#define SO_BROADCAST	6
#define SO_SNDBUF	7
#define SO_RCVBUF	8
#define SO_KEEPALIVE	9
#define SO_OOBINLINE	10
#define SO_NO_CHECK	11
#define SO_PRIORITY	12
#define SO_LINGER	13

/* IP options */
#define IP_TOS		1
#define	IPTOS_LOWDELAY		0x10
#define	IPTOS_THROUGHPUT	0x08
#define	IPTOS_RELIABILITY	0x04
#define IP_TTL		2

/* IPX options */
#define IPX_TYPE	1

/* AX.25 options */
#define AX25_WINDOW	1

/* TCP options - this way around because someone left a set in the c library includes */
#define TCP_NODELAY	1
#define TCP_MAXSEG	2

/* The various priorities. */
#define SOPRI_INTERACTIVE	0
#define SOPRI_NORMAL		1
#define SOPRI_BACKGROUND	2

#endif /* _LINUX_SOCKET_H */
