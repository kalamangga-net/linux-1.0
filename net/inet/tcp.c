/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Implementation of the Transmission Control Protocol(TCP).
 *
 * Version:	@(#)tcp.c	1.0.16	05/25/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Mark Evans, <evansmp@uhura.aston.ac.uk>
 *		Corey Minyard <wf-rch!minyard@relay.EU.net>
 *		Florian La Roche, <flla@stud.uni-sb.de>
 *
 * Fixes:	
 *		Alan Cox	:	Numerous verify_area() calls
 *		Alan Cox	:	Set the ACK bit on a reset
 *		Alan Cox	:	Stopped it crashing if it closed while sk->inuse=1
 *					and was trying to connect (tcp_err()).
 *		Alan Cox	:	All icmp error handling was broken
 *					pointers passed where wrong and the
 *					socket was looked up backwards. Nobody
 *					tested any icmp error code obviously.
 *		Alan Cox	:	tcp_err() now handled properly. It wakes people
 *					on errors. select behaves and the icmp error race
 *					has gone by moving it into sock.c
 *		Alan Cox	:	tcp_reset() fixed to work for everything not just
 *					packets for unknown sockets.
 *		Alan Cox	:	tcp option processing.
 *		Alan Cox	:	Reset tweaked (still not 100%) [Had syn rule wrong]
 *		Herp Rosmanith  :	More reset fixes
 *		Alan Cox	:	No longer acks invalid rst frames. Acking
 *					any kind of RST is right out.
 *		Alan Cox	:	Sets an ignore me flag on an rst receive
 *					otherwise odd bits of prattle escape still
 *		Alan Cox	:	Fixed another acking RST frame bug. Should stop
 *					LAN workplace lockups.
 *		Alan Cox	: 	Some tidyups using the new skb list facilities
 *		Alan Cox	:	sk->keepopen now seems to work
 *		Alan Cox	:	Pulls options out correctly on accepts
 *		Alan Cox	:	Fixed assorted sk->rqueue->next errors
 *		Alan Cox	:	PSH doesn't end a TCP read. Switched a bit to skb ops.
 *		Alan Cox	:	Tidied tcp_data to avoid a potential nasty.
 *		Alan Cox	:	Added some beter commenting, as the tcp is hard to follow
 *		Alan Cox	:	Removed incorrect check for 20 * psh
 *	Michael O'Reilly	:	ack < copied bug fix.
 *	Johannes Stille		:	Misc tcp fixes (not all in yet).
 *		Alan Cox	:	FIN with no memory -> CRASH
 *		Alan Cox	:	Added socket option proto entries. Also added awareness of them to accept.
 *		Alan Cox	:	Added TCP options (SOL_TCP)
 *		Alan Cox	:	Switched wakeup calls to callbacks, so the kernel can layer network sockets.
 *		Alan Cox	:	Use ip_tos/ip_ttl settings.
 *		Alan Cox	:	Handle FIN (more) properly (we hope).
 *		Alan Cox	:	RST frames sent on unsynchronised state ack error/
 *		Alan Cox	:	Put in missing check for SYN bit.
 *		Alan Cox	:	Added tcp_select_window() aka NET2E 
 *					window non shrink trick.
 *		Alan Cox	:	Added a couple of small NET2E timer fixes
 *		Charles Hedrick :	TCP fixes
 *		Toomas Tamm	:	TCP window fixes
 *		Alan Cox	:	Small URG fix to rlogin ^C ack fight
 *		Charles Hedrick	:	Window fix
 *		Linus		:	Rewrote tcp_read() and URG handling
 *					completely
 *
 *
 * To Fix:
 *			Possibly a problem with accept(). BSD accept never fails after
 *		it causes a select. Linux can - given the official select semantics I
 *		feel that _really_ its the BSD network programs that are bust (notably
 *		inetd, which hangs occasionally because of this).
 *			Add VJ Fastrecovery algorithm ?
 *			Protocol closedown badly messed up.
 *			Incompatiblity with spider ports (tcp hangs on that 
 *			socket occasionally).
 *		MSG_PEEK and read on same socket at once can cause crashes.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or(at your option) any later version.
 */
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/termios.h>
#include <linux/in.h>
#include <linux/fcntl.h>
#include "inet.h"
#include "dev.h"
#include "ip.h"
#include "protocol.h"
#include "icmp.h"
#include "tcp.h"
#include "skbuff.h"
#include "sock.h"
#include "arp.h"
#include <linux/errno.h>
#include <linux/timer.h>
#include <asm/system.h>
#include <asm/segment.h>
#include <linux/mm.h>

#define SEQ_TICK 3
unsigned long seq_offset;
#define SUBNETSARELOCAL

static __inline__ int 
min(unsigned int a, unsigned int b)
{
  if (a < b) return(a);
  return(b);
}


static void __print_th(struct tcphdr *th)
{
	unsigned char *ptr;

	printk("TCP header:\n");
	printk("    source=%d, dest=%d, seq =%ld, ack_seq = %ld\n",
		ntohs(th->source), ntohs(th->dest),
		ntohl(th->seq), ntohl(th->ack_seq));
	printk("    fin=%d, syn=%d, rst=%d, psh=%d, ack=%d, urg=%d res1=%d res2=%d\n",
		th->fin, th->syn, th->rst, th->psh, th->ack,
		th->urg, th->res1, th->res2);
	printk("    window = %d, check = %d urg_ptr = %d\n",
		ntohs(th->window), ntohs(th->check), ntohs(th->urg_ptr));
	printk("    doff = %d\n", th->doff);
	ptr =(unsigned char *)(th + 1);
	printk("    options = %d %d %d %d\n", ptr[0], ptr[1], ptr[2], ptr[3]);
}

static inline void print_th(struct tcphdr *th)
{
	if (inet_debug == DBG_TCP)
		__print_th(th);
}

/* This routine grabs the first thing off of a rcv queue. */
static struct sk_buff *
get_firstr(struct sock *sk)
{
  return skb_dequeue(&sk->rqueue);
}

/*
 *	Difference between two values in tcp ack terms.
 */

static long
diff(unsigned long seq1, unsigned long seq2)
{
  long d;

  d = seq1 - seq2;
  if (d > 0) return(d);

  /* I hope this returns what I want. */
  return(~d+1);
}

/* This routine picks a TCP windows for a socket based on
   the following constraints
   
   1. The window can never be shrunk once it is offered (RFC 793)
   2. We limit memory per socket
   
   For now we use NET2E3's heuristic of offering half the memory
   we have handy. All is not as bad as this seems however because
   of two things. Firstly we will bin packets even within the window
   in order to get the data we are waiting for into the memory limit.
   Secondly we bin common duplicate forms at receive time

   Better heuristics welcome
*/
   
static int tcp_select_window(struct sock *sk)
{
	int new_window = sk->prot->rspace(sk);

/*
 * two things are going on here.  First, we don't ever offer a
 * window less than min(sk->mss, MAX_WINDOW/2).  This is the
 * receiver side of SWS as specified in RFC1122.
 * Second, we always give them at least the window they
 * had before, in order to avoid retracting window.  This
 * is technically allowed, but RFC1122 advises against it and
 * in practice it causes trouble.
 */
	if (new_window < min(sk->mss, MAX_WINDOW/2) ||
	    new_window < sk->window)
	  return(sk->window);
	return(new_window);
}

/* Enter the time wait state. */

static void tcp_time_wait(struct sock *sk)
{
  sk->state = TCP_TIME_WAIT;
  sk->shutdown = SHUTDOWN_MASK;
  if (!sk->dead)
	sk->state_change(sk);
  reset_timer(sk, TIME_CLOSE, TCP_TIMEWAIT_LEN);
}

/*
 *	A timer event has trigger a tcp retransmit timeout. The
 *	socket xmit queue is ready and set up to send. Because
 *	the ack receive code keeps the queue straight we do
 *	nothing clever here.
 */

static void
tcp_retransmit(struct sock *sk, int all)
{
  if (all) {
	ip_retransmit(sk, all);
	return;
  }

  sk->ssthresh = sk->cong_window >> 1; /* remember window where we lost */
  /* sk->ssthresh in theory can be zero.  I guess that's OK */
  sk->cong_count = 0;

  sk->cong_window = 1;

  /* Do the actual retransmit. */
  ip_retransmit(sk, all);
}


/*
 * This routine is called by the ICMP module when it gets some
 * sort of error condition.  If err < 0 then the socket should
 * be closed and the error returned to the user.  If err > 0
 * it's just the icmp type << 8 | icmp code.  After adjustment
 * header points to the first 8 bytes of the tcp header.  We need
 * to find the appropriate port.
 */
void
tcp_err(int err, unsigned char *header, unsigned long daddr,
	unsigned long saddr, struct inet_protocol *protocol)
{
  struct tcphdr *th;
  struct sock *sk;
  struct iphdr *iph=(struct iphdr *)header;
  
  header+=4*iph->ihl;
   
  DPRINTF((DBG_TCP, "TCP: tcp_err(%d, hdr=%X, daddr=%X saddr=%X, protocol=%X)\n",
					err, header, daddr, saddr, protocol));

  th =(struct tcphdr *)header;
  sk = get_sock(&tcp_prot, th->source/*dest*/, daddr, th->dest/*source*/, saddr);
  print_th(th);

  if (sk == NULL) return;
  
  if(err<0)
  {
  	sk->err = -err;
  	sk->error_report(sk);
  	return;
  }

  if ((err & 0xff00) == (ICMP_SOURCE_QUENCH << 8)) {
	/*
	 * FIXME:
	 * For now we will just trigger a linear backoff.
	 * The slow start code should cause a real backoff here.
	 */
	if (sk->cong_window > 4) sk->cong_window--;
	return;
  }

  DPRINTF((DBG_TCP, "TCP: icmp_err got error\n"));
  sk->err = icmp_err_convert[err & 0xff].errno;

  /*
   * If we've already connected we will keep trying
   * until we time out, or the user gives up.
   */
  if (icmp_err_convert[err & 0xff].fatal) {
	if (sk->state == TCP_SYN_SENT) {
		sk->state = TCP_CLOSE;
		sk->error_report(sk);		/* Wake people up to see the error (see connect in sock.c) */
	}
  }
  return;
}


/*
 *	Walk down the receive queue counting readable data until we hit the end or we find a gap
 *	in the received data queue (ie a frame missing that needs sending to us)
 */

static int
tcp_readable(struct sock *sk)
{
  unsigned long counted;
  unsigned long amount;
  struct sk_buff *skb;
  int count=0;
  int sum;
  unsigned long flags;

  DPRINTF((DBG_TCP, "tcp_readable(sk=%X)\n", sk));
  if(sk && sk->debug)
  	printk("tcp_readable: %p - ",sk);

  if (sk == NULL || skb_peek(&sk->rqueue) == NULL) 	/* Empty sockets are easy! */
  {
  	if(sk && sk->debug) 
  		printk("empty\n");
  	return(0);
  }
  
  counted = sk->copied_seq+1;	/* Where we are at the moment */
  amount = 0;
  
  save_flags(flags);		/* So nobody adds things at the wrong moment */
  cli();
  skb =(struct sk_buff *)sk->rqueue;

  /* Do until a push or until we are out of data. */
  do {
	count++;
#ifdef OLD	
	/* This is wrong: It breaks Chameleon amongst other stacks */
	if (count > 20) {
		restore_flags(flags);
		DPRINTF((DBG_TCP, "tcp_readable, more than 20 packets without a psh\n"));
		printk("tcp_read: possible read_queue corruption.\n");
		return(amount);
	}
#endif	
	if (before(counted, skb->h.th->seq)) 	/* Found a hole so stops here */
		break;
	sum = skb->len -(counted - skb->h.th->seq);	/* Length - header but start from where we are up to (avoid overlaps) */
	if (skb->h.th->syn)
		sum++;
	if (sum >= 0) {					/* Add it up, move on */
		amount += sum;
		if (skb->h.th->syn) amount--;
		counted += sum;
	}
	if (amount && skb->h.th->psh) break;
	skb =(struct sk_buff *)skb->next;		/* Move along */
  } while(skb != sk->rqueue);
  if (amount && !sk->urginline && sk->urg_data &&
      (sk->urg_seq - sk->copied_seq) <= (counted - sk->copied_seq))
	amount--;		/* don't count urg data */
  restore_flags(flags);
  DPRINTF((DBG_TCP, "tcp readable returning %d bytes\n", amount));
  if(sk->debug)
  	printk("got %lu bytes.\n",amount);
  return(amount);
}


/*
 *	Wait for a TCP event. Note the oddity with SEL_IN and reading. The
 *	listening socket has a receive queue of sockets to accept.
 */

static int
tcp_select(struct sock *sk, int sel_type, select_table *wait)
{
  DPRINTF((DBG_TCP, "tcp_select(sk=%X, sel_type = %d, wait = %X)\n",
	  					sk, sel_type, wait));

  sk->inuse = 1;
  switch(sel_type) {
	case SEL_IN:
		if(sk->debug)
			printk("select in");
		select_wait(sk->sleep, wait);
		if(sk->debug)
			printk("-select out");
		if (skb_peek(&sk->rqueue) != NULL) {
			if (sk->state == TCP_LISTEN || tcp_readable(sk)) {
				release_sock(sk);
				if(sk->debug)
					printk("-select ok data\n");
				return(1);
			}
		}
		if (sk->err != 0)	/* Receiver error */
		{
			release_sock(sk);
			if(sk->debug)
				printk("-select ok error");
			return(1);
		}
		if (sk->shutdown & RCV_SHUTDOWN) {
			release_sock(sk);
			if(sk->debug)
				printk("-select ok down\n");
			return(1);
		} else {
			release_sock(sk);
			if(sk->debug)
				printk("-select fail\n");
			return(0);
		}
	case SEL_OUT:
		select_wait(sk->sleep, wait);
		if (sk->shutdown & SEND_SHUTDOWN) {
			DPRINTF((DBG_TCP,
				"write select on shutdown socket.\n"));

			/* FIXME: should this return an error? */
			release_sock(sk);
			return(0);
		}

		/*
		 * FIXME:
		 * Hack so it will probably be able to write
		 * something if it says it's ok to write.
		 */
		if (sk->prot->wspace(sk) >= sk->mss) {
			release_sock(sk);
			/* This should cause connect to work ok. */
			if (sk->state == TCP_SYN_RECV ||
			    sk->state == TCP_SYN_SENT) return(0);
			return(1);
		}
		DPRINTF((DBG_TCP,
			"tcp_select: sleeping on write sk->wmem_alloc = %d, "
			"sk->packets_out = %d\n"
			"sk->wback = %X, sk->wfront = %X\n"
			"sk->write_seq = %u, sk->window_seq=%u\n", 
				sk->wmem_alloc, sk->packets_out,
				sk->wback, sk->wfront,
				sk->write_seq, sk->window_seq));

		release_sock(sk);
		return(0);
	case SEL_EX:
		select_wait(sk->sleep,wait);
		if (sk->err || sk->urg_data) {
			release_sock(sk);
			return(1);
		}
		release_sock(sk);
		return(0);
  }

  release_sock(sk);
  return(0);
}


int
tcp_ioctl(struct sock *sk, int cmd, unsigned long arg)
{
  int err;
  DPRINTF((DBG_TCP, "tcp_ioctl(sk=%X, cmd = %d, arg=%X)\n", sk, cmd, arg));
  switch(cmd) {
	case DDIOCSDBG:
		return(dbg_ioctl((void *) arg, DBG_TCP));

	case TIOCINQ:
#ifdef FIXME	/* FIXME: */
	case FIONREAD:
#endif
		{
			unsigned long amount;

			if (sk->state == TCP_LISTEN) return(-EINVAL);

			sk->inuse = 1;
			amount = tcp_readable(sk);
			release_sock(sk);
			DPRINTF((DBG_TCP, "returning %d\n", amount));
			err=verify_area(VERIFY_WRITE,(void *)arg,
						   sizeof(unsigned long));
			if(err)
				return err;
			put_fs_long(amount,(unsigned long *)arg);
			return(0);
		}
	case SIOCATMARK:
		{
			int answ = sk->urg_data && sk->urg_seq == sk->copied_seq+1;

			err = verify_area(VERIFY_WRITE,(void *) arg,
						  sizeof(unsigned long));
			if (err)
				return err;
			put_fs_long(answ,(int *) arg);
			return(0);
		}
	case TIOCOUTQ:
		{
			unsigned long amount;

			if (sk->state == TCP_LISTEN) return(-EINVAL);
			amount = sk->prot->wspace(sk);
			err=verify_area(VERIFY_WRITE,(void *)arg,
						   sizeof(unsigned long));
			if(err)
				return err;
			put_fs_long(amount,(unsigned long *)arg);
			return(0);
		}
	default:
		return(-EINVAL);
  }
}


/* This routine computes a TCP checksum. */
unsigned short
tcp_check(struct tcphdr *th, int len,
	  unsigned long saddr, unsigned long daddr)
{     
  unsigned long sum;
   
  if (saddr == 0) saddr = my_addr();
  print_th(th);
  __asm__("\t addl %%ecx,%%ebx\n"
	  "\t adcl %%edx,%%ebx\n"
	  "\t adcl $0, %%ebx\n"
	  : "=b"(sum)
	  : "0"(daddr), "c"(saddr), "d"((ntohs(len) << 16) + IPPROTO_TCP*256)
	  : "cx","bx","dx" );
   
  if (len > 3) {
	__asm__("\tclc\n"
		"1:\n"
		"\t lodsl\n"
		"\t adcl %%eax, %%ebx\n"
		"\t loop 1b\n"
		"\t adcl $0, %%ebx\n"
		: "=b"(sum) , "=S"(th)
		: "0"(sum), "c"(len/4) ,"1"(th)
		: "ax", "cx", "bx", "si" );
  }
   
  /* Convert from 32 bits to 16 bits. */
  __asm__("\t movl %%ebx, %%ecx\n"
	  "\t shrl $16,%%ecx\n"
	  "\t addw %%cx, %%bx\n"
	  "\t adcw $0, %%bx\n"
	  : "=b"(sum)
	  : "0"(sum)
	  : "bx", "cx");
   
  /* Check for an extra word. */
  if ((len & 2) != 0) {
	__asm__("\t lodsw\n"
		"\t addw %%ax,%%bx\n"
		"\t adcw $0, %%bx\n"
		: "=b"(sum), "=S"(th)
		: "0"(sum) ,"1"(th)
		: "si", "ax", "bx");
  }
   
  /* Now check for the extra byte. */
  if ((len & 1) != 0) {
	__asm__("\t lodsb\n"
		"\t movb $0,%%ah\n"
		"\t addw %%ax,%%bx\n"
		"\t adcw $0, %%bx\n"
		: "=b"(sum)
		: "0"(sum) ,"S"(th)
		: "si", "ax", "bx");
  }
   
  /* We only want the bottom 16 bits, but we never cleared the top 16. */
  return((~sum) & 0xffff);
}


void tcp_send_check(struct tcphdr *th, unsigned long saddr, 
		unsigned long daddr, int len, struct sock *sk)
{
	th->check = 0;
	th->check = tcp_check(th, len, saddr, daddr);
	return;
}

static void tcp_send_skb(struct sock *sk, struct sk_buff *skb)
{
	int size;
	struct tcphdr * th = skb->h.th;

	/* length of packet (not counting length of pre-tcp headers) */
	size = skb->len - ((unsigned char *) th - skb->data);

	/* sanity check it.. */
	if (size < sizeof(struct tcphdr) || size > skb->len) {
		printk("tcp_send_skb: bad skb (skb = %p, data = %p, th = %p, len = %lu)\n",
			skb, skb->data, th, skb->len);
		kfree_skb(skb, FREE_WRITE);
		return;
	}

	/* If we have queued a header size packet.. */
	if (size == sizeof(struct tcphdr)) {
		/* If its got a syn or fin its notionally included in the size..*/
		if(!th->syn && !th->fin) {
			printk("tcp_send_skb: attempt to queue a bogon.\n");
			kfree_skb(skb,FREE_WRITE);
			return;
		}
	}
  
	/* We need to complete and send the packet. */
	tcp_send_check(th, sk->saddr, sk->daddr, size, sk);

	skb->h.seq = ntohl(th->seq) + size - 4*th->doff;
	if (after(skb->h.seq, sk->window_seq) ||
	    (sk->retransmits && sk->timeout == TIME_WRITE) ||
	     sk->packets_out >= sk->cong_window) {
		DPRINTF((DBG_TCP, "sk->cong_window = %d, sk->packets_out = %d\n",
					sk->cong_window, sk->packets_out));
		DPRINTF((DBG_TCP, "sk->write_seq = %d, sk->window_seq = %d\n",
					sk->write_seq, sk->window_seq));
		skb->next = NULL;
		skb->magic = TCP_WRITE_QUEUE_MAGIC;
		if (sk->wback == NULL) {
			sk->wfront = skb;
		} else {
			sk->wback->next = skb;
		}
		sk->wback = skb;
		if (before(sk->window_seq, sk->wfront->h.seq) &&
		    sk->send_head == NULL &&
		    sk->ack_backlog == 0)
		  reset_timer(sk, TIME_PROBE0, sk->rto);
	} else {
		sk->sent_seq = sk->write_seq;
		sk->prot->queue_xmit(sk, skb->dev, skb, 0);
	}
}

struct sk_buff * tcp_dequeue_partial(struct sock * sk)
{
	struct sk_buff * skb;
	unsigned long flags;

	save_flags(flags);
	cli();
	skb = sk->partial;
	if (skb) {
		sk->partial = NULL;
		del_timer(&sk->partial_timer);
	}
	restore_flags(flags);
	return skb;
}

static void tcp_send_partial(struct sock *sk)
{
	struct sk_buff *skb;

	if (sk == NULL)
		return;
	while ((skb = tcp_dequeue_partial(sk)) != NULL)
		tcp_send_skb(sk, skb);
}

void tcp_enqueue_partial(struct sk_buff * skb, struct sock * sk)
{
	struct sk_buff * tmp;
	unsigned long flags;

	save_flags(flags);
	cli();
	tmp = sk->partial;
	if (tmp)
		del_timer(&sk->partial_timer);
	sk->partial = skb;
	sk->partial_timer.expires = HZ;
	sk->partial_timer.function = (void (*)(unsigned long)) tcp_send_partial;
	sk->partial_timer.data = (unsigned long) sk;
	add_timer(&sk->partial_timer);
	restore_flags(flags);
	if (tmp)
		tcp_send_skb(sk, tmp);
}


/* This routine sends an ack and also updates the window. */
static void
tcp_send_ack(unsigned long sequence, unsigned long ack,
	     struct sock *sk,
	     struct tcphdr *th, unsigned long daddr)
{
  struct sk_buff *buff;
  struct tcphdr *t1;
  struct device *dev = NULL;
  int tmp;

  if(sk->zapped)
	return;		/* We have been reset, we may not send again */
  /*
   * We need to grab some memory, and put together an ack,
   * and then put it into the queue to be sent.
   */
  buff = sk->prot->wmalloc(sk, MAX_ACK_SIZE, 1, GFP_ATOMIC);
  if (buff == NULL) {
	/* Force it to send an ack. */
	sk->ack_backlog++;
	if (sk->timeout != TIME_WRITE && tcp_connected(sk->state)) {
		reset_timer(sk, TIME_WRITE, 10);
	}
if (inet_debug == DBG_SLIP) printk("\rtcp_ack: malloc failed\n");
	return;
  }

  buff->mem_addr = buff;
  buff->mem_len = MAX_ACK_SIZE;
  buff->len = sizeof(struct tcphdr);
  buff->sk = sk;
  t1 =(struct tcphdr *) buff->data;

  /* Put in the IP header and routing stuff. */
  tmp = sk->prot->build_header(buff, sk->saddr, daddr, &dev,
				IPPROTO_TCP, sk->opt, MAX_ACK_SIZE,sk->ip_tos,sk->ip_ttl);
  if (tmp < 0) {
  	buff->free=1;
	sk->prot->wfree(sk, buff->mem_addr, buff->mem_len);
if (inet_debug == DBG_SLIP) printk("\rtcp_ack: build_header failed\n");
	return;
  }
  buff->len += tmp;
  t1 =(struct tcphdr *)((char *)t1 +tmp);

  /* FIXME: */
  memcpy(t1, th, sizeof(*t1)); /* this should probably be removed */

  /* swap the send and the receive. */
  t1->dest = th->source;
  t1->source = th->dest;
  t1->seq = ntohl(sequence);
  t1->ack = 1;
  sk->window = tcp_select_window(sk);/*sk->prot->rspace(sk);*/
  t1->window = ntohs(sk->window);
  t1->res1 = 0;
  t1->res2 = 0;
  t1->rst = 0;
  t1->urg = 0;
  t1->syn = 0;
  t1->psh = 0;
  t1->fin = 0;
  if (ack == sk->acked_seq) {
	sk->ack_backlog = 0;
	sk->bytes_rcv = 0;
	sk->ack_timed = 0;
	if (sk->send_head == NULL && sk->wfront == NULL && sk->timeout == TIME_WRITE) 
	{
		if(sk->keepopen)
			reset_timer(sk,TIME_KEEPOPEN,TCP_TIMEOUT_LEN);
		else
			delete_timer(sk);
	}
  }
  t1->ack_seq = ntohl(ack);
  t1->doff = sizeof(*t1)/4;
  tcp_send_check(t1, sk->saddr, daddr, sizeof(*t1), sk);
  if (sk->debug)
  	 printk("\rtcp_ack: seq %lx ack %lx\n", sequence, ack);
  sk->prot->queue_xmit(sk, dev, buff, 1);
}


/* This routine builds a generic TCP header. */
static int
tcp_build_header(struct tcphdr *th, struct sock *sk, int push)
{

  /* FIXME: want to get rid of this. */
  memcpy(th,(void *) &(sk->dummy_th), sizeof(*th));
  th->seq = htonl(sk->write_seq);
  th->psh =(push == 0) ? 1 : 0;
  th->doff = sizeof(*th)/4;
  th->ack = 1;
  th->fin = 0;
  sk->ack_backlog = 0;
  sk->bytes_rcv = 0;
  sk->ack_timed = 0;
  th->ack_seq = htonl(sk->acked_seq);
  sk->window = tcp_select_window(sk)/*sk->prot->rspace(sk)*/;
  th->window = htons(sk->window);

  return(sizeof(*th));
}

/*
 * This routine copies from a user buffer into a socket,
 * and starts the transmit system.
 */
static int
tcp_write(struct sock *sk, unsigned char *from,
	  int len, int nonblock, unsigned flags)
{
  int copied = 0;
  int copy;
  int tmp;
  struct sk_buff *skb;
  struct sk_buff *send_tmp;
  unsigned char *buff;
  struct proto *prot;
  struct device *dev = NULL;

  DPRINTF((DBG_TCP, "tcp_write(sk=%X, from=%X, len=%d, nonblock=%d, flags=%X)\n",
					sk, from, len, nonblock, flags));

  sk->inuse=1;
  prot = sk->prot;
  while(len > 0) {
	if (sk->err) {			/* Stop on an error */
		release_sock(sk);
		if (copied) return(copied);
		tmp = -sk->err;
		sk->err = 0;
		return(tmp);
	}

	/* First thing we do is make sure that we are established. */	 
	if (sk->shutdown & SEND_SHUTDOWN) {
		release_sock(sk);
		sk->err = EPIPE;
		if (copied) return(copied);
		sk->err = 0;
		return(-EPIPE);
	}


	/* Wait for a connection to finish. */
	
	while(sk->state != TCP_ESTABLISHED && sk->state != TCP_CLOSE_WAIT) {
		if (sk->err) {
			release_sock(sk);
			if (copied) return(copied);
			tmp = -sk->err;
			sk->err = 0;
			return(tmp);
		}

		if (sk->state != TCP_SYN_SENT && sk->state != TCP_SYN_RECV) {
			release_sock(sk);
			DPRINTF((DBG_TCP, "tcp_write: return 1\n"));
			if (copied) return(copied);

			if (sk->err) {
				tmp = -sk->err;
				sk->err = 0;
				return(tmp);
			}

			if (sk->keepopen) {
				send_sig(SIGPIPE, current, 0);
			}
			return(-EPIPE);
		}

		if (nonblock || copied) {
			release_sock(sk);
			DPRINTF((DBG_TCP, "tcp_write: return 2\n"));
			if (copied) return(copied);
			return(-EAGAIN);
		}

		release_sock(sk);
		cli();
		if (sk->state != TCP_ESTABLISHED &&
		    sk->state != TCP_CLOSE_WAIT && sk->err == 0) {
			interruptible_sleep_on(sk->sleep);
			if (current->signal & ~current->blocked) {
				sti();
				DPRINTF((DBG_TCP, "tcp_write: return 3\n"));
				if (copied) return(copied);
				return(-ERESTARTSYS);
			}
		}
		sk->inuse = 1;
		sti();
	}

/*
 * The following code can result in copy <= if sk->mss is ever
 * decreased.  It shouldn't be.  sk->mss is min(sk->mtu, sk->max_window).
 * sk->mtu is constant once SYN processing is finished.  I.e. we
 * had better not get here until we've seen his SYN and at least one
 * valid ack.  (The SYN sets sk->mtu and the ack sets sk->max_window.)
 * But ESTABLISHED should guarantee that.  sk->max_window is by definition
 * non-decreasing.  Note that any ioctl to set user_mss must be done
 * before the exchange of SYN's.  If the initial ack from the other
 * end has a window of 0, max_window and thus mss will both be 0.
 */

	/* Now we need to check if we have a half built packet. */
	if ((skb = tcp_dequeue_partial(sk)) != NULL) {
	        int hdrlen;

	         /* IP header + TCP header */
		hdrlen = ((unsigned long)skb->h.th - (unsigned long)skb->data)
		         + sizeof(struct tcphdr);

		/* Add more stuff to the end of skb->len */
		if (!(flags & MSG_OOB)) {
			copy = min(sk->mss - (skb->len - hdrlen), len);
			/* FIXME: this is really a bug. */
			if (copy <= 0) {
			  printk("TCP: **bug**: \"copy\" <= 0!!\n");
			  copy = 0;
			}
	  
			memcpy_fromfs(skb->data + skb->len, from, copy);
			skb->len += copy;
			from += copy;
			copied += copy;
			len -= copy;
			sk->write_seq += copy;
		      }
		if ((skb->len - hdrlen) >= sk->mss ||
		    (flags & MSG_OOB) ||
		    !sk->packets_out)
			tcp_send_skb(sk, skb);
		else
			tcp_enqueue_partial(skb, sk);
		continue;
	}

	/*
	 * We also need to worry about the window.
 	 * If window < 1/2 the maximum window we've seen from this
 	 *   host, don't use it.  This is sender side
 	 *   silly window prevention, as specified in RFC1122.
 	 *   (Note that this is diffferent than earlier versions of
 	 *   SWS prevention, e.g. RFC813.).  What we actually do is 
	 *   use the whole MSS.  Since the results in the right
	 *   edge of the packet being outside the window, it will
	 *   be queued for later rather than sent.
	 */

	copy = sk->window_seq - sk->write_seq;
	if (copy <= 0 || copy < (sk->max_window >> 1) || copy > sk->mss)
		copy = sk->mss;
	if (copy > len)
		copy = len;

  /* We should really check the window here also. */
	send_tmp = NULL;
	if (copy < sk->mss && !(flags & MSG_OOB)) {
	/* We will release the socket incase we sleep here. */
	  release_sock(sk);
	  /* NB: following must be mtu, because mss can be increased.
	   * mss is always <= mtu */
	  skb = prot->wmalloc(sk, sk->mtu + 128 + prot->max_header + sizeof(*skb), 0, GFP_KERNEL);
	  sk->inuse = 1;
	  send_tmp = skb;
	} else {
		/* We will release the socket incase we sleep here. */
	  release_sock(sk);
	  skb = prot->wmalloc(sk, copy + prot->max_header + sizeof(*skb), 0, GFP_KERNEL);
	  sk->inuse = 1;
	}

	/* If we didn't get any memory, we need to sleep. */
	if (skb == NULL) {
		if (nonblock /* || copied */) {
			release_sock(sk);
			DPRINTF((DBG_TCP, "tcp_write: return 4\n"));
			if (copied) return(copied);
			return(-EAGAIN);
		}

		/* FIXME: here is another race condition. */
		tmp = sk->wmem_alloc;
		release_sock(sk);
		cli();
		/* Again we will try to avoid it. */
		if (tmp <= sk->wmem_alloc &&
		  (sk->state == TCP_ESTABLISHED||sk->state == TCP_CLOSE_WAIT)
				&& sk->err == 0) {
			interruptible_sleep_on(sk->sleep);
			if (current->signal & ~current->blocked) {
				sti();
				DPRINTF((DBG_TCP, "tcp_write: return 5\n"));
				if (copied) return(copied);
				return(-ERESTARTSYS);
			}
		}
		sk->inuse = 1;
		sti();
		continue;
	}

	skb->len = 0;
	skb->sk = sk;
	skb->free = 0;

	buff = skb->data;

	/*
	 * FIXME: we need to optimize this.
	 * Perhaps some hints here would be good.
	 */
	tmp = prot->build_header(skb, sk->saddr, sk->daddr, &dev,
				 IPPROTO_TCP, sk->opt, skb->mem_len,sk->ip_tos,sk->ip_ttl);
	if (tmp < 0 ) {
		prot->wfree(sk, skb->mem_addr, skb->mem_len);
		release_sock(sk);
		DPRINTF((DBG_TCP, "tcp_write: return 6\n"));
		if (copied) return(copied);
		return(tmp);
	}
	skb->len += tmp;
	skb->dev = dev;
	buff += tmp;
	skb->h.th =(struct tcphdr *) buff;
	tmp = tcp_build_header((struct tcphdr *)buff, sk, len-copy);
	if (tmp < 0) {
		prot->wfree(sk, skb->mem_addr, skb->mem_len);
		release_sock(sk);
		DPRINTF((DBG_TCP, "tcp_write: return 7\n"));
		if (copied) return(copied);
		return(tmp);
	}

	if (flags & MSG_OOB) {
		((struct tcphdr *)buff)->urg = 1;
		((struct tcphdr *)buff)->urg_ptr = ntohs(copy);
	}
	skb->len += tmp;
	memcpy_fromfs(buff+tmp, from, copy);

	from += copy;
	copied += copy;
	len -= copy;
	skb->len += copy;
	skb->free = 0;
	sk->write_seq += copy;

	if (send_tmp != NULL && sk->packets_out) {
		tcp_enqueue_partial(send_tmp, sk);
		continue;
	}
	tcp_send_skb(sk, skb);
  }
  sk->err = 0;

/*
 *	Nagles rule. Turn Nagle off with TCP_NODELAY for highly
 *	interactive fast network servers. It's meant to be on and
 *	it really improves the throughput though not the echo time
 *	on my slow slip link - Alan
 */

  /* Avoid possible race on send_tmp - c/o Johannes Stille */
  if(sk->partial && 
     ((!sk->packets_out) 
     /* If not nagling we can send on the before case too.. */
      || (sk->nonagle && before(sk->write_seq , sk->window_seq))
      ))
  	tcp_send_partial(sk);
  /* -- */
  release_sock(sk);
  DPRINTF((DBG_TCP, "tcp_write: return 8\n"));
  return(copied);
}


static int
tcp_sendto(struct sock *sk, unsigned char *from,
	   int len, int nonblock, unsigned flags,
	   struct sockaddr_in *addr, int addr_len)
{
  struct sockaddr_in sin;

  if (addr_len < sizeof(sin)) return(-EINVAL);
  memcpy_fromfs(&sin, addr, sizeof(sin));
  if (sin.sin_family && sin.sin_family != AF_INET) return(-EINVAL);
  if (sin.sin_port != sk->dummy_th.dest) return(-EINVAL);
  if (sin.sin_addr.s_addr != sk->daddr) return(-EINVAL);
  return(tcp_write(sk, from, len, nonblock, flags));
}


static void
tcp_read_wakeup(struct sock *sk)
{
  int tmp;
  struct device *dev = NULL;
  struct tcphdr *t1;
  struct sk_buff *buff;

  DPRINTF((DBG_TCP, "in tcp read wakeup\n"));
  if (!sk->ack_backlog) return;

  /*
   * FIXME: we need to put code here to prevent this routine from
   * being called.  Being called once in a while is ok, so only check
   * if this is the second time in a row.
   */

  /*
   * We need to grab some memory, and put together an ack,
   * and then put it into the queue to be sent.
   */
  buff = sk->prot->wmalloc(sk,MAX_ACK_SIZE,1, GFP_ATOMIC);
  if (buff == NULL) {
	/* Try again real soon. */
	reset_timer(sk, TIME_WRITE, 10);
	return;
  }

  buff->mem_addr = buff;
  buff->mem_len = MAX_ACK_SIZE;
  buff->len = sizeof(struct tcphdr);
  buff->sk = sk;

  /* Put in the IP header and routing stuff. */
  tmp = sk->prot->build_header(buff, sk->saddr, sk->daddr, &dev,
			       IPPROTO_TCP, sk->opt, MAX_ACK_SIZE,sk->ip_tos,sk->ip_ttl);
  if (tmp < 0) {
  	buff->free=1;
	sk->prot->wfree(sk, buff->mem_addr, buff->mem_len);
	return;
  }

  buff->len += tmp;
  t1 =(struct tcphdr *)(buff->data +tmp);

  memcpy(t1,(void *) &sk->dummy_th, sizeof(*t1));
  t1->seq = htonl(sk->sent_seq);
  t1->ack = 1;
  t1->res1 = 0;
  t1->res2 = 0;
  t1->rst = 0;
  t1->urg = 0;
  t1->syn = 0;
  t1->psh = 0;
  sk->ack_backlog = 0;
  sk->bytes_rcv = 0;
  sk->window = tcp_select_window(sk);/*sk->prot->rspace(sk);*/
  t1->window = ntohs(sk->window);
  t1->ack_seq = ntohl(sk->acked_seq);
  t1->doff = sizeof(*t1)/4;
  tcp_send_check(t1, sk->saddr, sk->daddr, sizeof(*t1), sk);
  sk->prot->queue_xmit(sk, dev, buff, 1);
}


/*
 * FIXME:
 * This routine frees used buffers.
 * It should consider sending an ACK to let the
 * other end know we now have a bigger window.
 */
static void
cleanup_rbuf(struct sock *sk)
{
  unsigned long flags;
  int left;
  struct sk_buff *skb;

  if(sk->debug)
  	printk("cleaning rbuf for sk=%p\n", sk);
  
  save_flags(flags);
  cli();
  
  left = sk->prot->rspace(sk);
 
  /*
   * We have to loop through all the buffer headers,
   * and try to free up all the space we can.
   */
  while((skb=skb_peek(&sk->rqueue)) != NULL ) 
  {
	if (!skb->used) 
		break;
	skb_unlink(skb);
	skb->sk = sk;
	kfree_skb(skb, FREE_READ);
  }

  restore_flags(flags);

  /*
   * FIXME:
   * At this point we should send an ack if the difference
   * in the window, and the amount of space is bigger than
   * TCP_WINDOW_DIFF.
   */
  DPRINTF((DBG_TCP, "sk->window left = %d, sk->prot->rspace(sk)=%d\n",
			sk->window - sk->bytes_rcv, sk->prot->rspace(sk)));

  if(sk->debug)
  	printk("sk->rspace = %lu, was %d\n", sk->prot->rspace(sk),
  					    left);
  if (sk->prot->rspace(sk) != left) 
  {
	/*
	 * This area has caused the most trouble.  The current strategy
	 * is to simply do nothing if the other end has room to send at
	 * least 3 full packets, because the ack from those will auto-
	 * matically update the window.  If the other end doesn't think
	 * we have much space left, but we have room for atleast 1 more
	 * complete packet than it thinks we do, we will send an ack
	 * immediatedly.  Otherwise we will wait up to .5 seconds in case
	 * the user reads some more.
	 */
	sk->ack_backlog++;
/*
 * It's unclear whether to use sk->mtu or sk->mss here.  They differ only
 * if the other end is offering a window smaller than the agreed on MSS
 * (called sk->mtu here).  In theory there's no connection between send
 * and receive, and so no reason to think that they're going to send
 * small packets.  For the moment I'm using the hack of reducing the mss
 * only on the send side, so I'm putting mtu here.
 */
	if ((sk->prot->rspace(sk) > (sk->window - sk->bytes_rcv + sk->mtu))) {
		/* Send an ack right now. */
		tcp_read_wakeup(sk);
	} else {
		/* Force it to send an ack soon. */
		int was_active = del_timer(&sk->timer);
		if (!was_active || TCP_ACK_TIME < sk->timer.expires) {
			reset_timer(sk, TIME_WRITE, TCP_ACK_TIME);
		} else
			add_timer(&sk->timer);
	}
  }
} 


/* Handle reading urgent data. */
static int
tcp_read_urg(struct sock * sk, int nonblock,
	     unsigned char *to, int len, unsigned flags)
{
	struct wait_queue wait = { current, NULL };

	while (len > 0) {
		if (sk->urginline || !sk->urg_data || sk->urg_data == URG_READ)
			return -EINVAL;
		if (sk->urg_data & URG_VALID) {
			char c = sk->urg_data;
			if (!(flags & MSG_PEEK))
				sk->urg_data = URG_READ;
			put_fs_byte(c, to);
			return 1;
		}

		if (sk->err) {
			int tmp = -sk->err;
			sk->err = 0;
			return tmp;
		}

		if (sk->state == TCP_CLOSE || sk->done) {
			if (!sk->done) {
				sk->done = 1;
				return 0;
			}
			return -ENOTCONN;
		}

		if (sk->shutdown & RCV_SHUTDOWN) {
			sk->done = 1;
			return 0;
		}

		if (nonblock)
			return -EAGAIN;

		if (current->signal & ~current->blocked)
			return -ERESTARTSYS;

		current->state = TASK_INTERRUPTIBLE;
		add_wait_queue(sk->sleep, &wait);
		if ((sk->urg_data & URG_NOTYET) && sk->err == 0 &&
		    !(sk->shutdown & RCV_SHUTDOWN))
			schedule();
		remove_wait_queue(sk->sleep, &wait);
		current->state = TASK_RUNNING;
	}
	return 0;
}


/* This routine copies from a sock struct into the user buffer. */
static int tcp_read(struct sock *sk, unsigned char *to,
	int len, int nonblock, unsigned flags)
{
	struct wait_queue wait = { current, NULL };
	int copied = 0;
	unsigned long peek_seq;
	unsigned long *seq;
	unsigned long used;
	int err;

	if (len == 0)
		return 0;

	if (len < 0)
		return -EINVAL;

	err = verify_area(VERIFY_WRITE, to, len);
	if (err)
		return err;

	/* This error should be checked. */
	if (sk->state == TCP_LISTEN)
		return -ENOTCONN;

	/* Urgent data needs to be handled specially. */
	if (flags & MSG_OOB)
		return tcp_read_urg(sk, nonblock, to, len, flags);

	peek_seq = sk->copied_seq;
	seq = &sk->copied_seq;
	if (flags & MSG_PEEK)
		seq = &peek_seq;

	add_wait_queue(sk->sleep, &wait);
	sk->inuse = 1;
	while (len > 0) {
		struct sk_buff * skb;
		unsigned long offset;
	
		/*
		 * are we at urgent data? Stop if we have read anything.
		 */
		if (copied && sk->urg_data && sk->urg_seq == 1+*seq)
			break;

		current->state = TASK_INTERRUPTIBLE;

		skb = sk->rqueue;
		do {
			if (!skb)
				break;
			if (before(1+*seq, skb->h.th->seq))
				break;
			offset = 1 + *seq - skb->h.th->seq;
			if (skb->h.th->syn)
				offset--;
			if (offset < skb->len)
				goto found_ok_skb;
			if (!(flags & MSG_PEEK))
				skb->used = 1;
			skb = (struct sk_buff *)skb->next;
		} while (skb != sk->rqueue);

		if (copied)
			break;

		if (sk->err) {
			copied = -sk->err;
			sk->err = 0;
			break;
		}

		if (sk->state == TCP_CLOSE) {
			if (!sk->done) {
				sk->done = 1;
				break;
			}
			copied = -ENOTCONN;
			break;
		}

		if (sk->shutdown & RCV_SHUTDOWN) {
			sk->done = 1;
			break;
		}
			
		if (nonblock) {
			copied = -EAGAIN;
			break;
		}

		cleanup_rbuf(sk);
		release_sock(sk);
		schedule();
		sk->inuse = 1;

		if (current->signal & ~current->blocked) {
			copied = -ERESTARTSYS;
			break;
		}
		continue;

	found_ok_skb:
		/* Ok so how much can we use ? */
		used = skb->len - offset;
		if (len < used)
			used = len;
		/* do we have urgent data here? */
		if (sk->urg_data) {
			unsigned long urg_offset = sk->urg_seq - (1 + *seq);
			if (urg_offset < used) {
				if (!urg_offset) {
					if (!sk->urginline) {
						++*seq;
						offset++;
						used--;
					}
				} else
					used = urg_offset;
			}
		}
		/* Copy it */
		memcpy_tofs(to,((unsigned char *)skb->h.th) +
			skb->h.th->doff*4 + offset, used);
		copied += used;
		len -= used;
		to += used;
		*seq += used;
		if (after(sk->copied_seq+1,sk->urg_seq))
			sk->urg_data = 0;
		if (!(flags & MSG_PEEK) && (used + offset >= skb->len))
			skb->used = 1;
	}
	remove_wait_queue(sk->sleep, &wait);
	current->state = TASK_RUNNING;

	/* Clean up data we have read: This will do ACK frames */
	cleanup_rbuf(sk);
	release_sock(sk);
	DPRINTF((DBG_TCP, "tcp_read: returning %d\n", copied));
	return copied;
}

 
/*
 * Send a FIN without closing the connection.
 * Not called at interrupt time.
 */
void
tcp_shutdown(struct sock *sk, int how)
{
  struct sk_buff *buff;
  struct tcphdr *t1, *th;
  struct proto *prot;
  int tmp;
  struct device *dev = NULL;

  /*
   * We need to grab some memory, and put together a FIN,
   * and then put it into the queue to be sent.
   * FIXME:
   *	Tim MacKenzie(tym@dibbler.cs.monash.edu.au) 4 Dec '92.
   *	Most of this is guesswork, so maybe it will work...
   */
  /* If we've already sent a FIN, return. */
  if (sk->state == TCP_FIN_WAIT1 || sk->state == TCP_FIN_WAIT2) return;
  if (!(how & SEND_SHUTDOWN)) return;
  sk->inuse = 1;

  /* Clear out any half completed packets. */
  if (sk->partial)
	tcp_send_partial(sk);

  prot =(struct proto *)sk->prot;
  th =(struct tcphdr *)&sk->dummy_th;
  release_sock(sk); /* incase the malloc sleeps. */
  buff = prot->wmalloc(sk, MAX_RESET_SIZE,1 , GFP_KERNEL);
  if (buff == NULL) return;
  sk->inuse = 1;

  DPRINTF((DBG_TCP, "tcp_shutdown_send buff = %X\n", buff));
  buff->mem_addr = buff;
  buff->mem_len = MAX_RESET_SIZE;
  buff->sk = sk;
  buff->len = sizeof(*t1);
  t1 =(struct tcphdr *) buff->data;

  /* Put in the IP header and routing stuff. */
  tmp = prot->build_header(buff,sk->saddr, sk->daddr, &dev,
			   IPPROTO_TCP, sk->opt,
			   sizeof(struct tcphdr),sk->ip_tos,sk->ip_ttl);
  if (tmp < 0) {
  	buff->free=1;
	prot->wfree(sk,buff->mem_addr, buff->mem_len);
	release_sock(sk);
	DPRINTF((DBG_TCP, "Unable to build header for fin.\n"));
	return;
  }

  t1 =(struct tcphdr *)((char *)t1 +tmp);
  buff->len += tmp;
  buff->dev = dev;
  memcpy(t1, th, sizeof(*t1));
  t1->seq = ntohl(sk->write_seq);
  sk->write_seq++;
  buff->h.seq = sk->write_seq;
  t1->ack = 1;
  t1->ack_seq = ntohl(sk->acked_seq);
  t1->window = ntohs(sk->window=tcp_select_window(sk)/*sk->prot->rspace(sk)*/);
  t1->fin = 1;
  t1->rst = 0;
  t1->doff = sizeof(*t1)/4;
  tcp_send_check(t1, sk->saddr, sk->daddr, sizeof(*t1), sk);

  /*
   * Can't just queue this up.
   * It should go at the end of the write queue.
   */
  if (sk->wback != NULL) {
  	buff->free=0;	
	buff->next = NULL;
	sk->wback->next = buff;
	sk->wback = buff;
	buff->magic = TCP_WRITE_QUEUE_MAGIC;
  } else {
        sk->sent_seq = sk->write_seq;
	sk->prot->queue_xmit(sk, dev, buff, 0);
  }

  if (sk->state == TCP_ESTABLISHED) sk->state = TCP_FIN_WAIT1;
    else sk->state = TCP_FIN_WAIT2;

  release_sock(sk);
}


static int
tcp_recvfrom(struct sock *sk, unsigned char *to,
	     int to_len, int nonblock, unsigned flags,
	     struct sockaddr_in *addr, int *addr_len)
{
  struct sockaddr_in sin;
  int len;
  int err;
  int result;
  
  /* Have to check these first unlike the old code. If 
     we check them after we lose data on an error
     which is wrong */
  err = verify_area(VERIFY_WRITE,addr_len,sizeof(long));
  if(err)
  	return err;
  len = get_fs_long(addr_len);
  if(len > sizeof(sin))
  	len = sizeof(sin);
  err=verify_area(VERIFY_WRITE, addr, len);  
  if(err)
  	return err;
  	
  result=tcp_read(sk, to, to_len, nonblock, flags);

  if (result < 0) return(result);
  
  sin.sin_family = AF_INET;
  sin.sin_port = sk->dummy_th.dest;
  sin.sin_addr.s_addr = sk->daddr;

  memcpy_tofs(addr, &sin, len);
  put_fs_long(len, addr_len);
  return(result);
}


/* This routine will send an RST to the other tcp. */
static void
tcp_reset(unsigned long saddr, unsigned long daddr, struct tcphdr *th,
	  struct proto *prot, struct options *opt, struct device *dev, int tos, int ttl)
{
  struct sk_buff *buff;
  struct tcphdr *t1;
  int tmp;

  /*
   * We need to grab some memory, and put together an RST,
   * and then put it into the queue to be sent.
   */
  buff = prot->wmalloc(NULL, MAX_RESET_SIZE, 1, GFP_ATOMIC);
  if (buff == NULL) 
  	return;

  DPRINTF((DBG_TCP, "tcp_reset buff = %X\n", buff));
  buff->mem_addr = buff;
  buff->mem_len = MAX_RESET_SIZE;
  buff->len = sizeof(*t1);
  buff->sk = NULL;
  buff->dev = dev;

  t1 =(struct tcphdr *) buff->data;

  /* Put in the IP header and routing stuff. */
  tmp = prot->build_header(buff, saddr, daddr, &dev, IPPROTO_TCP, opt,
			   sizeof(struct tcphdr),tos,ttl);
  if (tmp < 0) {
  	buff->free = 1;
	prot->wfree(NULL, buff->mem_addr, buff->mem_len);
	return;
  }
  t1 =(struct tcphdr *)((char *)t1 +tmp);
  buff->len += tmp;
  memcpy(t1, th, sizeof(*t1));

  /* Swap the send and the receive. */
  t1->dest = th->source;
  t1->source = th->dest;
  t1->rst = 1;  
  t1->window = 0;
  
  if(th->ack)
  {
  	t1->ack = 0;
  	t1->seq = th->ack_seq;
  	t1->ack_seq = 0;
  }
  else
  {
  	t1->ack = 1;
  	if(!th->syn)
  		t1->ack_seq=htonl(th->seq);
  	else
  		t1->ack_seq=htonl(th->seq+1);
  	t1->seq=0;
  }

  t1->syn = 0;
  t1->urg = 0;
  t1->fin = 0;
  t1->psh = 0;
  t1->doff = sizeof(*t1)/4;
  tcp_send_check(t1, saddr, daddr, sizeof(*t1), NULL);
  prot->queue_xmit(NULL, dev, buff, 1);
}


/*
 *	Look for tcp options. Parses everything but only knows about MSS.
 *      This routine is always called with the packet containing the SYN.
 *      However it may also be called with the ack to the SYN.  So you
 *      can't assume this is always the SYN.  It's always called after
 *      we have set up sk->mtu to our own MTU.
 */
 
static void
tcp_options(struct sock *sk, struct tcphdr *th)
{
  unsigned char *ptr;
  int length=(th->doff*4)-sizeof(struct tcphdr);
  int mss_seen = 0;
    
  ptr = (unsigned char *)(th + 1);
  
  while(length>0)
  {
  	int opcode=*ptr++;
  	int opsize=*ptr++;
  	switch(opcode)
  	{
  		case TCPOPT_EOL:
  			return;
  		case TCPOPT_NOP:
  			length-=2;
  			continue;
  		
  		default:
  			if(opsize<=2)	/* Avoid silly options looping forever */
  				return;
  			switch(opcode)
  			{
  				case TCPOPT_MSS:
  					if(opsize==4 && th->syn)
  					{
  						sk->mtu=min(sk->mtu,ntohs(*(unsigned short *)ptr));
						mss_seen = 1;
  					}
  					break;
  				/* Add other options here as people feel the urge to implement stuff like large windows */
  			}
  			ptr+=opsize-2;
  			length-=opsize;
  	}
  }
  if (th->syn) {
    if (! mss_seen)
      sk->mtu=min(sk->mtu, 536);  /* default MSS if none sent */
  }
  sk->mss = min(sk->max_window, sk->mtu);
}

static inline unsigned long default_mask(unsigned long dst)
{
	dst = ntohl(dst);
	if (IN_CLASSA(dst))
		return htonl(IN_CLASSA_NET);
	if (IN_CLASSB(dst))
		return htonl(IN_CLASSB_NET);
	return htonl(IN_CLASSC_NET);
}

/*
 * This routine handles a connection request.
 * It should make sure we haven't already responded.
 * Because of the way BSD works, we have to send a syn/ack now.
 * This also means it will be harder to close a socket which is
 * listening.
 */
static void
tcp_conn_request(struct sock *sk, struct sk_buff *skb,
		 unsigned long daddr, unsigned long saddr,
		 struct options *opt, struct device *dev)
{
  struct sk_buff *buff;
  struct tcphdr *t1;
  unsigned char *ptr;
  struct sock *newsk;
  struct tcphdr *th;
  int tmp;

  DPRINTF((DBG_TCP, "tcp_conn_request(sk = %X, skb = %X, daddr = %X, sadd4= %X, \n"
	  "                  opt = %X, dev = %X)\n",
	  sk, skb, daddr, saddr, opt, dev));
  
  th = skb->h.th;

  /* If the socket is dead, don't accept the connection. */
  if (!sk->dead) {
  	sk->data_ready(sk,0);
  } else {
	DPRINTF((DBG_TCP, "tcp_conn_request on dead socket\n"));
	tcp_reset(daddr, saddr, th, sk->prot, opt, dev, sk->ip_tos,sk->ip_ttl);
	kfree_skb(skb, FREE_READ);
	return;
  }

  /*
   * Make sure we can accept more.  This will prevent a
   * flurry of syns from eating up all our memory.
   */
  if (sk->ack_backlog >= sk->max_ack_backlog) {
	kfree_skb(skb, FREE_READ);
	return;
  }

  /*
   * We need to build a new sock struct.
   * It is sort of bad to have a socket without an inode attached
   * to it, but the wake_up's will just wake up the listening socket,
   * and if the listening socket is destroyed before this is taken
   * off of the queue, this will take care of it.
   */
  newsk = (struct sock *) kmalloc(sizeof(struct sock), GFP_ATOMIC);
  if (newsk == NULL) {
	/* just ignore the syn.  It will get retransmitted. */
	kfree_skb(skb, FREE_READ);
	return;
  }

  DPRINTF((DBG_TCP, "newsk = %X\n", newsk));
  memcpy((void *)newsk,(void *)sk, sizeof(*newsk));
  newsk->wback = NULL;
  newsk->wfront = NULL;
  newsk->rqueue = NULL;
  newsk->send_head = NULL;
  newsk->send_tail = NULL;
  newsk->back_log = NULL;
  newsk->rtt = TCP_CONNECT_TIME << 3;
  newsk->rto = TCP_CONNECT_TIME;
  newsk->mdev = 0;
  newsk->max_window = 0;
  newsk->cong_window = 1;
  newsk->cong_count = 0;
  newsk->ssthresh = 0;
  newsk->backoff = 0;
  newsk->blog = 0;
  newsk->intr = 0;
  newsk->proc = 0;
  newsk->done = 0;
  newsk->partial = NULL;
  newsk->pair = NULL;
  newsk->wmem_alloc = 0;
  newsk->rmem_alloc = 0;

  newsk->max_unacked = MAX_WINDOW - TCP_WINDOW_DIFF;

  newsk->err = 0;
  newsk->shutdown = 0;
  newsk->ack_backlog = 0;
  newsk->acked_seq = skb->h.th->seq+1;
  newsk->fin_seq = skb->h.th->seq;
  newsk->copied_seq = skb->h.th->seq;
  newsk->state = TCP_SYN_RECV;
  newsk->timeout = 0;
  newsk->write_seq = jiffies * SEQ_TICK - seq_offset;
  newsk->window_seq = newsk->write_seq;
  newsk->rcv_ack_seq = newsk->write_seq;
  newsk->urg_data = 0;
  newsk->retransmits = 0;
  newsk->destroy = 0;
  newsk->timer.data = (unsigned long)newsk;
  newsk->timer.function = &net_timer;
  newsk->dummy_th.source = skb->h.th->dest;
  newsk->dummy_th.dest = skb->h.th->source;

  /* Swap these two, they are from our point of view. */
  newsk->daddr = saddr;
  newsk->saddr = daddr;

  put_sock(newsk->num,newsk);
  newsk->dummy_th.res1 = 0;
  newsk->dummy_th.doff = 6;
  newsk->dummy_th.fin = 0;
  newsk->dummy_th.syn = 0;
  newsk->dummy_th.rst = 0;
  newsk->dummy_th.psh = 0;
  newsk->dummy_th.ack = 0;
  newsk->dummy_th.urg = 0;
  newsk->dummy_th.res2 = 0;
  newsk->acked_seq = skb->h.th->seq + 1;
  newsk->copied_seq = skb->h.th->seq;

  /* Grab the ttl and tos values and use them */
  newsk->ip_ttl=sk->ip_ttl;
  newsk->ip_tos=skb->ip_hdr->tos;

/* use 512 or whatever user asked for */
/* note use of sk->user_mss, since user has no direct access to newsk */
  if (sk->user_mss)
    newsk->mtu = sk->user_mss;
  else {
#ifdef SUBNETSARELOCAL
    if ((saddr ^ daddr) & default_mask(saddr))
#else
    if ((saddr ^ daddr) & dev->pa_mask)
#endif
      newsk->mtu = 576 - HEADER_SIZE;
    else
      newsk->mtu = MAX_WINDOW;
  }
/* but not bigger than device MTU */
  newsk->mtu = min(newsk->mtu, dev->mtu - HEADER_SIZE);

/* this will min with what arrived in the packet */
  tcp_options(newsk,skb->h.th);

  buff = newsk->prot->wmalloc(newsk, MAX_SYN_SIZE, 1, GFP_ATOMIC);
  if (buff == NULL) {
	sk->err = -ENOMEM;
	newsk->dead = 1;
	release_sock(newsk);
	kfree_skb(skb, FREE_READ);
	return;
  }
  
  buff->mem_addr = buff;
  buff->mem_len = MAX_SYN_SIZE;
  buff->len = sizeof(struct tcphdr)+4;
  buff->sk = newsk;
  
  t1 =(struct tcphdr *) buff->data;

  /* Put in the IP header and routing stuff. */
  tmp = sk->prot->build_header(buff, newsk->saddr, newsk->daddr, &dev,
			       IPPROTO_TCP, NULL, MAX_SYN_SIZE,sk->ip_tos,sk->ip_ttl);

  /* Something went wrong. */
  if (tmp < 0) {
	sk->err = tmp;
	buff->free=1;
	kfree_skb(buff,FREE_WRITE);
	newsk->dead = 1;
	release_sock(newsk);
	skb->sk = sk;
	kfree_skb(skb, FREE_READ);
	return;
  }

  buff->len += tmp;
  t1 =(struct tcphdr *)((char *)t1 +tmp);
  
  memcpy(t1, skb->h.th, sizeof(*t1));
  buff->h.seq = newsk->write_seq;

  /* Swap the send and the receive. */
  t1->dest = skb->h.th->source;
  t1->source = newsk->dummy_th.source;
  t1->seq = ntohl(newsk->write_seq++);
  t1->ack = 1;
  newsk->window = tcp_select_window(newsk);/*newsk->prot->rspace(newsk);*/
  newsk->sent_seq = newsk->write_seq;
  t1->window = ntohs(newsk->window);
  t1->res1 = 0;
  t1->res2 = 0;
  t1->rst = 0;
  t1->urg = 0;
  t1->psh = 0;
  t1->syn = 1;
  t1->ack_seq = ntohl(skb->h.th->seq+1);
  t1->doff = sizeof(*t1)/4+1;

  ptr =(unsigned char *)(t1+1);
  ptr[0] = 2;
  ptr[1] = 4;
  ptr[2] = ((newsk->mtu) >> 8) & 0xff;
  ptr[3] =(newsk->mtu) & 0xff;

  tcp_send_check(t1, daddr, saddr, sizeof(*t1)+4, newsk);
  newsk->prot->queue_xmit(newsk, dev, buff, 0);

  reset_timer(newsk, TIME_WRITE /* -1 ? FIXME ??? */, TCP_CONNECT_TIME);
  skb->sk = newsk;

  /* Charge the sock_buff to newsk. */
  sk->rmem_alloc -= skb->mem_len;
  newsk->rmem_alloc += skb->mem_len;

  skb_queue_tail(&sk->rqueue,skb);
  sk->ack_backlog++;
  release_sock(newsk);
}


static void
tcp_close(struct sock *sk, int timeout)
{
  struct sk_buff *buff;
  int need_reset = 0;
  struct tcphdr *t1, *th;
  struct proto *prot;
  struct device *dev=NULL;
  int tmp;

  /*
   * We need to grab some memory, and put together a FIN,
   * and then put it into the queue to be sent.
   */
  DPRINTF((DBG_TCP, "tcp_close((struct sock *)%X, %d)\n",sk, timeout));
  sk->inuse = 1;
  sk->keepopen = 1;
  sk->shutdown = SHUTDOWN_MASK;

  if (!sk->dead) 
  	sk->state_change(sk);

  /* We need to flush the recv. buffs. */
  if (skb_peek(&sk->rqueue) != NULL) 
  {
	struct sk_buff *skb;
	if(sk->debug)
		printk("Clean rcv queue\n");
	while((skb=skb_dequeue(&sk->rqueue))!=NULL)
	{
		if(skb->len > 0 && after(skb->h.th->seq + skb->len + 1 , sk->copied_seq))
				need_reset = 1;
		kfree_skb(skb, FREE_READ);
	}
	if(sk->debug)
		printk("Cleaned.\n");
  }
  sk->rqueue = NULL;

  /* Get rid off any half-completed packets. */
  if (sk->partial) {
	tcp_send_partial(sk);
  }

  switch(sk->state) {
	case TCP_FIN_WAIT1:
	case TCP_FIN_WAIT2:
	case TCP_LAST_ACK:
		/* start a timer. */
                /* original code was 4 * sk->rtt.  In converting to the
		 * new rtt representation, we can't quite use that.
		 * it seems to make most sense to  use the backed off value
		 */
		reset_timer(sk, TIME_CLOSE, 4 * sk->rto);
		if (timeout) tcp_time_wait(sk);
		release_sock(sk);
		return;	/* break causes a double release - messy */
	case TCP_TIME_WAIT:
		if (timeout) {
		  sk->state = TCP_CLOSE;
		}
		release_sock(sk);
		return;
	case TCP_LISTEN:
		sk->state = TCP_CLOSE;
		release_sock(sk);
		return;
	case TCP_CLOSE:
		release_sock(sk);
		return;
	case TCP_CLOSE_WAIT:
	case TCP_ESTABLISHED:
	case TCP_SYN_SENT:
	case TCP_SYN_RECV:
		prot =(struct proto *)sk->prot;
		th =(struct tcphdr *)&sk->dummy_th;
		buff = prot->wmalloc(sk, MAX_FIN_SIZE, 1, GFP_ATOMIC);
		if (buff == NULL) {
			/* This will force it to try again later. */
			/* Or it would have if someone released the socket
			   first. Anyway it might work now */
			release_sock(sk);
			if (sk->state != TCP_CLOSE_WAIT)
					sk->state = TCP_ESTABLISHED;
			reset_timer(sk, TIME_CLOSE, 100);
			return;
		}
		buff->mem_addr = buff;
		buff->mem_len = MAX_FIN_SIZE;
		buff->sk = sk;
		buff->free = 1;
		buff->len = sizeof(*t1);
		t1 =(struct tcphdr *) buff->data;

		/* Put in the IP header and routing stuff. */
		tmp = prot->build_header(buff,sk->saddr, sk->daddr, &dev,
					 IPPROTO_TCP, sk->opt,
				         sizeof(struct tcphdr),sk->ip_tos,sk->ip_ttl);
		if (tmp < 0) {
			kfree_skb(buff,FREE_WRITE);
			DPRINTF((DBG_TCP, "Unable to build header for fin.\n"));
			release_sock(sk);
			return;
		}

		t1 =(struct tcphdr *)((char *)t1 +tmp);
		buff->len += tmp;
		buff->dev = dev;
		memcpy(t1, th, sizeof(*t1));
		t1->seq = ntohl(sk->write_seq);
		sk->write_seq++;
		buff->h.seq = sk->write_seq;
		t1->ack = 1;

		/* Ack everything immediately from now on. */
		sk->delay_acks = 0;
		t1->ack_seq = ntohl(sk->acked_seq);
		t1->window = ntohs(sk->window=tcp_select_window(sk)/*sk->prot->rspace(sk)*/);
		t1->fin = 1;
		t1->rst = need_reset;
		t1->doff = sizeof(*t1)/4;
		tcp_send_check(t1, sk->saddr, sk->daddr, sizeof(*t1), sk);

		if (sk->wfront == NULL) {
			sk->sent_seq = sk->write_seq;
			prot->queue_xmit(sk, dev, buff, 0);
		} else {
			reset_timer(sk, TIME_WRITE, sk->rto);
			buff->next = NULL;
			if (sk->wback == NULL) {
				sk->wfront = buff;
			} else {
				sk->wback->next = buff;
			}
			sk->wback = buff;
			buff->magic = TCP_WRITE_QUEUE_MAGIC;
		}

		if (sk->state == TCP_CLOSE_WAIT) {
			sk->state = TCP_FIN_WAIT2;
		} else {
			sk->state = TCP_FIN_WAIT1;
	}
  }
  release_sock(sk);
}


/*
 * This routine takes stuff off of the write queue,
 * and puts it in the xmit queue.
 */
static void
tcp_write_xmit(struct sock *sk)
{
  struct sk_buff *skb;

  DPRINTF((DBG_TCP, "tcp_write_xmit(sk=%X)\n", sk));

  /* The bytes will have to remain here. In time closedown will
     empty the write queue and all will be happy */
  if(sk->zapped)
	return;

  while(sk->wfront != NULL &&
        before(sk->wfront->h.seq, sk->window_seq +1) &&
	(sk->retransmits == 0 ||
	 sk->timeout != TIME_WRITE ||
	 before(sk->wfront->h.seq, sk->rcv_ack_seq +1))
        && sk->packets_out < sk->cong_window) {
		skb = sk->wfront;
		IS_SKB(skb);
		sk->wfront = skb->next;
		if (sk->wfront == NULL) sk->wback = NULL;
		skb->next = NULL;
		if (skb->magic != TCP_WRITE_QUEUE_MAGIC) {
			printk("tcp.c skb with bad magic(%X) on write queue. Squashing "
				"queue\n", skb->magic);
			sk->wfront = NULL;
			sk->wback = NULL;
			return;
		}
		skb->magic = 0;
		DPRINTF((DBG_TCP, "Sending a packet.\n"));

		/* See if we really need to send the packet. */
		if (before(skb->h.seq, sk->rcv_ack_seq +1)) {
			sk->retransmits = 0;
			kfree_skb(skb, FREE_WRITE);
			if (!sk->dead) sk->write_space(sk);
		} else {
			sk->sent_seq = skb->h.seq;
			sk->prot->queue_xmit(sk, skb->dev, skb, skb->free);
		}
	}
}


/*
 * This routine sorts the send list, and resets the
 * sk->send_head and sk->send_tail pointers.
 */
void
sort_send(struct sock *sk)
{
  struct sk_buff *list = NULL;
  struct sk_buff *skb,*skb2,*skb3;

  for (skb = sk->send_head; skb != NULL; skb = skb2) {
	skb2 = (struct sk_buff *)skb->link3;
	if (list == NULL || before (skb2->h.seq, list->h.seq)) {
		skb->link3 = list;
		sk->send_tail = skb;
		list = skb;
	} else {
		for (skb3 = list; ; skb3 = (struct sk_buff *)skb3->link3) {
			if (skb3->link3 == NULL ||
			    before(skb->h.seq, skb3->link3->h.seq)) {
				skb->link3 = skb3->link3;
				skb3->link3 = skb;
				if (skb->link3 == NULL) sk->send_tail = skb;
				break;
			}
		}
	}
  }
  sk->send_head = list;
}
  

/* This routine deals with incoming acks, but not outgoing ones. */
static int
tcp_ack(struct sock *sk, struct tcphdr *th, unsigned long saddr, int len)
{
  unsigned long ack;
  int flag = 0;
  /* 
   * 1 - there was data in packet as well as ack or new data is sent or 
   *     in shutdown state
   * 2 - data from retransmit queue was acked and removed
   * 4 - window shrunk or data from retransmit queue was acked and removed
   */

  if(sk->zapped)
	return(1);	/* Dead, cant ack any more so why bother */

  ack = ntohl(th->ack_seq);
  DPRINTF((DBG_TCP, "tcp_ack ack=%d, window=%d, "
	  "sk->rcv_ack_seq=%d, sk->window_seq = %d\n",
	  ack, ntohs(th->window), sk->rcv_ack_seq, sk->window_seq));

  if (ntohs(th->window) > sk->max_window) {
  	sk->max_window = ntohs(th->window);
	sk->mss = min(sk->max_window, sk->mtu);
  }

  if (sk->retransmits && sk->timeout == TIME_KEEPOPEN)
  	sk->retransmits = 0;

/* not quite clear why the +1 and -1 here, and why not +1 in next line */
  if (after(ack, sk->sent_seq+1) || before(ack, sk->rcv_ack_seq-1)) {
	if (after(ack, sk->sent_seq) ||
	   (sk->state != TCP_ESTABLISHED && sk->state != TCP_CLOSE_WAIT)) {
		return(0);
	}
	if (sk->keepopen) {
		reset_timer(sk, TIME_KEEPOPEN, TCP_TIMEOUT_LEN);
	}
	return(1);
  }

  if (len != th->doff*4) flag |= 1;

  /* See if our window has been shrunk. */
  if (after(sk->window_seq, ack+ntohs(th->window))) {
	/*
	 * We may need to move packets from the send queue
	 * to the write queue, if the window has been shrunk on us.
	 * The RFC says you are not allowed to shrink your window
	 * like this, but if the other end does, you must be able
	 * to deal with it.
	 */
	struct sk_buff *skb;
	struct sk_buff *skb2;
	struct sk_buff *wskb = NULL;
  
	skb2 = sk->send_head;
	sk->send_head = NULL;
	sk->send_tail = NULL;

	flag |= 4;

	sk->window_seq = ack + ntohs(th->window);
	cli();
	while (skb2 != NULL) {
		skb = skb2;
		skb2 = (struct sk_buff *)skb->link3;
		skb->link3 = NULL;
		if (after(skb->h.seq, sk->window_seq)) {
			if (sk->packets_out > 0) sk->packets_out--;
			/* We may need to remove this from the dev send list. */
			if (skb->next != NULL) {
				skb_unlink(skb);				
			}
			/* Now add it to the write_queue. */
			skb->magic = TCP_WRITE_QUEUE_MAGIC;
			if (wskb == NULL) {
				skb->next = sk->wfront;
				sk->wfront = skb;
			} else {
				skb->next = wskb->next;
				wskb->next = skb;
			}
			if (sk->wback == wskb) sk->wback = skb;
			wskb = skb;
		} else {
			if (sk->send_head == NULL) {
				sk->send_head = skb;
				sk->send_tail = skb;
			} else {
				sk->send_tail->link3 = skb;
				sk->send_tail = skb;
			}
			skb->link3 = NULL;
		}
	}
	sti();
  }

  if (sk->send_tail == NULL || sk->send_head == NULL) {
	sk->send_head = NULL;
	sk->send_tail = NULL;
	sk->packets_out= 0;
  }

  sk->window_seq = ack + ntohs(th->window);

  /* We don't want too many packets out there. */
  if (sk->timeout == TIME_WRITE && 
      sk->cong_window < 2048 && after(ack, sk->rcv_ack_seq)) {
/* 
 * This is Jacobson's slow start and congestion avoidance. 
 * SIGCOMM '88, p. 328.  Because we keep cong_window in integral
 * mss's, we can't do cwnd += 1 / cwnd.  Instead, maintain a 
 * counter and increment it once every cwnd times.  It's possible
 * that this should be done only if sk->retransmits == 0.  I'm
 * interpreting "new data is acked" as including data that has
 * been retransmitted but is just now being acked.
 */
	if (sk->cong_window < sk->ssthresh)  
	  /* in "safe" area, increase */
	  sk->cong_window++;
	else {
	  /* in dangerous area, increase slowly.  In theory this is
	     sk->cong_window += 1 / sk->cong_window
	   */
	  if (sk->cong_count >= sk->cong_window) {
	    sk->cong_window++;
	    sk->cong_count = 0;
	  } else 
	    sk->cong_count++;
	}
  }

  DPRINTF((DBG_TCP, "tcp_ack: Updating rcv ack sequence.\n"));
  sk->rcv_ack_seq = ack;

  /*
   * if this ack opens up a zero window, clear backoff.  It was
   * being used to time the probes, and is probably far higher than
   * it needs to be for normal retransmission
   */
  if (sk->timeout == TIME_PROBE0) {
  	if (sk->wfront != NULL &&   /* should always be non-null */
	    ! before (sk->window_seq, sk->wfront->h.seq)) {
	  sk->retransmits = 0;
	  sk->backoff = 0;
	  /* recompute rto from rtt.  this eliminates any backoff */
	  sk->rto = ((sk->rtt >> 2) + sk->mdev) >> 1;
	  if (sk->rto > 120*HZ)
	    sk->rto = 120*HZ;
	  if (sk->rto < 1*HZ)
	    sk->rto = 1*HZ;
	}
  }

  /* See if we can take anything off of the retransmit queue. */
  while(sk->send_head != NULL) {
	/* Check for a bug. */
	if (sk->send_head->link3 &&
	    after(sk->send_head->h.seq, sk->send_head->link3->h.seq)) {
		printk("INET: tcp.c: *** bug send_list out of order.\n");
		sort_send(sk);
	}

	if (before(sk->send_head->h.seq, ack+1)) {
		struct sk_buff *oskb;

		if (sk->retransmits) {

		  /* we were retransmitting.  don't count this in RTT est */
		  flag |= 2;

		  /*
		   * even though we've gotten an ack, we're still
		   * retransmitting as long as we're sending from
		   * the retransmit queue.  Keeping retransmits non-zero
		   * prevents us from getting new data interspersed with
		   * retransmissions.
		   */

		  if (sk->send_head->link3)
		    sk->retransmits = 1;
		  else
		    sk->retransmits = 0;

		}

  		/*
		 * Note that we only reset backoff and rto in the
		 * rtt recomputation code.  And that doesn't happen
		 * if there were retransmissions in effect.  So the
		 * first new packet after the retransmissions is
		 * sent with the backoff still in effect.  Not until
		 * we get an ack from a non-retransmitted packet do
		 * we reset the backoff and rto.  This allows us to deal
		 * with a situation where the network delay has increased
		 * suddenly.  I.e. Karn's algorithm. (SIGCOMM '87, p5.)
		 */

		/* We have one less packet out there. */
		if (sk->packets_out > 0) sk->packets_out --;
		DPRINTF((DBG_TCP, "skb=%X skb->h.seq = %d acked ack=%d\n",
				sk->send_head, sk->send_head->h.seq, ack));

		/* Wake up the process, it can probably write more. */
		if (!sk->dead) sk->write_space(sk);

		oskb = sk->send_head;

		if (!(flag&2)) {
		  long m;

		  /* The following amusing code comes from Jacobson's
		   * article in SIGCOMM '88.  Note that rtt and mdev
		   * are scaled versions of rtt and mean deviation.
		   * This is designed to be as fast as possible 
		   * m stands for "measurement".
		   */

		  m = jiffies - oskb->when;  /* RTT */
		  m -= (sk->rtt >> 3);       /* m is now error in rtt est */
		  sk->rtt += m;              /* rtt = 7/8 rtt + 1/8 new */
		  if (m < 0)
		    m = -m;		     /* m is now abs(error) */
		  m -= (sk->mdev >> 2);      /* similar update on mdev */
		  sk->mdev += m;	     /* mdev = 3/4 mdev + 1/4 new */

		  /* now update timeout.  Note that this removes any backoff */
		  sk->rto = ((sk->rtt >> 2) + sk->mdev) >> 1;
		  if (sk->rto > 120*HZ)
		    sk->rto = 120*HZ;
		  if (sk->rto < 1*HZ)
		    sk->rto = 1*HZ;
		  sk->backoff = 0;

		}
		flag |= (2|4);

		cli();

		oskb = sk->send_head;
		IS_SKB(oskb);
		sk->send_head =(struct sk_buff *)oskb->link3;
		if (sk->send_head == NULL) {
			sk->send_tail = NULL;
		}

		/* We may need to remove this from the dev send list. */		
		skb_unlink(oskb);	/* Much easier! */
		sti();
		oskb->magic = 0;
		kfree_skb(oskb, FREE_WRITE); /* write. */
		if (!sk->dead) sk->write_space(sk);
	} else {
		break;
	}
  }

  /*
   * Maybe we can take some stuff off of the write queue,
   * and put it onto the xmit queue.
   */
  if (sk->wfront != NULL) {
	if (after (sk->window_seq+1, sk->wfront->h.seq) &&
	        (sk->retransmits == 0 || 
		 sk->timeout != TIME_WRITE ||
		 before(sk->wfront->h.seq, sk->rcv_ack_seq +1))
		&& sk->packets_out < sk->cong_window) {
		flag |= 1;
		tcp_write_xmit(sk);
 	} else if (before(sk->window_seq, sk->wfront->h.seq) &&
 		   sk->send_head == NULL &&
 		   sk->ack_backlog == 0 &&
 		   sk->state != TCP_TIME_WAIT) {
 	        reset_timer(sk, TIME_PROBE0, sk->rto);
 	}		
  } else {
	if (sk->send_head == NULL && sk->ack_backlog == 0 &&
	    sk->state != TCP_TIME_WAIT && !sk->keepopen) {
		DPRINTF((DBG_TCP, "Nothing to do, going to sleep.\n")); 
		if (!sk->dead) sk->write_space(sk);

		if (sk->keepopen)
			reset_timer(sk, TIME_KEEPOPEN, TCP_TIMEOUT_LEN);
		else
			delete_timer(sk);
	} else {
		if (sk->state != (unsigned char) sk->keepopen) {
			reset_timer(sk, TIME_WRITE, sk->rto);
		}
		if (sk->state == TCP_TIME_WAIT) {
			reset_timer(sk, TIME_CLOSE, TCP_TIMEWAIT_LEN);
		}
	}
  }

  if (sk->packets_out == 0 && sk->partial != NULL &&
      sk->wfront == NULL && sk->send_head == NULL) {
	flag |= 1;
	tcp_send_partial(sk);
  }

  /* See if we are done. */
  if (sk->state == TCP_TIME_WAIT) {
	if (!sk->dead)
		sk->state_change(sk);
	if (sk->rcv_ack_seq == sk->write_seq && sk->acked_seq == sk->fin_seq) {
		flag |= 1;
		sk->state = TCP_CLOSE;
		sk->shutdown = SHUTDOWN_MASK;
	}
  }

  if (sk->state == TCP_LAST_ACK || sk->state == TCP_FIN_WAIT2) {
	if (!sk->dead) sk->state_change(sk);
	if (sk->rcv_ack_seq == sk->write_seq) {
		flag |= 1;
		if (sk->acked_seq != sk->fin_seq) {
			tcp_time_wait(sk);
		} else {
			DPRINTF((DBG_TCP, "tcp_ack closing socket - %X\n", sk));
			tcp_send_ack(sk->sent_seq, sk->acked_seq, sk,
				     th, sk->daddr);
			sk->shutdown = SHUTDOWN_MASK;
			sk->state = TCP_CLOSE;
		}
	}
  }

/*
 * I make no guarantees about the first clause in the following
 * test, i.e. "(!flag) || (flag&4)".  I'm not entirely sure under
 * what conditions "!flag" would be true.  However I think the rest
 * of the conditions would prevent that from causing any
 * unnecessary retransmission. 
 *   Clearly if the first packet has expired it should be 
 * retransmitted.  The other alternative, "flag&2 && retransmits", is
 * harder to explain:  You have to look carefully at how and when the
 * timer is set and with what timeout.  The most recent transmission always
 * sets the timer.  So in general if the most recent thing has timed
 * out, everything before it has as well.  So we want to go ahead and
 * retransmit some more.  If we didn't explicitly test for this
 * condition with "flag&2 && retransmits", chances are "when + rto < jiffies"
 * would not be true.  If you look at the pattern of timing, you can
 * show that rto is increased fast enough that the next packet would
 * almost never be retransmitted immediately.  Then you'd end up
 * waiting for a timeout to send each packet on the retranmission
 * queue.  With my implementation of the Karn sampling algorithm,
 * the timeout would double each time.  The net result is that it would
 * take a hideous amount of time to recover from a single dropped packet.
 * It's possible that there should also be a test for TIME_WRITE, but
 * I think as long as "send_head != NULL" and "retransmit" is on, we've
 * got to be in real retransmission mode.
 *   Note that ip_do_retransmit is called with all==1.  Setting cong_window
 * back to 1 at the timeout will cause us to send 1, then 2, etc. packets.
 * As long as no further losses occur, this seems reasonable.
 */

  if (((!flag) || (flag&4)) && sk->send_head != NULL &&
      (((flag&2) && sk->retransmits) ||
       (sk->send_head->when + sk->rto < jiffies))) {
	ip_do_retransmit(sk, 1);
	reset_timer(sk, TIME_WRITE, sk->rto);
      }

  DPRINTF((DBG_TCP, "leaving tcp_ack\n"));
  return(1);
}


/*
 * This routine handles the data.  If there is room in the buffer,
 * it will be have already been moved into it.  If there is no
 * room, then we will just have to discard the packet.
 */
static int
tcp_data(struct sk_buff *skb, struct sock *sk, 
	 unsigned long saddr, unsigned short len)
{
  struct sk_buff *skb1, *skb2;
  struct tcphdr *th;
  int dup_dumped=0;

  th = skb->h.th;
  print_th(th);
  skb->len = len -(th->doff*4);

  DPRINTF((DBG_TCP, "tcp_data len = %d sk = %X:\n", skb->len, sk));

  sk->bytes_rcv += skb->len;
  if (skb->len == 0 && !th->fin && !th->urg && !th->psh) {
	/* Don't want to keep passing ack's back and forth. */
	if (!th->ack) tcp_send_ack(sk->sent_seq, sk->acked_seq,sk, th, saddr);
	kfree_skb(skb, FREE_READ);
	return(0);
  }

  if (sk->shutdown & RCV_SHUTDOWN) {
	sk->acked_seq = th->seq + skb->len + th->syn + th->fin;
	tcp_reset(sk->saddr, sk->daddr, skb->h.th,
	sk->prot, NULL, skb->dev, sk->ip_tos, sk->ip_ttl);
	sk->state = TCP_CLOSE;
	sk->err = EPIPE;
	sk->shutdown = SHUTDOWN_MASK;
	DPRINTF((DBG_TCP, "tcp_data: closing socket - %X\n", sk));
	kfree_skb(skb, FREE_READ);
	if (!sk->dead) sk->state_change(sk);
	return(0);
  }

  /*
   * Now we have to walk the chain, and figure out where this one
   * goes into it.  This is set up so that the last packet we received
   * will be the first one we look at, that way if everything comes
   * in order, there will be no performance loss, and if they come
   * out of order we will be able to fit things in nicely.
   */

  /* This should start at the last one, and then go around forwards. */
  if (sk->rqueue == NULL) {
	DPRINTF((DBG_TCP, "tcp_data: skb = %X:\n", skb));
#ifdef OLDWAY
	sk->rqueue = skb;
	skb->next = skb;
	skb->prev = skb;
	skb->list = &sk->rqueue;
#else
	skb_queue_head(&sk->rqueue,skb);
#endif		
	skb1= NULL;
  } else {
	DPRINTF((DBG_TCP, "tcp_data adding to chain sk = %X:\n", sk));
	for(skb1=sk->rqueue->prev; ; skb1 =(struct sk_buff *)skb1->prev) {
		if(sk->debug)
		{
			printk("skb1=%p :", skb1);
			printk("skb1->h.th->seq = %ld: ", skb1->h.th->seq);
			printk("skb->h.th->seq = %ld\n",skb->h.th->seq);
			printk("copied_seq = %ld acked_seq = %ld\n", sk->copied_seq,
					sk->acked_seq);
		}
#ifdef OLD		
		if (after(th->seq+1, skb1->h.th->seq)) {
			skb->prev = skb1;
			skb->next = skb1->next;
			skb->next->prev = skb;
			skb1->next = skb;
			if (skb1 == sk->rqueue) sk->rqueue = skb;
			break;
		}
		if (skb1->prev == sk->rqueue) {
			skb->next= skb1;
			skb->prev = skb1->prev;
			skb->prev->next = skb;
			skb1->prev = skb;
			skb1 = NULL; /* so we know we might be able
					to ack stuff. */
			break;
		}
#else
		if (th->seq==skb1->h.th->seq && skb->len>= skb1->len)
		{
			skb_append(skb1,skb);
			skb_unlink(skb1);
			kfree_skb(skb1,FREE_READ);
			dup_dumped=1;
			skb1=NULL;
			break;
		}
		if (after(th->seq+1, skb1->h.th->seq))
		{
			skb_append(skb1,skb);
			break;
		}
		if (skb1 == sk->rqueue)
		{
			skb_queue_head(&sk->rqueue, skb);		
			break;
		}
#endif		
	}
	DPRINTF((DBG_TCP, "skb = %X:\n", skb));
  }

  th->ack_seq = th->seq + skb->len;
  if (th->syn) th->ack_seq++;
  if (th->fin) th->ack_seq++;

  if (before(sk->acked_seq, sk->copied_seq)) {
	printk("*** tcp.c:tcp_data bug acked < copied\n");
	sk->acked_seq = sk->copied_seq;
  }

  /* Now figure out if we can ack anything. */
  if ((!dup_dumped && (skb1 == NULL || skb1->acked)) || before(th->seq, sk->acked_seq+1)) {
      if (before(th->seq, sk->acked_seq+1)) {
		int newwindow;

		if (after(th->ack_seq, sk->acked_seq)) {
			newwindow = sk->window -
				       (th->ack_seq - sk->acked_seq);
			if (newwindow < 0)
				newwindow = 0;	
			sk->window = newwindow;
			sk->acked_seq = th->ack_seq;
		}
		skb->acked = 1;

		/* When we ack the fin, we turn on the RCV_SHUTDOWN flag. */
		if (skb->h.th->fin) {
			if (!sk->dead) sk->state_change(sk);
			sk->shutdown |= RCV_SHUTDOWN;
		}
	  
		for(skb2 = (struct sk_buff *)skb->next;
		    skb2 !=(struct sk_buff *) sk->rqueue;
		    skb2 = (struct sk_buff *)skb2->next) {
			if (before(skb2->h.th->seq, sk->acked_seq+1)) {
				if (after(skb2->h.th->ack_seq, sk->acked_seq))
				{
					newwindow = sk->window -
					 (skb2->h.th->ack_seq - sk->acked_seq);
					if (newwindow < 0)
						newwindow = 0;	
					sk->window = newwindow;
					sk->acked_seq = skb2->h.th->ack_seq;
				}
				skb2->acked = 1;

				/*
				 * When we ack the fin, we turn on
				 * the RCV_SHUTDOWN flag.
				 */
				if (skb2->h.th->fin) {
					sk->shutdown |= RCV_SHUTDOWN;
					if (!sk->dead) sk->state_change(sk);
				}

				/* Force an immediate ack. */
				sk->ack_backlog = sk->max_ack_backlog;
			} else {
				break;
			}
		}

		/*
		 * This also takes care of updating the window.
		 * This if statement needs to be simplified.
		 */
		if (!sk->delay_acks ||
		    sk->ack_backlog >= sk->max_ack_backlog || 
		    sk->bytes_rcv > sk->max_unacked || th->fin) {
/*			tcp_send_ack(sk->sent_seq, sk->acked_seq,sk,th, saddr); */
		} else {
			sk->ack_backlog++;
			if(sk->debug)
				printk("Ack queued.\n");
			reset_timer(sk, TIME_WRITE, TCP_ACK_TIME);
		}
	}
  }

  /*
   * If we've missed a packet, send an ack.
   * Also start a timer to send another.
   */
  if (!skb->acked) {
	/*
	 * This is important.  If we don't have much room left,
	 * we need to throw out a few packets so we have a good
	 * window.  Note that mtu is used, not mss, because mss is really
	 * for the send side.  He could be sending us stuff as large as mtu.
	 */
	while (sk->prot->rspace(sk) < sk->mtu) {
		skb1 = skb_peek(&sk->rqueue);
		if (skb1 == NULL) {
			printk("INET: tcp.c:tcp_data memory leak detected.\n");
			break;
		}

		/* Don't throw out something that has been acked. */
		if (skb1->acked) {
			break;
		}
		
		skb_unlink(skb1);
#ifdef OLDWAY		
		if (skb1->prev == skb1) {
			sk->rqueue = NULL;
		} else {
			sk->rqueue = (struct sk_buff *)skb1->prev;
			skb1->next->prev = skb1->prev;
			skb1->prev->next = skb1->next;
		}
#endif		
		kfree_skb(skb1, FREE_READ);
	}
	tcp_send_ack(sk->sent_seq, sk->acked_seq, sk, th, saddr);
	sk->ack_backlog++;
	reset_timer(sk, TIME_WRITE, TCP_ACK_TIME);
  } else {
	/* We missed a packet.  Send an ack to try to resync things. */
	tcp_send_ack(sk->sent_seq, sk->acked_seq, sk, th, saddr);
  }

  /* Now tell the user we may have some data. */
  if (!sk->dead) {
        if(sk->debug)
        	printk("Data wakeup.\n");
	sk->data_ready(sk,0);
  } else {
	DPRINTF((DBG_TCP, "data received on dead socket.\n"));
  }

  if (sk->state == TCP_FIN_WAIT2 &&
      sk->acked_seq == sk->fin_seq && sk->rcv_ack_seq == sk->write_seq) {
	DPRINTF((DBG_TCP, "tcp_data: entering last_ack state sk = %X\n", sk));

/*	tcp_send_ack(sk->sent_seq, sk->acked_seq, sk, th, saddr); */
	sk->shutdown = SHUTDOWN_MASK;
	sk->state = TCP_LAST_ACK;
	if (!sk->dead) sk->state_change(sk);
  }

  return(0);
}


static void tcp_check_urg(struct sock * sk, struct tcphdr * th)
{
	unsigned long ptr = ntohs(th->urg_ptr);

	if (ptr)
		ptr--;
	ptr += th->seq;

	/* ignore urgent data that we've already seen and read */
	if (after(sk->copied_seq+1, ptr))
		return;

	/* do we already have a newer (or duplicate) urgent pointer? */
	if (sk->urg_data && !after(ptr, sk->urg_seq))
		return;

	/* tell the world about our new urgent pointer */
	if (sk->proc != 0) {
		if (sk->proc > 0) {
			kill_proc(sk->proc, SIGURG, 1);
		} else {
			kill_pg(-sk->proc, SIGURG, 1);
		}
	}
	sk->urg_data = URG_NOTYET;
	sk->urg_seq = ptr;
}

static inline int tcp_urg(struct sock *sk, struct tcphdr *th,
	unsigned long saddr, unsigned long len)
{
	unsigned long ptr;

	/* check if we get a new urgent pointer */
	if (th->urg)
		tcp_check_urg(sk,th);

	/* do we wait for any urgent data? */
	if (sk->urg_data != URG_NOTYET)
		return 0;

	/* is the urgent pointer pointing into this packet? */
	ptr = sk->urg_seq - th->seq + th->doff*4;
	if (ptr >= len)
		return 0;

	/* ok, got the correct packet, update info */
	sk->urg_data = URG_VALID | *(ptr + (unsigned char *) th);
	if (!sk->dead)
		wake_up_interruptible(sk->sleep);
	return 0;
}


/* This deals with incoming fins. 'Linus at 9 O'clock' 8-) */
static int
tcp_fin(struct sock *sk, struct tcphdr *th, 
	 unsigned long saddr, struct device *dev)
{
  DPRINTF((DBG_TCP, "tcp_fin(sk=%X, th=%X, saddr=%X, dev=%X)\n",
						sk, th, saddr, dev));
  
  if (!sk->dead) {
	sk->state_change(sk);
  }

  switch(sk->state) {
	case TCP_SYN_RECV:
	case TCP_SYN_SENT:
	case TCP_ESTABLISHED:
		/* Contains the one that needs to be acked */
		sk->fin_seq = th->seq+1;
		sk->state = TCP_CLOSE_WAIT;
		if (th->rst) sk->shutdown = SHUTDOWN_MASK;
		break;

	case TCP_CLOSE_WAIT:
	case TCP_FIN_WAIT2:
		break; /* we got a retransmit of the fin. */

	case TCP_FIN_WAIT1:
		/* Contains the one that needs to be acked */
		sk->fin_seq = th->seq+1;
		sk->state = TCP_FIN_WAIT2;
		break;

	default:
	case TCP_TIME_WAIT:
		sk->state = TCP_LAST_ACK;

		/* Start the timers. */
		reset_timer(sk, TIME_CLOSE, TCP_TIMEWAIT_LEN);
		return(0);
  }
  sk->ack_backlog++;

  return(0);
}


/* This will accept the next outstanding connection. */
static struct sock *
tcp_accept(struct sock *sk, int flags)
{
  struct sock *newsk;
  struct sk_buff *skb;
  
  DPRINTF((DBG_TCP, "tcp_accept(sk=%X, flags=%X, addr=%s)\n",
				sk, flags, in_ntoa(sk->saddr)));

  /*
   * We need to make sure that this socket is listening,
   * and that it has something pending.
   */
  if (sk->state != TCP_LISTEN) {
	sk->err = EINVAL;
	return(NULL); 
  }

  /* avoid the race. */
  cli();
  sk->inuse = 1;
  while((skb = get_firstr(sk)) == NULL) {
	if (flags & O_NONBLOCK) {
		sti();
		release_sock(sk);
		sk->err = EAGAIN;
		return(NULL);
	}

	release_sock(sk);
	interruptible_sleep_on(sk->sleep);
	if (current->signal & ~current->blocked) {
		sti();
		sk->err = ERESTARTSYS;
		return(NULL);
	}
	sk->inuse = 1;
  }
  sti();

  /* Now all we need to do is return skb->sk. */
  newsk = skb->sk;

  kfree_skb(skb, FREE_READ);
  sk->ack_backlog--;
  release_sock(sk);
  return(newsk);
}


/* This will initiate an outgoing connection. */
static int
tcp_connect(struct sock *sk, struct sockaddr_in *usin, int addr_len)
{
  struct sk_buff *buff;
  struct sockaddr_in sin;
  struct device *dev=NULL;
  unsigned char *ptr;
  int tmp;
  struct tcphdr *t1;
  int err;

  if (sk->state != TCP_CLOSE) return(-EISCONN);
  if (addr_len < 8) return(-EINVAL);

  err=verify_area(VERIFY_READ, usin, addr_len);
  if(err)
  	return err;
  	
  memcpy_fromfs(&sin,usin, min(sizeof(sin), addr_len));

  if (sin.sin_family && sin.sin_family != AF_INET) return(-EAFNOSUPPORT);

  DPRINTF((DBG_TCP, "TCP connect daddr=%s\n", in_ntoa(sin.sin_addr.s_addr)));
  
  /* Don't want a TCP connection going to a broadcast address */
  if (chk_addr(sin.sin_addr.s_addr) == IS_BROADCAST) { 
	DPRINTF((DBG_TCP, "TCP connection to broadcast address not allowed\n"));
	return(-ENETUNREACH);
  }
  
  /* Connect back to the same socket: Blows up so disallow it */
  if(sk->saddr == sin.sin_addr.s_addr && sk->num==ntohs(sin.sin_port))
	return -EBUSY;

  sk->inuse = 1;
  sk->daddr = sin.sin_addr.s_addr;
  sk->write_seq = jiffies * SEQ_TICK - seq_offset;
  sk->window_seq = sk->write_seq;
  sk->rcv_ack_seq = sk->write_seq -1;
  sk->err = 0;
  sk->dummy_th.dest = sin.sin_port;
  release_sock(sk);

  buff = sk->prot->wmalloc(sk,MAX_SYN_SIZE,0, GFP_KERNEL);
  if (buff == NULL) {
	return(-ENOMEM);
  }
  sk->inuse = 1;
  buff->mem_addr = buff;
  buff->mem_len = MAX_SYN_SIZE;
  buff->len = 24;
  buff->sk = sk;
  buff->free = 1;
  t1 = (struct tcphdr *) buff->data;

  /* Put in the IP header and routing stuff. */
  /* We need to build the routing stuff fromt the things saved in skb. */
  tmp = sk->prot->build_header(buff, sk->saddr, sk->daddr, &dev,
					IPPROTO_TCP, NULL, MAX_SYN_SIZE,sk->ip_tos,sk->ip_ttl);
  if (tmp < 0) {
	sk->prot->wfree(sk, buff->mem_addr, buff->mem_len);
	release_sock(sk);
	return(-ENETUNREACH);
  }
  buff->len += tmp;
  t1 = (struct tcphdr *)((char *)t1 +tmp);

  memcpy(t1,(void *)&(sk->dummy_th), sizeof(*t1));
  t1->seq = ntohl(sk->write_seq++);
  sk->sent_seq = sk->write_seq;
  buff->h.seq = sk->write_seq;
  t1->ack = 0;
  t1->window = 2;
  t1->res1=0;
  t1->res2=0;
  t1->rst = 0;
  t1->urg = 0;
  t1->psh = 0;
  t1->syn = 1;
  t1->urg_ptr = 0;
  t1->doff = 6;

/* use 512 or whatever user asked for */
  if (sk->user_mss)
    sk->mtu = sk->user_mss;
  else {
#ifdef SUBNETSARELOCAL
    if ((sk->saddr ^ sk->daddr) & default_mask(sk->saddr))
#else
    if ((sk->saddr ^ sk->daddr) & dev->pa_mask)
#endif
      sk->mtu = 576 - HEADER_SIZE;
    else
      sk->mtu = MAX_WINDOW;
  }
/* but not bigger than device MTU */
  sk->mtu = min(sk->mtu, dev->mtu - HEADER_SIZE);

  /* Put in the TCP options to say MTU. */
  ptr = (unsigned char *)(t1+1);
  ptr[0] = 2;
  ptr[1] = 4;
  ptr[2] = (sk->mtu) >> 8;
  ptr[3] = (sk->mtu) & 0xff;
  tcp_send_check(t1, sk->saddr, sk->daddr,
		  sizeof(struct tcphdr) + 4, sk);

  /* This must go first otherwise a really quick response will get reset. */
  sk->state = TCP_SYN_SENT;
  sk->rtt = TCP_CONNECT_TIME;
  reset_timer(sk, TIME_WRITE, TCP_CONNECT_TIME);	/* Timer for repeating the SYN until an answer */
  sk->retransmits = TCP_RETR2 - TCP_SYN_RETRIES;

  sk->prot->queue_xmit(sk, dev, buff, 0);  
  
  release_sock(sk);
  return(0);
}


/* This functions checks to see if the tcp header is actually acceptable. */
static int
tcp_sequence(struct sock *sk, struct tcphdr *th, short len,
	     struct options *opt, unsigned long saddr, struct device *dev)
{
	unsigned long next_seq;

	next_seq = len - 4*th->doff;
	if (th->fin)
		next_seq++;
	/* if we have a zero window, we can't have any data in the packet.. */
	if (next_seq && !sk->window)
		goto ignore_it;
	next_seq += th->seq;

	/*
	 * This isn't quite right.  sk->acked_seq could be more recent
	 * than sk->window.  This is however close enough.  We will accept
	 * slightly more packets than we should, but it should not cause
	 * problems unless someone is trying to forge packets.
	 */

	/* have we already seen all of this packet? */
	if (!after(next_seq+1, sk->acked_seq))
		goto ignore_it;
	/* or does it start beyond the window? */
	if (!before(th->seq, sk->acked_seq + sk->window + 1))
		goto ignore_it;

	/* ok, at least part of this packet would seem interesting.. */
	return 1;

ignore_it:
	DPRINTF((DBG_TCP, "tcp_sequence: rejecting packet.\n"));

	/*
	 *	Send a reset if we get something not ours and we are
	 *	unsynchronized. Note: We don't do anything to our end. We
	 *	are just killing the bogus remote connection then we will
	 *	connect again and it will work (with luck).
	 */
  	 
	if (sk->state==TCP_SYN_SENT || sk->state==TCP_SYN_RECV) {
		tcp_reset(sk->saddr,sk->daddr,th,sk->prot,NULL,dev, sk->ip_tos,sk->ip_ttl);
		return 1;
	}

	if (th->rst)
		return 0;

	/* Try to resync things. */
	tcp_send_ack(sk->sent_seq, sk->acked_seq, sk, th, saddr);
	return 0;
}


int
tcp_rcv(struct sk_buff *skb, struct device *dev, struct options *opt,
	unsigned long daddr, unsigned short len,
	unsigned long saddr, int redo, struct inet_protocol * protocol)
{
  struct tcphdr *th;
  struct sock *sk;

  if (!skb) {
	DPRINTF((DBG_TCP, "tcp.c: tcp_rcv skb = NULL\n"));
	return(0);
  }
#if 0	/* FIXME: it's ok for protocol to be NULL */
  if (!protocol) {
	DPRINTF((DBG_TCP, "tcp.c: tcp_rcv protocol = NULL\n"));
	return(0);
  }

  if (!opt) {	/* FIXME: it's ok for opt to be NULL */
	DPRINTF((DBG_TCP, "tcp.c: tcp_rcv opt = NULL\n"));
  }
#endif
  if (!dev) {
	DPRINTF((DBG_TCP, "tcp.c: tcp_rcv dev = NULL\n"));
	return(0);
  }
  th = skb->h.th;

  /* Find the socket. */
  sk = get_sock(&tcp_prot, th->dest, saddr, th->source, daddr);
  DPRINTF((DBG_TCP, "<<\n"));
  DPRINTF((DBG_TCP, "len = %d, redo = %d, skb=%X\n", len, redo, skb));
  
  /* If this socket has got a reset its to all intents and purposes 
     really dead */
  if (sk!=NULL && sk->zapped)
	sk=NULL;

  if (sk) {
	 DPRINTF((DBG_TCP, "sk = %X:\n", sk));
  }

  if (!redo) {
	if (tcp_check(th, len, saddr, daddr )) {
		skb->sk = NULL;
		DPRINTF((DBG_TCP, "packet dropped with bad checksum.\n"));
if (inet_debug == DBG_SLIP) printk("\rtcp_rcv: bad checksum\n");
		kfree_skb(skb,FREE_READ);
		/*
		 * We don't release the socket because it was
		 * never marked in use.
		 */
		return(0);
	}

	th->seq = ntohl(th->seq);

	/* See if we know about the socket. */
	if (sk == NULL) {
		if (!th->rst)
			tcp_reset(daddr, saddr, th, &tcp_prot, opt,dev,skb->ip_hdr->tos,255);
		skb->sk = NULL;
		kfree_skb(skb, FREE_READ);
		return(0);
	}

	skb->len = len;
	skb->sk = sk;
	skb->acked = 0;
	skb->used = 0;
	skb->free = 0;
	skb->saddr = daddr;
	skb->daddr = saddr;

	/* We may need to add it to the backlog here. */
	cli();
	if (sk->inuse) {
		if (sk->back_log == NULL) {
			sk->back_log = skb;
			skb->next = skb;
			skb->prev = skb;
		} else {
			skb->next = sk->back_log;
			skb->prev = sk->back_log->prev;
			skb->prev->next = skb;
			skb->next->prev = skb;
		}
		sti();
		return(0);
	}
	sk->inuse = 1;
	sti();
  } else {
	if (!sk) {
		DPRINTF((DBG_TCP, "tcp.c: tcp_rcv bug sk=NULL redo = 1\n"));
		return(0);
	}
  }

  if (!sk->prot) {
	DPRINTF((DBG_TCP, "tcp.c: tcp_rcv sk->prot = NULL \n"));
	return(0);
  }

  /* Charge the memory to the socket. */
  if (sk->rmem_alloc + skb->mem_len >= sk->rcvbuf) {
	skb->sk = NULL;
	DPRINTF((DBG_TCP, "dropping packet due to lack of buffer space.\n"));
	kfree_skb(skb, FREE_READ);
	release_sock(sk);
	return(0);
  }
  sk->rmem_alloc += skb->mem_len;

  DPRINTF((DBG_TCP, "About to do switch.\n"));

  /* Now deal with it. */
  switch(sk->state) {
	/*
	 * This should close the system down if it's waiting
	 * for an ack that is never going to be sent.
	 */
	case TCP_LAST_ACK:
		if (th->rst) {
			sk->zapped=1;
			sk->err = ECONNRESET;
 			sk->state = TCP_CLOSE;
			sk->shutdown = SHUTDOWN_MASK;
			if (!sk->dead) {
				sk->state_change(sk);
			}
			kfree_skb(skb, FREE_READ);
			release_sock(sk);
			return(0);
		}

	case TCP_ESTABLISHED:
	case TCP_CLOSE_WAIT:
	case TCP_FIN_WAIT1:
	case TCP_FIN_WAIT2:
	case TCP_TIME_WAIT:
		if (!tcp_sequence(sk, th, len, opt, saddr,dev)) {
if (inet_debug == DBG_SLIP) printk("\rtcp_rcv: not in seq\n");
#ifdef undef
/* nice idea, but tcp_sequence already does this.  Maybe it shouldn't?? */
			if(!th->rst)
				tcp_send_ack(sk->sent_seq, sk->acked_seq, 
				     sk, th, saddr);
#endif
			kfree_skb(skb, FREE_READ);
			release_sock(sk);
			return(0);
		}

		if (th->rst) {
			sk->zapped=1;
			/* This means the thing should really be closed. */
			sk->err = ECONNRESET;

			if (sk->state == TCP_CLOSE_WAIT) {
				sk->err = EPIPE;
			}

			/*
			 * A reset with a fin just means that
			 * the data was not all read.
			 */
			sk->state = TCP_CLOSE;
			sk->shutdown = SHUTDOWN_MASK;
			if (!sk->dead) {
				sk->state_change(sk);
			}
			kfree_skb(skb, FREE_READ);
			release_sock(sk);
			return(0);
		}
		if (
#if 0
		if ((opt && (opt->security != 0 ||
			    opt->compartment != 0)) || 
#endif
				 th->syn) {
			sk->err = ECONNRESET;
			sk->state = TCP_CLOSE;
			sk->shutdown = SHUTDOWN_MASK;
			tcp_reset(daddr, saddr,  th, sk->prot, opt,dev, sk->ip_tos,sk->ip_ttl);
			if (!sk->dead) {
				sk->state_change(sk);
			}
			kfree_skb(skb, FREE_READ);
			release_sock(sk);
			return(0);
		}

		if (th->ack && !tcp_ack(sk, th, saddr, len)) {
			kfree_skb(skb, FREE_READ);
			release_sock(sk);
			return(0);
		}

		if (tcp_urg(sk, th, saddr, len)) {
			kfree_skb(skb, FREE_READ);
			release_sock(sk);
			return(0);
		}

		if (tcp_data(skb, sk, saddr, len)) {
			kfree_skb(skb, FREE_READ);
			release_sock(sk);
			return(0);
		}

		/* Moved: you must do data then fin bit */
		if (th->fin && tcp_fin(sk, th, saddr, dev)) {
			kfree_skb(skb, FREE_READ);
			release_sock(sk);
			return(0);
		}

		release_sock(sk);
		return(0);

	case TCP_CLOSE:
		if (sk->dead || sk->daddr) {
			DPRINTF((DBG_TCP, "packet received for closed,dead socket\n"));
			kfree_skb(skb, FREE_READ);
			release_sock(sk);
			return(0);
		}

		if (!th->rst) {
			if (!th->ack)
				th->ack_seq = 0;
			tcp_reset(daddr, saddr, th, sk->prot, opt,dev,sk->ip_tos,sk->ip_ttl);
		}
		kfree_skb(skb, FREE_READ);
		release_sock(sk);
		return(0);

	case TCP_LISTEN:
		if (th->rst) {
			kfree_skb(skb, FREE_READ);
			release_sock(sk);
			return(0);
		}
		if (th->ack) {
			tcp_reset(daddr, saddr, th, sk->prot, opt,dev,sk->ip_tos,sk->ip_ttl);
			kfree_skb(skb, FREE_READ);
			release_sock(sk);
			return(0);
		}

		if (th->syn) {
#if 0
			if (opt->security != 0 || opt->compartment != 0) {
				tcp_reset(daddr, saddr, th, prot, opt,dev);
				release_sock(sk);
				return(0);
			}
#endif

			/*
			 * Now we just put the whole thing including
			 * the header and saddr, and protocol pointer
			 * into the buffer.  We can't respond until the
			 * user tells us to accept the connection.
			 */
			tcp_conn_request(sk, skb, daddr, saddr, opt, dev);
			release_sock(sk);
			return(0);
		}

		kfree_skb(skb, FREE_READ);
		release_sock(sk);
		return(0);

	default:
		if (!tcp_sequence(sk, th, len, opt, saddr,dev)) {
			kfree_skb(skb, FREE_READ);
			release_sock(sk);
			return(0);
		}

	case TCP_SYN_SENT:
		if (th->rst) {
			sk->err = ECONNREFUSED;
			sk->state = TCP_CLOSE;
			sk->shutdown = SHUTDOWN_MASK;
			sk->zapped = 1;
			if (!sk->dead) {
				sk->state_change(sk);
			}
			kfree_skb(skb, FREE_READ);
			release_sock(sk);
			return(0);
		}
#if 0
		if (opt->security != 0 || opt->compartment != 0) {
			sk->err = ECONNRESET;
			sk->state = TCP_CLOSE;
			sk->shutdown = SHUTDOWN_MASK;
			tcp_reset(daddr, saddr,  th, sk->prot, opt, dev);
			if (!sk->dead) {
				wake_up_interruptible(sk->sleep);
			}
			kfree_skb(skb, FREE_READ);
			release_sock(sk);
			return(0);
		}
#endif
		if (!th->ack) {
			if (th->syn) {
				sk->state = TCP_SYN_RECV;
			}

			kfree_skb(skb, FREE_READ);
			release_sock(sk);
			return(0);
		}

		switch(sk->state) {
			case TCP_SYN_SENT:
				if (!tcp_ack(sk, th, saddr, len)) {
					tcp_reset(daddr, saddr, th,
							sk->prot, opt,dev,sk->ip_tos,sk->ip_ttl);
					kfree_skb(skb, FREE_READ);
					release_sock(sk);
					return(0);
				}

				/*
				 * If the syn bit is also set, switch to
				 * tcp_syn_recv, and then to established.
				 */
				if (!th->syn) {
					kfree_skb(skb, FREE_READ);
					release_sock(sk);
					return(0);
				}

				/* Ack the syn and fall through. */
				sk->acked_seq = th->seq+1;
				sk->fin_seq = th->seq;
				tcp_send_ack(sk->sent_seq, th->seq+1,
							sk, th, sk->daddr);
	
			case TCP_SYN_RECV:
				if (!tcp_ack(sk, th, saddr, len)) {
					tcp_reset(daddr, saddr, th,
							sk->prot, opt, dev,sk->ip_tos,sk->ip_ttl);
					kfree_skb(skb, FREE_READ);
					release_sock(sk);
					return(0);
				}
				sk->state = TCP_ESTABLISHED;

				/*
				 * Now we need to finish filling out
				 * some of the tcp header.
				 */
				/* We need to check for mtu info. */
				tcp_options(sk, th);
				sk->dummy_th.dest = th->source;
				sk->copied_seq = sk->acked_seq-1;
				if (!sk->dead) {
					sk->state_change(sk);
				}

				/*
				 * We've already processed his first
				 * ack.  In just about all cases that
				 * will have set max_window.  This is
				 * to protect us against the possibility
				 * that the initial window he sent was 0.
				 * This must occur after tcp_options, which
				 * sets sk->mtu.
				 */
				if (sk->max_window == 0) {
				  sk->max_window = 32;
				  sk->mss = min(sk->max_window, sk->mtu);
				}

				/*
				 * Now process the rest like we were
				 * already in the established state.
				 */
				if (th->urg) {
					if (tcp_urg(sk, th, saddr, len)) { 
						kfree_skb(skb, FREE_READ);
						release_sock(sk);
						return(0);
					}
			}
			if (tcp_data(skb, sk, saddr, len))
						kfree_skb(skb, FREE_READ);

			if (th->fin) tcp_fin(sk, th, saddr, dev);
			release_sock(sk);
			return(0);
		}

		if (th->urg) {
			if (tcp_urg(sk, th, saddr, len)) {
				kfree_skb(skb, FREE_READ);
				release_sock(sk);
				return(0);
			}
		}

		if (tcp_data(skb, sk, saddr, len)) {
			kfree_skb(skb, FREE_READ);
			release_sock(sk);
			return(0);
		}

		if (!th->fin) {
			release_sock(sk);
			return(0);
		}
		tcp_fin(sk, th, saddr, dev);
		release_sock(sk);
		return(0);
	}
}


/*
  * This routine sends a packet with an out of date sequence
  * number. It assumes the other end will try to ack it.
  */
static void
tcp_write_wakeup(struct sock *sk)
{
  struct sk_buff *buff;
  struct tcphdr *t1;
  struct device *dev=NULL;
  int tmp;

  if (sk->zapped)
	return;	/* Afer a valid reset we can send no more */

  if (sk -> state != TCP_ESTABLISHED && sk->state != TCP_CLOSE_WAIT &&
      sk -> state != TCP_FIN_WAIT1 && sk->state != TCP_FIN_WAIT2)
	return;

  buff = sk->prot->wmalloc(sk,MAX_ACK_SIZE,1, GFP_ATOMIC);
  if (buff == NULL) return;

  buff->mem_addr = buff;
  buff->mem_len = MAX_ACK_SIZE;
  buff->len = sizeof(struct tcphdr);
  buff->free = 1;
  buff->sk = sk;
  DPRINTF((DBG_TCP, "in tcp_write_wakeup\n"));
  t1 = (struct tcphdr *) buff->data;

  /* Put in the IP header and routing stuff. */
  tmp = sk->prot->build_header(buff, sk->saddr, sk->daddr, &dev,
				IPPROTO_TCP, sk->opt, MAX_ACK_SIZE,sk->ip_tos,sk->ip_ttl);
  if (tmp < 0) {
	sk->prot->wfree(sk, buff->mem_addr, buff->mem_len);
	return;
  }

  buff->len += tmp;
  t1 = (struct tcphdr *)((char *)t1 +tmp);

  memcpy(t1,(void *) &sk->dummy_th, sizeof(*t1));

  /*
   * Use a previous sequence.
   * This should cause the other end to send an ack.
   */
  t1->seq = htonl(sk->sent_seq-1);
  t1->ack = 1; 
  t1->res1= 0;
  t1->res2= 0;
  t1->rst = 0;
  t1->urg = 0;
  t1->psh = 0;
  t1->fin = 0;
  t1->syn = 0;
  t1->ack_seq = ntohl(sk->acked_seq);
  t1->window = ntohs(tcp_select_window(sk)/*sk->prot->rspace(sk)*/);
  t1->doff = sizeof(*t1)/4;
  tcp_send_check(t1, sk->saddr, sk->daddr, sizeof(*t1), sk);

  /* Send it and free it.
   * This will prevent the timer from automatically being restarted.
  */
  sk->prot->queue_xmit(sk, dev, buff, 1);
}

void
tcp_send_probe0(struct sock *sk)
{
	if (sk->zapped)
		return;		/* Afer a valid reset we can send no more */

	tcp_write_wakeup(sk);

	sk->backoff++;
	sk->rto = min(sk->rto << 1, 120*HZ);
	reset_timer (sk, TIME_PROBE0, sk->rto);
	sk->retransmits++;
	sk->prot->retransmits ++;
}


/*
 *	Socket option code for TCP. 
 */  
int tcp_setsockopt(struct sock *sk, int level, int optname, char *optval, int optlen)
{
	int val,err;

	if(level!=SOL_TCP)
		return ip_setsockopt(sk,level,optname,optval,optlen);

  	if (optval == NULL) 
  		return(-EINVAL);

  	err=verify_area(VERIFY_READ, optval, sizeof(int));
  	if(err)
  		return err;
  	
  	val = get_fs_long((unsigned long *)optval);

	switch(optname)
	{
		case TCP_MAXSEG:
/*			if(val<200||val>2048 || val>sk->mtu) */
/*
 * values greater than interface MTU won't take effect.  however at
 * the point when this call is done we typically don't yet know
 * which interface is going to be used
 */
	  		if(val<1||val>MAX_WINDOW)
				return -EINVAL;
			sk->user_mss=val;
			return 0;
		case TCP_NODELAY:
			sk->nonagle=(val==0)?0:1;
			return 0;
		default:
			return(-ENOPROTOOPT);
	}
}

int tcp_getsockopt(struct sock *sk, int level, int optname, char *optval, int *optlen)
{
	int val,err;

	if(level!=SOL_TCP)
		return ip_getsockopt(sk,level,optname,optval,optlen);
			
	switch(optname)
	{
		case TCP_MAXSEG:
			val=sk->user_mss;
			break;
		case TCP_NODELAY:
			val=sk->nonagle;	/* Until Johannes stuff is in */
			break;
		default:
			return(-ENOPROTOOPT);
	}
	err=verify_area(VERIFY_WRITE, optlen, sizeof(int));
	if(err)
  		return err;
  	put_fs_long(sizeof(int),(unsigned long *) optlen);

  	err=verify_area(VERIFY_WRITE, optval, sizeof(int));
  	if(err)
  		return err;
  	put_fs_long(val,(unsigned long *)optval);

  	return(0);
}	


struct proto tcp_prot = {
  sock_wmalloc,
  sock_rmalloc,
  sock_wfree,
  sock_rfree,
  sock_rspace,
  sock_wspace,
  tcp_close,
  tcp_read,
  tcp_write,
  tcp_sendto,
  tcp_recvfrom,
  ip_build_header,
  tcp_connect,
  tcp_accept,
  ip_queue_xmit,
  tcp_retransmit,
  tcp_write_wakeup,
  tcp_read_wakeup,
  tcp_rcv,
  tcp_select,
  tcp_ioctl,
  NULL,
  tcp_shutdown,
  tcp_setsockopt,
  tcp_getsockopt,
  128,
  0,
  {NULL,},
  "TCP"
};
