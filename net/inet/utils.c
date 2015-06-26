/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Various kernel-resident INET utility functions; mainly
 *		for format conversion and debugging output.
 *
 * Version:	@(#)utils.c	1.0.7	05/18/93
 *
 * Author:	Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 * Fixes:
 *		Alan Cox	:	verify_area check.
 *
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
#include <linux/mm.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/errno.h>
#include <linux/stat.h>
#include <stdarg.h>
#include "inet.h"
#include "dev.h"
#include "eth.h"
#include "ip.h"
#include "protocol.h"
#include "tcp.h"
#include "skbuff.h"
#include "arp.h"


/* Display an IP address in readable format. */
char *in_ntoa(unsigned long in)
{
  static char buff[18];
  register char *p;

  p = (char *) &in;
  sprintf(buff, "%d.%d.%d.%d",
	(p[0] & 255), (p[1] & 255), (p[2] & 255), (p[3] & 255));
  return(buff);
}


/* Convert an ASCII string to binary IP. */
unsigned long
in_aton(char *str)
{
  unsigned long l;
  unsigned int val;
  int i;

  l = 0;
  for (i = 0; i < 4; i++) {
	l <<= 8;
	if (*str != '\0') {
		val = 0;
		while (*str != '\0' && *str != '.') {
			val *= 10;
			val += *str - '0';
			str++;
		}
		l |= val;
		if (*str != '\0') str++;
	}
  }
  return(htonl(l));
}


void
dprintf(int level, char *fmt, ...)
{
  va_list args;
  char *buff;
  extern int vsprintf(char * buf, const char * fmt, va_list args);

  if (level != inet_debug) return;

  buff = (char *) kmalloc(256, GFP_ATOMIC);
  if (buff != NULL) {
	va_start(args, fmt);
	vsprintf(buff, fmt, args);
	va_end(args);
	printk(buff);
	kfree(buff);
  }
}


int
dbg_ioctl(void *arg, int level)
{
  int val;
  int err;
  
  if (!suser()) return(-EPERM);
  err=verify_area(VERIFY_READ, (void *)arg, sizeof(int));
  if(err)
  	return err;
  val = get_fs_long((int *)arg);
  switch(val) {
	case 0:	/* OFF */
		inet_debug = DBG_OFF;
		break;
	case 1:	/* ON, INET */
		inet_debug = level;
		break;

	case DBG_RT:		/* modules */
	case DBG_DEV:
	case DBG_ETH:
	case DBG_PROTO:
	case DBG_TMR:
	case DBG_PKT:
	case DBG_RAW:

	case DBG_LOOPB:		/* drivers */
	case DBG_SLIP:

	case DBG_ARP:		/* protocols */
	case DBG_IP:
	case DBG_ICMP:
	case DBG_TCP:
	case DBG_UDP:

		inet_debug = val;
		break;

	default:
		return(-EINVAL);
  }

  return(0);
}
