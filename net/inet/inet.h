/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		General Definitions for the TCP/IP (INET) module. This is
 *		mostly a bunch of "general" macros, plus the PROTOCOL link
 *		code and data.
 *
 * Version:	@(#)inet.h	1.0.6	05/25/93
 *
 * Author:	Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 *		This work was derived friom Ross Biro's inspirational work
 *		for the LINUX operating system.  His version numbers were:
 *
 *		$Id: Space.c,v     0.8.4.5  1992/12/12 19:25:04 bir7 Exp $
 *		$Id: arp.c,v       0.8.4.6  1993/01/28 22:30:00 bir7 Exp $
 *		$Id: arp.h,v       0.8.4.6  1993/01/28 22:30:00 bir7 Exp $
 *		$Id: dev.c,v       0.8.4.13 1993/01/23 18:00:11 bir7 Exp $
 *		$Id: dev.h,v       0.8.4.7  1993/01/23 18:00:11 bir7 Exp $
 *		$Id: eth.c,v       0.8.4.4  1993/01/22 23:21:38 bir7 Exp $
 *		$Id: eth.h,v       0.8.4.1  1992/11/10 00:17:18 bir7 Exp $
 *		$Id: icmp.c,v      0.8.4.9  1993/01/23 18:00:11 bir7 Exp $
 *		$Id: icmp.h,v      0.8.4.2  1992/11/15 14:55:30 bir7 Exp $
 * 		$Id: ip.c,v        0.8.4.8  1992/12/12 19:25:04 bir7 Exp $
 * 		$Id: ip.h,v        0.8.4.2  1993/01/23 18:00:11 bir7 Exp $
 * 		$Id: loopback.c,v  0.8.4.8  1993/01/23 18:00:11 bir7 Exp $
 * 		$Id: packet.c,v    0.8.4.7  1993/01/26 22:04:00 bir7 Exp $
 *		$Id: protocols.c,v 0.8.4.3  1992/11/15 14:55:30 bir7 Exp $
 *		$Id: raw.c,v       0.8.4.12 1993/01/26 22:04:00 bir7 Exp $
 *		$Id: sock.c,v      0.8.4.6  1993/01/28 22:30:00 bir7 Exp $
 *		$Id: sock.h,v      0.8.4.7  1993/01/26 22:04:00 bir7 Exp $
 *		$Id: tcp.c,v       0.8.4.16 1993/01/26 22:04:00 bir7 Exp $
 *		$Id: tcp.h,v       0.8.4.7  1993/01/22 22:58:08 bir7 Exp $
 *		$Id: timer.c,v     0.8.4.8  1993/01/23 18:00:11 bir7 Exp $
 *		$Id: timer.h,v     0.8.4.2  1993/01/23 18:00:11 bir7 Exp $
 *		$Id: udp.c,v       0.8.4.12 1993/01/26 22:04:00 bir7 Exp $
 *		$Id: udp.h,v       0.8.4.1  1992/11/10 00:17:18 bir7 Exp $
 *		$Id: we.c,v        0.8.4.10 1993/01/23 18:00:11 bir7 Exp $
 *		$Id: wereg.h,v     0.8.4.1  1992/11/10 00:17:18 bir7 Exp $
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#ifndef _INET_H
#define _INET_H


#include <linux/ddi.h>


#define NET16(x)	((((x) >> 8) & 0x00FF) | (((x) << 8) & 0xFF00))


#undef	INET_DEBUG
#ifdef	INET_DEBUG
#  define	DPRINTF(x)	dprintf x 
#else
#   define	DPRINTF(x)	do ; while (0)
#endif

/* Debug levels. One per module. */
#define DBG_OFF		0			/* no debugging		*/
#define DBG_INET	1			/* sock.c		*/
#define DBG_RT		2			/* route.c		*/
#define DBG_DEV		3			/* dev.c		*/
#define DBG_ETH		4			/* eth.c		*/
#define DBG_PROTO	5			/* protocol.c		*/
#define DBG_TMR		6			/* timer.c		*/
#define DBG_PKT		7			/* packet.c		*/
#define DBG_RAW		8			/* raw.c		*/

#define DBG_LOOPB	10			/* loopback.c		*/
#define DBG_SLIP	11			/* slip.c		*/

#define DBG_ARP		20			/* arp.c		*/
#define DBG_IP		21			/* ip.c			*/
#define DBG_ICMP	22			/* icmp.c		*/
#define DBG_TCP		23			/* tcp.c		*/
#define DBG_UDP		24			/* udp.c		*/


extern int		inet_debug;


extern void		inet_proto_init(struct ddi_proto *pro);
extern char		*in_ntoa(unsigned long in);
extern unsigned long	in_aton(char *str);

extern void		dprintf(int level, char *fmt, ...);

extern int		dbg_ioctl(void *arg, int level);

#endif	/* _INET_H */
