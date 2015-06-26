/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		This file implements the various access functions for the
 *		PROC file system.  It is mainly used for debugging and
 *		statistics.
 *
 * Version:	@(#)proc.c	1.0.5	05/27/93
 *
 * Authors:	Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Gerald J. Heim, <heim@peanuts.informatik.uni-tuebingen.de>
 *		Fred Baumgarten, <dc6iq@insu1.etec.uni-karlsruhe.de>
 *
 * Fixes:
 *		Alan Cox	:	UDP sockets show the rxqueue/txqueue
 *					using hint flag for the netinfo.
 *	Pauline Middelink	:	Pidentd support
 *		Alan Cox	:	Make /proc safer.
 *
 * To Do:
 *		Put the creating userid in the proc/net/... files. This will
 *		allow us to write an RFC931 daemon for Linux
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#include <asm/system.h>
#include <linux/autoconf.h>
#include <linux/sched.h>
#include <linux/socket.h>
#include <linux/net.h>
#include <linux/un.h>
#include <linux/in.h>
#include <linux/param.h>
#include "inet.h"
#include "dev.h"
#include "ip.h"
#include "protocol.h"
#include "tcp.h"
#include "udp.h"
#include "skbuff.h"
#include "sock.h"
#include "raw.h"

/*
 * Get__netinfo returns the length of that string.
 *
 * KNOWN BUGS
 *  As in get_unix_netinfo, the buffer might be too small. If this
 *  happens, get__netinfo returns only part of the available infos.
 */
static int
get__netinfo(struct proto *pro, char *buffer, int format)
{
  struct sock **s_array;
  struct sock *sp;
  char *pos=buffer;
  int i;
  int timer_active;
  unsigned long  dest, src;
  unsigned short destp, srcp;

  s_array = pro->sock_array;
  pos+=sprintf(pos, "sl  local_address rem_address   st tx_queue rx_queue tr tm->when uid\n");
/*
 *	This was very pretty but didn't work when a socket is destroyed at the wrong moment
 *	(eg a syn recv socket getting a reset), or a memory timer destroy. Instead of playing
 *	with timers we just concede defeat and cli().
 */
  for(i = 0; i < SOCK_ARRAY_SIZE; i++) {
  	cli();
	sp = s_array[i];
	while(sp != NULL) {
		dest  = sp->daddr;
		src   = sp->saddr;
		destp = sp->dummy_th.dest;
		srcp  = sp->dummy_th.source;

		/* Since we are Little Endian we need to swap the bytes :-( */
		destp = ntohs(destp);
		srcp  = ntohs(srcp);
		timer_active = del_timer(&sp->timer);
		if (!timer_active)
			sp->timer.expires = 0;
		pos+=sprintf(pos, "%2d: %08lX:%04X %08lX:%04X %02X %08lX:%08lX %02X:%08lX %08X %d\n",
			i, src, srcp, dest, destp, sp->state, 
			format==0?sp->write_seq-sp->rcv_ack_seq:sp->rmem_alloc, 
			format==0?sp->acked_seq-sp->copied_seq:sp->wmem_alloc,
			timer_active, sp->timer.expires, (unsigned) sp->retransmits,
			SOCK_INODE(sp->socket)->i_uid);
		if (timer_active)
			add_timer(&sp->timer);
		/* Is place in buffer too rare? then abort. */
		if (pos > buffer+PAGE_SIZE-80) {
			printk("oops, too many %s sockets for netinfo.\n",
					pro->name);
			return(strlen(buffer));
		}

		/*
		 * All sockets with (port mod SOCK_ARRAY_SIZE) = i
		 * are kept in sock_array[i], so we must follow the
		 * 'next' link to get them all.
		 */
		sp = sp->next;
	}
	sti();	/* We only turn interrupts back on for a moment, but because the interrupt queues anything built up
		   before this will clear before we jump back and cli, so its not as bad as it looks */
  }
  return(strlen(buffer));
} 


int tcp_get_info(char *buffer)
{
  return get__netinfo(&tcp_prot, buffer,0);
}


int udp_get_info(char *buffer)
{
  return get__netinfo(&udp_prot, buffer,1);
}


int raw_get_info(char *buffer)
{
  return get__netinfo(&raw_prot, buffer,1);
}
