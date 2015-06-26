/*
 * UNIX		An implementation of the AF_UNIX network domain for the
 *		LINUX operating system.  UNIX is implemented using the
 *		BSD Socket interface as the means of communication with
 *		the user level.
 *
 *		The functions in this file provide an interface between
 *		the PROC file system and the "unix" family of networking
 *		protocols. It is mainly used for debugging and statistics.
 *
 * Version:	@(#)proc.c	1.0.4	05/23/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Gerald J. Heim, <heim@peanuts.informatik.uni-tuebingen.de>
 *		Fred Baumgarten, <dc6iq@insu1.etec.uni-kalrsruhe.de>
 *
 * Fixes:
 *		Dmitry Gorodchanin	:	/proc locking fix
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#include <linux/autoconf.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/socket.h>
#include <linux/net.h>
#include <linux/ddi.h>
#include <linux/un.h>
#include <linux/param.h>
#include "unix.h"


/* Called from PROCfs. */
int unix_get_info(char *buffer)
{
  char *pos;
  int i;

  pos = buffer;
  pos += sprintf(pos, "Num RefCount Protocol Flags    Type St Path\n");

  for(i = 0; i < NSOCKETS; i++) {
	if (unix_datas[i].refcnt>0) {
		pos += sprintf(pos, "%2d: %08X %08X %08lX %04X %02X", i,
			unix_datas[i].refcnt,
			unix_datas[i].protocol,
			unix_datas[i].socket->flags,
			unix_datas[i].socket->type,
			unix_datas[i].socket->state
		);

		/* If socket is bound to a filename, we'll print it. */
		if(unix_datas[i].sockaddr_len>0) {
			pos += sprintf(pos, " %s\n",
				unix_datas[i].sockaddr_un.sun_path);
		} else { /* just add a newline */
			*pos='\n';
			pos++;
			*pos='\0';
		}

		/*
		 * Check whether buffer _may_ overflow in the next loop.
		 * Since sockets may have very very long paths, we make
		 * PATH_MAX+80 the minimum space left for a new line.
		 */
		if (pos > buffer+PAGE_SIZE-80-PATH_MAX) {
			printk("UNIX: netinfo: oops, too many sockets.\n");
			return(pos - buffer);
		}
	}
  }
  return(pos - buffer);
}
