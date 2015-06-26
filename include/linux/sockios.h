/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions of the socket-level I/O control calls.
 *
 * Version:	@(#)sockios.h	1.0.2	03/09/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#ifndef _LINUX_SOCKIOS_H
#define _LINUX_SOCKIOS_H

/* This section will go away soon! */
#if 1	/* FIXME: */
#define MAX_IP_NAME	20
#define IP_SET_DEV	0x2401

struct ip_config {
  char		name[MAX_IP_NAME];
  unsigned long	paddr;
  unsigned long	router;
  unsigned long	net;
  unsigned long	up:1,destroy:1;
};
#endif	/* FIXME: */

/* Socket-level I/O control calls. */
#define FIOSETOWN 	0x8901
#define SIOCSPGRP	0x8902
#define FIOGETOWN	0x8903
#define SIOCGPGRP	0x8904
#define SIOCATMARK	0x8905

/* Routing table calls. */
#define SIOCADDRT	0x890B		/* add routing table entry	*/
#define SIOCDELRT	0x890C		/* delete routing table entry	*/

/* Socket configuration controls. */
#define SIOCGIFNAME	0x8910		/* get iface name		*/
#define SIOCSIFLINK	0x8911		/* set iface channel		*/
#define SIOCGIFCONF	0x8912		/* get iface list		*/
#define SIOCGIFFLAGS	0x8913		/* get flags			*/
#define SIOCSIFFLAGS	0x8914		/* set flags			*/
#define SIOCGIFADDR	0x8915		/* get PA address		*/
#define SIOCSIFADDR	0x8916		/* set PA address		*/
#define SIOCGIFDSTADDR	0x8917		/* get remote PA address	*/
#define SIOCSIFDSTADDR	0x8918		/* set remote PA address	*/
#define SIOCGIFBRDADDR	0x8919		/* get broadcast PA address	*/
#define SIOCSIFBRDADDR	0x891a		/* set broadcast PA address	*/
#define SIOCGIFNETMASK	0x891b		/* get network PA mask		*/
#define SIOCSIFNETMASK	0x891c		/* set network PA mask		*/
#define SIOCGIFMETRIC	0x891d		/* get metric			*/
#define SIOCSIFMETRIC	0x891e		/* set metric			*/
#define SIOCGIFMEM	0x891f		/* get memory address (BSD)	*/
#define SIOCSIFMEM	0x8920		/* set memory address (BSD)	*/
#define SIOCGIFMTU	0x8921		/* get MTU size			*/
#define SIOCSIFMTU	0x8922		/* set MTU size			*/
#define	SIOCGIFHWADDR	0x8923		/* get hardware address		*/
#define	SIOCSIFHWADDR	0x8924		/* set hardware address (NI)	*/
#define SIOCGIFENCAP	0x8925		/* get/set slip encapsulation   */
#define SIOCSIFENCAP	0x8926		

/* Routing table calls (oldrtent - don't use) */
#define SIOCADDRTOLD	0x8940		/* add routing table entry	*/
#define SIOCDELRTOLD	0x8941		/* delete routing table entry	*/

/* ARP cache control calls. */
#define SIOCDARP	0x8950		/* delete ARP table entry	*/
#define SIOCGARP	0x8951		/* get ARP table entry		*/
#define SIOCSARP	0x8952		/* set ARP table entry		*/

#endif	/* _LINUX_SOCKIOS_H */
