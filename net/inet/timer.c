/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		TIMER - implementation of software timers.
 *
 * Version:	@(#)timer.c	1.0.7	05/25/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Corey Minyard <wf-rch!minyard@relay.EU.net>
 *		Fred Baumgarten, <dc6iq@insu1.etec.uni-karlsruhe.de>
 *		Florian La Roche, <flla@stud.uni-sb.de>
 *
 * Fixes:
 *		Alan Cox	:	To avoid destroying a wait queue as we use it
 *					we defer destruction until the destroy timer goes
 *					off.
 *		Alan Cox	:	Destroy socket doesnt write a status value to the
 *					socket buffer _AFTER_ freeing it! Also sock ensures
 *					the socket will get removed BEFORE this is called
 *					otherwise if the timer TIME_DESTROY occurs inside
 *					of inet_bh() with this socket being handled it goes
 *					BOOM! Have to stop timer going off if inet_bh is
 *					active or the destroy causes crashes.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <asm/system.h>
#include <linux/interrupt.h>
#include "inet.h"
#include "dev.h"
#include "ip.h"
#include "protocol.h"
#include "tcp.h"
#include "skbuff.h"
#include "sock.h"
#include "arp.h"

void
delete_timer (struct sock *t)
{
  unsigned long flags;

  save_flags (flags);
  cli();

  t->timeout = 0;
  del_timer (&t->timer);

  restore_flags (flags);
}

void
reset_timer (struct sock *t, int timeout, unsigned long len)
{
  delete_timer (t);

  if (timeout != -1)
    t->timeout = timeout;

#if 1
  /* FIXME: ??? */
  if ((int) len < 0)	/* prevent close to infinite timers. THEY _DO_ */
	len = 3;	/* happen (negative values ?) - don't ask me why ! -FB */
#endif
  t->timer.expires = len;
  add_timer (&t->timer);
}


/*
 * Now we will only be called whenever we need to do
 * something, but we must be sure to process all of the
 * sockets that need it.
 */
void
net_timer (unsigned long data)
{
  struct sock *sk = (struct sock*)data;
  int why = sk->timeout;
  /* timeout is overwritten by 'delete_timer' and 'reset_timer' */

  if (sk->inuse || in_inet_bh()) {
    sk->timer.expires = 10;
    add_timer(&sk->timer);
    return;
  }
  sk->inuse = 1;

  DPRINTF ((DBG_TMR, "net_timer: found sk=%X why = %d\n", sk, why));
  if (sk->wfront && 
      before(sk->window_seq, sk->wfront->h.seq) &&
      sk->send_head == NULL &&
      sk->ack_backlog == 0 &&
      sk->state != TCP_TIME_WAIT)
    reset_timer(sk, TIME_PROBE0, sk->rto);
  else if (sk->keepopen)
    reset_timer (sk, TIME_KEEPOPEN, TCP_TIMEOUT_LEN);

  /* Always see if we need to send an ack. */
  if (sk->ack_backlog) {
    sk->prot->read_wakeup (sk);
    if (! sk->dead)
      wake_up_interruptible (sk->sleep);
  }

  /* Now we need to figure out why the socket was on the timer. */
  switch (why) {
    case TIME_DONE:
	if (! sk->dead || sk->state != TCP_CLOSE) {
	  printk ("non dead socket in time_done\n");
	  release_sock (sk);
	  break;
	}
	destroy_sock (sk);
	break;
    case TIME_DESTROY:
	/* We've waited for a while for all the memory associated with
	 * the socket to be freed.  We need to print an error message.
	 */
	if(sk->wmem_alloc!=0 || sk->rmem_alloc!=0)
	{
		DPRINTF ((DBG_TMR, "possible memory leak.  sk = %X\n", sk));
		sk->wmem_alloc++;	/* So it DOESNT go away */
		destroy_sock (sk);
		sk->wmem_alloc--;	/* Might now have hit 0 - fall through and do it again if so */
		sk->inuse = 0;	/* This will be ok, the destroy won't totally work */
	}
	if(sk->wmem_alloc==0 && sk->rmem_alloc==0)
		destroy_sock(sk);	/* Socket gone, DONT update sk->inuse! */
	break;
    case TIME_CLOSE:
	/* We've waited long enough, close the socket. */
	sk->state = TCP_CLOSE;
	delete_timer (sk);
	/* Kill the ARP entry in case the hardware has changed. */
	arp_destroy_maybe (sk->daddr);
	if (!sk->dead)
	  wake_up_interruptible (sk->sleep);
	sk->shutdown = SHUTDOWN_MASK;
	reset_timer (sk, TIME_DESTROY, TCP_DONE_TIME);
	release_sock (sk);
	break;
    case TIME_PROBE0:
	tcp_send_probe0(sk);
	release_sock (sk);
	break;
    case TIME_WRITE:	/* try to retransmit. */
	/* It could be we got here because we needed to send an ack.
	 * So we need to check for that.
	 */
	if (sk->send_head) {
	  if (jiffies < (sk->send_head->when + sk->rto)) {
	    reset_timer (sk, TIME_WRITE, 
			 (sk->send_head->when + sk->rto - jiffies));
	    release_sock (sk);
	    break;
	  }
	  /* printk("timer: seq %d retrans %d out %d cong %d\n", sk->send_head->h.seq,
	     sk->retransmits, sk->packets_out, sk->cong_window); */
	  DPRINTF ((DBG_TMR, "retransmitting.\n"));
	  sk->prot->retransmit (sk, 0);
	  if ((sk->state == TCP_ESTABLISHED && sk->retransmits && !(sk->retransmits & 7))
	    || (sk->state != TCP_ESTABLISHED && sk->retransmits > TCP_RETR1)) {
	    DPRINTF ((DBG_TMR, "timer.c TIME_WRITE time-out 1\n"));
	    arp_destroy_maybe (sk->daddr);
	    ip_route_check (sk->daddr);
	  }
	  if (sk->state != TCP_ESTABLISHED && sk->retransmits > TCP_RETR2) {
	    DPRINTF ((DBG_TMR, "timer.c TIME_WRITE time-out 2\n"));
	    sk->err = ETIMEDOUT;
	    if (sk->state == TCP_FIN_WAIT1 || sk->state == TCP_FIN_WAIT2
	      || sk->state == TCP_LAST_ACK) {
	      sk->state = TCP_TIME_WAIT;
	      reset_timer (sk, TIME_CLOSE, TCP_TIMEWAIT_LEN);
	    } else {
	      sk->prot->close (sk, 1);
	      break;
	    }
	  }
	}
	release_sock (sk);
	break;
    case TIME_KEEPOPEN:
	/* Send something to keep the connection open. */
	if (sk->prot->write_wakeup)
	  sk->prot->write_wakeup (sk);
	sk->retransmits++;
	if (sk->shutdown == SHUTDOWN_MASK) {
	  sk->prot->close (sk, 1);
	  sk->state = TCP_CLOSE;
	}

	if ((sk->state == TCP_ESTABLISHED && sk->retransmits && !(sk->retransmits & 7))
	  || (sk->state != TCP_ESTABLISHED && sk->retransmits > TCP_RETR1)) {
	  DPRINTF ((DBG_TMR, "timer.c TIME_KEEPOPEN time-out 1\n"));
	  arp_destroy_maybe (sk->daddr);
	  ip_route_check (sk->daddr);
	  release_sock (sk);
	  break;
	}
	if (sk->state != TCP_ESTABLISHED && sk->retransmits > TCP_RETR2) {
	  DPRINTF ((DBG_TMR, "timer.c TIME_KEEPOPEN time-out 2\n"));
	  arp_destroy_maybe (sk->daddr);
	  sk->err = ETIMEDOUT;
	  if (sk->state == TCP_FIN_WAIT1 || sk->state == TCP_FIN_WAIT2) {
	    sk->state = TCP_TIME_WAIT;
	    if (!sk->dead)
	      wake_up_interruptible (sk->sleep);
	    release_sock (sk);
	  } else {
	    sk->prot->close (sk, 1);
	  }
	  break;
	}
	release_sock (sk);
	break;
    default:
	printk ("net timer expired - reason unknown, sk=%08X\n", (int)sk);
	release_sock (sk);
	break;
  }
}

