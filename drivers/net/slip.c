/*
 * slip.c	This module implements the SLIP protocol for kernel-based
 *		devices like TTY.  It interfaces between a raw TTY, and the
 *		kernel's INET protocol layers (via DDI).
 *
 * Version:	@(#)slip.c	0.7.6	05/25/93
 *
 * Authors:	Laurence Culhane, <loz@holmes.demon.co.uk>
 *		Fred N. van Kempen, <waltje@uwalt.nl.mugnet.org>
 *
 * Fixes:
 *		Alan Cox	: 	Sanity checks and avoid tx overruns.
 *					Has a new sl->mtu field.
 *		Alan Cox	: 	Found cause of overrun. ifconfig sl0 mtu upwards.
 *					Driver now spots this and grows/shrinks its buffers(hack!).
 *					Memory leak if you run out of memory setting up a slip driver fixed.
 *		Matt Dillon	:	Printable slip (borrowed from NET2E)
 *	Pauline Middelink	:	Slip driver fixes.
 *		Alan Cox	:	Honours the old SL_COMPRESSED flag
 *		Alan Cox	:	KISS AX.25 and AXUI IP support
 *		Michael Riepe	:	Automatic CSLIP recognition added
 *		Charles Hedrick :	CSLIP header length problem fix.
 *		Alan Cox	:	Corrected non-IP cases of the above.
 */
 
#include <asm/segment.h>
#include <asm/system.h>

#include <linux/config.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/interrupt.h>
#include <linux/tty.h>
#include <linux/errno.h>
#include <linux/stat.h>
#include <linux/in.h>
#include "inet.h"
#include "dev.h"
#ifdef CONFIG_AX25
#include "ax25.h"
#endif
#include "eth.h"
#include "ip.h"
#include "route.h"
#include "protocol.h"
#include "tcp.h"
#include "skbuff.h"
#include "sock.h"
#include "arp.h"
#include "slip.h"
#include "slhc.h"

#define	SLIP_VERSION	"0.7.5"

/* Define some IP layer stuff.  Not all systems have it. */
#ifdef SL_DUMP
#   define	IP_VERSION	4	/* version# of our IP software	*/
#   define	IPF_F_OFFSET	0x1fff	/* Offset field			*/
#   define	IPF_DF		0x4000	/* Don't fragment flag		*/
#   define	IPF_MF		0x2000	/* More Fragments flag		*/
#   define	IP_OF_COPIED	0x80	/* Copied-on-fragmentation flag	*/
#   define	IP_OF_CLASS	0x60	/* Option class			*/
#   define	IP_OF_NUMBER	0x1f	/* Option number		*/
#endif


static struct slip	sl_ctrl[SL_NRUNIT];
static struct tty_ldisc	sl_ldisc;
static int		already = 0;


/* Dump the contents of an IP datagram. */
static void
ip_dump(unsigned char *ptr, int len)
{
#ifdef SL_DUMP
  struct iphdr *ip;
  struct tcphdr *th;
  int dlen, doff;

  if (inet_debug != DBG_SLIP) return;

  ip = (struct iphdr *) ptr;
  th = (struct tcphdr *) (ptr + ip->ihl * 4);
  printk("\r%s -> %s seq %lx ack %lx len %d\n",
	 in_ntoa(ip->saddr), in_ntoa(ip->daddr), 
	 ntohl(th->seq), ntohl(th->ack_seq), ntohs(ip->tot_len));
  return;

  printk("\r*****\n");
  printk("%p %d\n", ptr, len);
  ip = (struct iphdr *) ptr;
  dlen = ntohs(ip->tot_len);
  doff = ((ntohs(ip->frag_off) & IPF_F_OFFSET) << 3);


  printk("SLIP: %s->", in_ntoa(ip->saddr));
  printk("%s\n", in_ntoa(ip->daddr));
  printk(" len %u ihl %u ver %u ttl %u prot %u",
	dlen, ip->ihl, ip->version, ip->ttl, ip->protocol);

  if (ip->tos != 0) printk(" tos %u", ip->tos);
  if (doff != 0 || (ntohs(ip->frag_off) & IPF_MF))
	printk(" id %u offs %u", ntohs(ip->id), doff);

  if (ntohs(ip->frag_off) & IPF_DF) printk(" DF");
  if (ntohs(ip->frag_off) & IPF_MF) printk(" MF");
  printk("\n*****\n");
#endif
}

#if 0
void clh_dump(unsigned char *cp, int len)
{
  if (len > 60)
    len = 60;
  printk("%d:", len);
  while (len > 0) {
    printk(" %x", *cp++);
    len--;
  }
  printk("\n\n");
}
#endif

/* Initialize a SLIP control block for use. */
static void
sl_initialize(struct slip *sl, struct device *dev)
{
  sl->inuse		= 0;
  sl->sending		= 0;
  sl->escape		= 0;
  sl->flags		= 0;
#ifdef SL_ADAPTIVE
  sl->mode		= SL_MODE_ADAPTIVE;	/* automatic CSLIP recognition */
#else
#ifdef SL_COMPRESSED
  sl->mode		= SL_MODE_CSLIP | SL_MODE_ADAPTIVE;	/* Default */
#else
  sl->mode		= SL_MODE_SLIP;		/* Default for non compressors */
#endif
#endif  

  sl->line		= dev->base_addr;
  sl->tty		= NULL;
  sl->dev		= dev;
  sl->slcomp		= NULL;

  /* Clear all pointers. */
  sl->rbuff		= NULL;
  sl->xbuff		= NULL;
  sl->cbuff		= NULL;

  sl->rhead		= NULL;
  sl->rend		= NULL;
  dev->rmem_end		= (unsigned long) NULL;
  dev->rmem_start	= (unsigned long) NULL;
  dev->mem_end		= (unsigned long) NULL;
  dev->mem_start	= (unsigned long) NULL;
}


/* Find a SLIP channel from its `tty' link. */
static struct slip *
sl_find(struct tty_struct *tty)
{
  struct slip *sl;
  int i;

  if (tty == NULL) return(NULL);
  for (i = 0; i < SL_NRUNIT; i++) {
	sl = &sl_ctrl[i];
	if (sl->tty == tty) return(sl);
  }
  return(NULL);
}


/* Find a free SLIP channel, and link in this `tty' line. */
static inline struct slip *
sl_alloc(void)
{
  unsigned long flags;
  struct slip *sl;
  int i;

  save_flags (flags);
  cli();
  for (i = 0; i < SL_NRUNIT; i++) {
	sl = &sl_ctrl[i];
	if (sl->inuse == 0) {
		sl->inuse = 1;
		sl->tty = NULL;
		restore_flags(flags);
		return(sl);
	}
  }
  restore_flags(flags);
  return(NULL);
}


/* Free a SLIP channel. */
static inline void
sl_free(struct slip *sl)
{
  unsigned long flags;

  if (sl->inuse) {
  	save_flags(flags);
  	cli();
	sl->inuse = 0;
	sl->tty = NULL;
	restore_flags(flags);
  }
}

/* MTU has been changed by the IP layer. Unfortunately we are not told about this, but
   we spot it ourselves and fix things up. We could be in an upcall from the tty
   driver, or in an ip packet queue. */
   
static void sl_changedmtu(struct slip *sl)
{
	struct device *dev=sl->dev;
	unsigned char *tb,*rb,*cb,*tf,*rf,*cf;
	int l;
	int omtu=sl->mtu;
	
	sl->mtu=dev->mtu;
	l=(dev->mtu *2);
/*
 * allow for arrival of larger UDP packets, even if we say not to
 * also fixes a bug in which SunOS sends 512-byte packets even with
 * an MSS of 128
 */
	if (l < (576 * 2))
	  l = 576 * 2;
	
	DPRINTF((DBG_SLIP,"SLIP: mtu changed!\n"));
	
	tb= (unsigned char *) kmalloc(l + 4, GFP_ATOMIC);
	rb= (unsigned char *) kmalloc(l + 4, GFP_ATOMIC);
	cb= (unsigned char *) kmalloc(l + 4, GFP_ATOMIC);
	
	if(tb==NULL || rb==NULL || cb==NULL)
	{
		printk("Unable to grow slip buffers. MTU change cancelled.\n");
		sl->mtu=omtu;
		dev->mtu=omtu;
		if(tb!=NULL)
			kfree(tb);
		if(rb!=NULL)
			kfree(rb);
		if(cb!=NULL)
			kfree(cb);
		return;
	}
	
	cli();
	
	tf=(unsigned char *)sl->dev->mem_start;
	sl->dev->mem_start=(unsigned long)tb;
	sl->dev->mem_end=(unsigned long) (sl->dev->mem_start + l);
	rf=(unsigned char *)sl->dev->rmem_start;
	sl->dev->rmem_start=(unsigned long)rb;
	sl->dev->rmem_end=(unsigned long) (sl->dev->rmem_start + l);
	
	sl->xbuff = (unsigned char *) sl->dev->mem_start;
	sl->rbuff = (unsigned char *) sl->dev->rmem_start;
	sl->rend  = (unsigned char *) sl->dev->rmem_end;
	sl->rhead = sl->rbuff;
	
	cf=sl->cbuff;
	sl->cbuff=cb;
	
	sl->escape=0;
	sl->sending=0;
	sl->rcount=0;

	sti();	
	
	if(rf!=NULL)
		kfree(rf);
	if(tf!=NULL)
		kfree(tf);
	if(cf!=NULL)
		kfree(cf);
}


/* Stuff one byte into a SLIP receiver buffer. */
static inline void
sl_enqueue(struct slip *sl, unsigned char c)
{
  unsigned long flags;

  save_flags(flags);
  cli();
  if (sl->rhead < sl->rend) {
	*sl->rhead = c;
	sl->rhead++;
	sl->rcount++;
  } else sl->roverrun++;
  restore_flags(flags);
}

/* Release 'i' bytes from a SLIP receiver buffer. */
static inline void
sl_dequeue(struct slip *sl, int i)
{
  unsigned long flags;

  save_flags(flags);
  cli();
  if (sl->rhead > sl->rbuff) {
	sl->rhead -= i;
	sl->rcount -= i;
  }
  restore_flags(flags);
}


/* Set the "sending" flag.  This must be atomic, hence the ASM. */
static inline void
sl_lock(struct slip *sl)
{
  unsigned long flags;

  save_flags(flags);
  cli();
  sl->sending = 1;
  sl->dev->tbusy = 1;
  restore_flags(flags);
}


/* Clear the "sending" flag.  This must be atomic, hence the ASM. */
static inline void
sl_unlock(struct slip *sl)
{
  unsigned long flags;

  save_flags(flags);
  cli();
  sl->sending = 0;
  sl->dev->tbusy = 0;
  restore_flags(flags);
}


/* Send one completely decapsulated IP datagram to the IP layer. */
static void
sl_bump(struct slip *sl)
{
  int done;
  unsigned char c;
  unsigned long flags;
  int count;

  count = sl->rcount;
  if (sl->mode & (SL_MODE_ADAPTIVE | SL_MODE_CSLIP)) {
    if ((c = sl->rbuff[0]) & SL_TYPE_COMPRESSED_TCP) {
#if 1
      /* ignore compressed packets when CSLIP is off */
      if (!(sl->mode & SL_MODE_CSLIP)) {
	printk("SLIP: compressed packet ignored\n");
	return;
      }
#endif
      /* make sure we've reserved enough space for uncompress to use */
      save_flags(flags);
      cli();
      if ((sl->rhead + 80) < sl->rend) {
	sl->rhead += 80;
	sl->rcount += 80;
	done = 1;
      } else {
	sl->roverrun++;
	done = 0;
      }
      restore_flags(flags);
      if (! done)  /* not enough space available */
	return;

      count = slhc_uncompress(sl->slcomp, sl->rbuff, count);
      if (count <= 0) {
	sl->errors++;
	return;
      }
    } else if (c >= SL_TYPE_UNCOMPRESSED_TCP) {
      if (!(sl->mode & SL_MODE_CSLIP)) {
	/* turn on header compression */
	sl->mode |= SL_MODE_CSLIP;
	printk("SLIP: header compression turned on\n");
      }
      sl->rbuff[0] &= 0x4f;
      if (slhc_remember(sl->slcomp, sl->rbuff, count) <= 0) {
	sl->errors++;
	return;
      }
    }
  }

  DPRINTF((DBG_SLIP, "<< \"%s\" recv:\r\n", sl->dev->name));
  ip_dump(sl->rbuff, sl->rcount);

  /* Bump the datagram to the upper layers... */
  do {
	DPRINTF((DBG_SLIP, "SLIP: packet is %d at 0x%X\n",
					sl->rcount, sl->rbuff));
	/* clh_dump(sl->rbuff, count); */
	done = dev_rint(sl->rbuff, count, 0, sl->dev);
	if (done == 0 || done == 1) break;
  } while(1);

  sl->rpacket++;
}


/* TTY finished sending a datagram, so clean up. */
static void
sl_next(struct slip *sl)
{
  DPRINTF((DBG_SLIP, "SLIP: sl_next(0x%X) called!\n", sl));
  sl_unlock(sl);
  dev_tint(sl->dev);
}


/* Encapsulate one IP datagram and stuff into a TTY queue. */
static void
sl_encaps(struct slip *sl, unsigned char *icp, int len)
{
  unsigned char *bp, *p;
  int count;

  DPRINTF((DBG_SLIP, "SLIP: sl_encaps(0x%X, %d) called\n", icp, len));
  DPRINTF((DBG_SLIP, ">> \"%s\" sent:\r\n", sl->dev->name));
  
  ip_dump(icp, len);
  
  if(sl->mtu != sl->dev->mtu)	/* Someone has been ifconfigging */
  	sl_changedmtu(sl);
  
  if(len>sl->mtu)		/* Sigh, shouldn't occur BUT ... */
  {
  	len=sl->mtu;
  	printk("slip: truncating oversized transmit packet!\n");
  }

  p = icp;
  if(sl->mode & SL_MODE_CSLIP)
	  len = slhc_compress(sl->slcomp, p, len, sl->cbuff, &p, 1);

#ifdef OLD  
  /*
   * Send an initial END character to flush out any
   * data that may have accumulated in the receiver
   * due to line noise.
   */
  bp = sl->xbuff;
  *bp++ = END;
  count = 1;

  /*
   * For each byte in the packet, send the appropriate
   * character sequence, according to the SLIP protocol.
   */
  while(len-- > 0) {
	c = *p++;
	switch(c) {
		case END:
			*bp++ = ESC;
			*bp++ = ESC_END;
			count += 2;
                       	break;
		case ESC:
			*bp++ = ESC;
			*bp++ = ESC_ESC;
			count += 2;
                       	break;
		default:
			*bp++ = c;
			count++;
	}
  }
  *bp++ = END;  
  count++;
#else
  if(sl->mode & SL_MODE_SLIP6)
  	count=slip_esc6(p, (unsigned char *)sl->xbuff,len);
  else
  	count=slip_esc(p, (unsigned char *)sl->xbuff,len);
#endif  	  
  sl->spacket++;
  bp = sl->xbuff;

  /* Tell TTY to send it on its way. */
  DPRINTF((DBG_SLIP, "SLIP: kicking TTY for %d bytes at 0x%X\n", count, bp));
  if (tty_write_data(sl->tty, (char *) bp, count,
	     (void (*)(void *))sl_next, (void *) sl) == 0) {
	DPRINTF((DBG_SLIP, "SLIP: TTY already done with %d bytes!\n", count));
	sl_next(sl);
  }
}

/*static void sl_hex_dump(unsigned char *x,int l)
{
	int n=0;
	printk("sl_xmit: (%d bytes)\n",l);
	while(l)
	{
		printk("%2X ",(int)*x++);
		l--;
		n++;
		if(n%32==0)
			printk("\n");
	}
	if(n%32)
		printk("\n");
}*/

/* Encapsulate an IP datagram and kick it into a TTY queue. */
static int
sl_xmit(struct sk_buff *skb, struct device *dev)
{
  struct tty_struct *tty;
  struct slip *sl;
  int size;

  /* Find the correct SLIP channel to use. */
  sl = &sl_ctrl[dev->base_addr];
  tty = sl->tty;
  DPRINTF((DBG_SLIP, "SLIP: sl_xmit(\"%s\") skb=0x%X busy=%d\n",
				dev->name, skb, sl->sending));

  /*
   * If we are busy already- too bad.  We ought to be able
   * to queue things at this point, to allow for a little
   * frame buffer.  Oh well...
   */
  if (sl->sending) {
	DPRINTF((DBG_SLIP, "SLIP: sl_xmit: BUSY\r\n"));
	sl->sbusy++;
	return(1);
  }

  /* We were not, so we are now... :-) */
  if (skb != NULL) {
#ifdef CONFIG_AX25  
	if(sl->mode & SL_MODE_AX25)
	{
		if(!skb->arp && dev->rebuild_header(skb->data,dev))
		{
			skb->dev=dev;
			arp_queue(skb);
			return 0;
		}
		skb->arp=1;
	}
#endif  	
	sl_lock(sl);
	size = skb->len;
	if (!(sl->mode & SL_MODE_AX25)) {
		if (size < sizeof(struct iphdr)) {
			printk("Runt IP frame fed to slip!\n");
		} else {
			size = ((struct iphdr *)(skb->data))->tot_len;
			size = ntohs(size);
		}
	}
	/*	sl_hex_dump(skb->data,skb->len);*/
	sl_encaps(sl, skb->data, size);
	if (skb->free)
		kfree_skb(skb, FREE_WRITE);
  }
  return(0);
}

/* Return the frame type ID.  This is normally IP but maybe be AX.25. */
static unsigned short
sl_type_trans (struct sk_buff *skb, struct device *dev)
{
#ifdef CONFIG_AX25
	struct slip *sl=&sl_ctrl[dev->base_addr];
	if(sl->mode&SL_MODE_AX25)
		return(NET16(ETH_P_AX25));
#endif
  return(NET16(ETH_P_IP));
}


/* Fill in the MAC-level header. Not used by SLIP. */
static int
sl_header(unsigned char *buff, struct device *dev, unsigned short type,
	  unsigned long daddr, unsigned long saddr, unsigned len)
{
#ifdef CONFIG_AX25
  struct slip *sl=&sl_ctrl[dev->base_addr];
  if((sl->mode&SL_MODE_AX25) && type!=NET16(ETH_P_AX25))
  	return ax25_encapsulate_ip(buff,dev,type,daddr,saddr,len);
#endif  

  return(0);
}


/* Add an ARP-entry for this device's broadcast address. Not used. */
static void
sl_add_arp(unsigned long addr, struct sk_buff *skb, struct device *dev)
{
#ifdef CONFIG_AX25
	struct slip *sl=&sl_ctrl[dev->base_addr];
	
	if(sl->mode&SL_MODE_AX25)
		arp_add(addr,((char *) skb->data)+8,dev);
#endif		
}


/* Rebuild the MAC-level header.  Not used by SLIP. */
static int
sl_rebuild_header(void *buff, struct device *dev)
{
#ifdef CONFIG_AX25
  struct slip *sl=&sl_ctrl[dev->base_addr];
  
  if(sl->mode&SL_MODE_AX25)
  	return ax25_rebuild_header(buff,dev);
#endif  
  return(0);
}


/* Open the low-level part of the SLIP channel. Easy! */
static int
sl_open(struct device *dev)
{
  struct slip *sl;
  unsigned char *p;
  unsigned long l;

  sl = &sl_ctrl[dev->base_addr];
  if (sl->tty == NULL) {
	DPRINTF((DBG_SLIP, "SLIP: channel %d not connected!\n", sl->line));
	return(-ENXIO);
  }
  sl->dev = dev;

  /*
   * Allocate the SLIP frame buffers:
   *
   * mem_end	Top of frame buffers
   * mem_start	Start of frame buffers
   * rmem_end	Top of RECV frame buffer
   * rmem_start	Start of RECV frame buffer
   */
  l = (dev->mtu * 2);
/*
 * allow for arrival of larger UDP packets, even if we say not to
 * also fixes a bug in which SunOS sends 512-byte packets even with
 * an MSS of 128
 */
  if (l < (576 * 2))
    l = 576 * 2;

  p = (unsigned char *) kmalloc(l + 4, GFP_KERNEL);
  if (p == NULL) {
	DPRINTF((DBG_SLIP, "SLIP: no memory for SLIP XMIT buffer!\n"));
	return(-ENOMEM);
  }
  
  sl->mtu		= dev->mtu;
  sl->dev->mem_start	= (unsigned long) p;
  sl->dev->mem_end	= (unsigned long) (sl->dev->mem_start + l);

  p = (unsigned char *) kmalloc(l + 4, GFP_KERNEL);
  if (p == NULL) {
	DPRINTF((DBG_SLIP, "SLIP: no memory for SLIP RECV buffer!\n"));
	return(-ENOMEM);
  }
  sl->dev->rmem_start	= (unsigned long) p;
  sl->dev->rmem_end	= (unsigned long) (sl->dev->rmem_start + l);

  sl->xbuff		= (unsigned char *) sl->dev->mem_start;
  sl->rbuff		= (unsigned char *) sl->dev->rmem_start;
  sl->rend		= (unsigned char *) sl->dev->rmem_end;
  sl->rhead		= sl->rbuff;

  sl->escape		= 0;
  sl->sending		= 0;
  sl->rcount		= 0;

  p = (unsigned char *) kmalloc(l + 4, GFP_KERNEL);
  if (p == NULL) {
  	kfree((unsigned char *)sl->dev->mem_start);
	DPRINTF((DBG_SLIP, "SLIP: no memory for SLIP COMPRESS buffer!\n"));
	return(-ENOMEM);
  }
  sl->cbuff		= p;

  sl->slcomp = slhc_init(16, 16);
  if (sl->slcomp == NULL) {
  	kfree((unsigned char *)sl->dev->mem_start);
  	kfree((unsigned char *)sl->dev->rmem_start);
  	kfree(sl->cbuff);
	DPRINTF((DBG_SLIP, "SLIP: no memory for SLCOMP!\n"));
	return(-ENOMEM);
  }

  dev->flags|=IFF_UP;
  /* Needed because address '0' is special */
  if(dev->pa_addr==0)
  	dev->pa_addr=ntohl(0xC0000001);
  DPRINTF((DBG_SLIP, "SLIP: channel %d opened.\n", sl->line));
  return(0);
}


/* Close the low-level part of the SLIP channel. Easy! */
static int
sl_close(struct device *dev)
{
  struct slip *sl;

  sl = &sl_ctrl[dev->base_addr];
  if (sl->tty == NULL) {
	DPRINTF((DBG_SLIP, "SLIP: channel %d not connected!\n", sl->line));
	return(-EBUSY);
  }
  sl_free(sl);

  /* Free all SLIP frame buffers. */
  kfree(sl->rbuff);
  kfree(sl->xbuff);
  kfree(sl->cbuff);
  slhc_free(sl->slcomp);

  sl_initialize(sl, dev);

  DPRINTF((DBG_SLIP, "SLIP: channel %d closed.\n", sl->line));
  return(0);
}


/*
 * Handle the 'receiver data ready' interrupt.
 * This function is called by the 'tty_io' module in the kernel when
 * a block of SLIP data has been received, which can now be decapsulated
 * and sent on to some IP layer for further processing.
 */
static void
slip_recv(struct tty_struct *tty)
{
  unsigned char buff[128];
  unsigned char *p;
  struct slip *sl;
  int count, error=0;
  
  DPRINTF((DBG_SLIP, "SLIP: slip_recv(%d) called\n", tty->line));
  if ((sl = sl_find(tty)) == NULL) return;	/* not connected */

  if(sl->mtu!=sl->dev->mtu)	/* Argh! mtu change time! - costs us the packet part received at the change */
  	sl_changedmtu(sl);
  	
  /* Suck the bytes out of the TTY queues. */
  do {
	count = tty_read_raw_data(tty, buff, 128);
	if (count <= 0)
	{
		count= - count;
		if(count)
			error=1;
		break;
	}
	p = buff;
#ifdef OLD	
	while (count--) {
		c = *p++;
		if (sl->escape) {
			if (c == ESC_ESC)
				sl_enqueue(sl, ESC);
			else if (c == ESC_END)
				sl_enqueue(sl, END);
			else
				printk ("SLIP: received wrong character\n");
			sl->escape = 0;
		} else {
			if (c == ESC)
				sl->escape = 1;
			else if (c == END) {
				if (sl->rcount > 2) sl_bump(sl);
				sl_dequeue(sl, sl->rcount);
				sl->rcount = 0;
			} else	sl_enqueue(sl, c);
		}
	}
#else
	if(sl->mode & SL_MODE_SLIP6)
		slip_unesc6(sl,buff,count,error);
	else
		slip_unesc(sl,buff,count,error);
#endif		
  } while(1);
  
}


/*
 * Open the high-level part of the SLIP channel.  
 * This function is called by the TTY module when the
 * SLIP line discipline is called for.  Because we are
 * sure the tty line exists, we only have to link it to
 * a free SLIP channel...
 */
static int
slip_open(struct tty_struct *tty)
{
  struct slip *sl;

  /* First make sure we're not already connected. */
  if ((sl = sl_find(tty)) != NULL) {
	DPRINTF((DBG_SLIP, "SLIP: TTY %d already connected to %s !\n",
					tty->line, sl->dev->name));
	return(-EEXIST);
  }

  /* OK.  Find a free SLIP channel to use. */
  if ((sl = sl_alloc()) == NULL) {
	DPRINTF((DBG_SLIP, "SLIP: TTY %d not connected: all channels in use!\n",
						tty->line));
	return(-ENFILE);
  }
  sl->tty = tty;
  tty_read_flush(tty);
  tty_write_flush(tty);

  /* Perform the low-level SLIP initialization. */
  (void) sl_open(sl->dev);
  DPRINTF((DBG_SLIP, "SLIP: TTY %d connected to %s.\n",
				tty->line, sl->dev->name));

  /* Done.  We have linked the TTY line to a channel. */
  return(sl->line);
}

 
static struct enet_statistics *
sl_get_stats(struct device *dev)
{
    static struct enet_statistics stats;
    struct slip *sl;
    struct slcompress *comp;

    /* Find the correct SLIP channel to use. */
    sl = &sl_ctrl[dev->base_addr];
    if (! sl)
      return NULL;

    memset(&stats, 0, sizeof(struct enet_statistics));

    stats.rx_packets = sl->rpacket;
    stats.rx_over_errors = sl->roverrun;
    stats.tx_packets = sl->spacket;
    stats.tx_dropped = sl->sbusy;
    stats.rx_errors = sl->errors;

    comp = sl->slcomp;
    if (comp) {
      stats.rx_fifo_errors = comp->sls_i_compressed;
      stats.rx_dropped = comp->sls_i_tossed;
      stats.tx_fifo_errors = comp->sls_o_compressed;
      stats.collisions = comp->sls_o_misses;
    }

    return (&stats);
}

/*
 * Close down a SLIP channel.
 * This means flushing out any pending queues, and then restoring the
 * TTY line discipline to what it was before it got hooked to SLIP
 * (which usually is TTY again).
 */
static void
slip_close(struct tty_struct *tty)
{
  struct slip *sl;

  /* First make sure we're connected. */
  if ((sl = sl_find(tty)) == NULL) {
	DPRINTF((DBG_SLIP, "SLIP: TTY %d not connected !\n", tty->line));
	return;
  }

  (void) dev_close(sl->dev);
  DPRINTF((DBG_SLIP, "SLIP: TTY %d disconnected from %s.\n",
					tty->line, sl->dev->name));
}

 
 /************************************************************************
  *			STANDARD SLIP ENCAPSULATION			*
  ************************************************************************
  *
  */
 
 int
 slip_esc(unsigned char *s, unsigned char *d, int len)
 {
     int count = 0;
 
     /*
      * Send an initial END character to flush out any
      * data that may have accumulated in the receiver
      * due to line noise.
      */
 
     d[count++] = END;
 
     /*
      * For each byte in the packet, send the appropriate
      * character sequence, according to the SLIP protocol.
      */
 
     while(len-- > 0) {
     	switch(*s) {
     	case END:
     	    d[count++] = ESC;
     	    d[count++] = ESC_END;
 	    break;
 	case ESC:
     	    d[count++] = ESC;
     	    d[count++] = ESC_ESC;
 	    break;
 	default:
 	    d[count++] = *s;
 	}
 	++s;
     }
     d[count++] = END;
     return(count);
 }
 
 void
 slip_unesc(struct slip *sl, unsigned char *s, int count, int error)
 {
     int i;
 
     for (i = 0; i < count; ++i, ++s) {
 	switch(*s) {
 	case ESC:
 	    sl->flags |= SLF_ESCAPE;
 	    break;
 	case ESC_ESC:
 	    if (sl->flags & SLF_ESCAPE)
 	    	sl_enqueue(sl, ESC);
 	    else
 	        sl_enqueue(sl, *s);
	    sl->flags &= ~SLF_ESCAPE;
 	    break;
	case ESC_END:
 	    if (sl->flags & SLF_ESCAPE)
	    	sl_enqueue(sl, END);
	    else
 	        sl_enqueue(sl, *s);
	    sl->flags &= ~SLF_ESCAPE;
	    break;
	case END:
 	    if (sl->rcount > 2) 
 	    	sl_bump(sl);
 	    sl_dequeue(sl, sl->rcount);
 	    sl->rcount = 0;
 	    sl->flags &= ~(SLF_ESCAPE | SLF_ERROR);
 	    break;
 	default:
 	    sl_enqueue(sl, *s);
 	    sl->flags &= ~SLF_ESCAPE;
 	}
     }
     if (error)
     	sl->flags |= SLF_ERROR;
 }
 
 /************************************************************************
  *			 6 BIT SLIP ENCAPSULATION			*
  ************************************************************************
  *
  */
 
 int
 slip_esc6(unsigned char *s, unsigned char *d, int len)
 {
     int count = 0;
     int i;
     unsigned short v = 0;
     short bits = 0;
 
     /*
      * Send an initial END character to flush out any
      * data that may have accumulated in the receiver
      * due to line noise.
      */
 
     d[count++] = 0x70;
 
     /*
      * Encode the packet into printable ascii characters
      */
 
     for (i = 0; i < len; ++i) {
     	v = (v << 8) | s[i];
     	bits += 8;
     	while (bits >= 6) {
     	    unsigned char c;
 
     	    bits -= 6;
     	    c = 0x30 + ((v >> bits) & 0x3F);
     	    d[count++] = c;
 	}
     }
     if (bits) {
     	unsigned char c;
 
     	c = 0x30 + ((v << (6 - bits)) & 0x3F);
     	d[count++] = c;
     }
     d[count++] = 0x70;
     return(count);
 }
 
 void
 slip_unesc6(struct slip *sl, unsigned char *s, int count, int error)
 {
     int i;
     unsigned char c;
 
     for (i = 0; i < count; ++i, ++s) {
     	if (*s == 0x70) {
 	    if (sl->rcount > 8) {	/* XXX must be 2 for compressed slip */
 #ifdef NOTDEF
 	        printk("rbuff %02x %02x %02x %02x\n",
 	            sl->rbuff[0],
 	            sl->rbuff[1],
 	            sl->rbuff[2],
 	            sl->rbuff[3]
 	        );
 #endif
 	    	sl_bump(sl);
 	    }
 	    sl_dequeue(sl, sl->rcount);
 	    sl->rcount = 0;
 	    sl->flags &= ~(SLF_ESCAPE | SLF_ERROR); /* SLF_ESCAPE not used */
 	    sl->xbits = 0;
 	} else if (*s >= 0x30 && *s < 0x70) {
 	    sl->xdata = (sl->xdata << 6) | ((*s - 0x30) & 0x3F);
 	    sl->xbits += 6;
 	    if (sl->xbits >= 8) {
 	    	sl->xbits -= 8;
 	    	c = (unsigned char)(sl->xdata >> sl->xbits);
 		sl_enqueue(sl, c);
 	    }
 
 	}
     }
     if (error)
     	sl->flags |= SLF_ERROR;
 }


#ifdef CONFIG_AX25

int sl_set_mac_address(struct device *dev, void *addr)
{
	int err=verify_area(VERIFY_READ,addr,7);
	if(err)
		return err;
	memcpy_fromfs(dev->dev_addr,addr,7);	/* addr is an AX.25 shifted ASCII mac address */
	return 0;
}
#endif


/* Perform I/O control on an active SLIP channel. */
static int
slip_ioctl(struct tty_struct *tty, void *file, int cmd, void *arg)
{
  struct slip *sl;
  int err;

  /* First make sure we're connected. */
  if ((sl = sl_find(tty)) == NULL) {
	DPRINTF((DBG_SLIP, "SLIP: ioctl: TTY %d not connected !\n", tty->line));
	return(-EINVAL);
  }

  DPRINTF((DBG_SLIP, "SLIP: ioctl(%d, 0x%X, 0x%X)\n", tty->line, cmd, arg));
  switch(cmd) {
	case SIOCGIFNAME:
		err=verify_area(VERIFY_WRITE, arg, 16);
		if(err)
			return -err;
		memcpy_tofs(arg, sl->dev->name, strlen(sl->dev->name) + 1);
		return(0);
	case SIOCGIFENCAP:
		err=verify_area(VERIFY_WRITE,arg,sizeof(long));
		put_fs_long(sl->mode,(long *)arg);
		return(0);
	case SIOCSIFENCAP:
		err=verify_area(VERIFY_READ,arg,sizeof(long));
		sl->mode=get_fs_long((long *)arg);
#ifdef CONFIG_AX25		
		if(sl->mode & SL_MODE_AX25)
		{
			sl->dev->addr_len=7;	/* sizeof an AX.25 addr */
			sl->dev->hard_header_len=17;	/* We don't do digipeaters */
			sl->dev->type=3;		/* AF_AX25 not an AF_INET device */
		}
		else
		{
			sl->dev->addr_len=0;	/* No mac addr in slip mode */
			sl->dev->hard_header_len=0;
			sl->dev->type=0;
		}
#endif		
		return(0);
	case SIOCSIFHWADDR:
#ifdef CONFIG_AX25	
		return sl_set_mac_address(sl->dev,arg);
#endif
	default:
		return(-EINVAL);
  }
  return(-EINVAL);
}


/* Initialize the SLIP driver.  Called by DDI. */
int
slip_init(struct device *dev)
{
  struct slip *sl;
  int i;
#ifdef CONFIG_AX25  
  static char ax25_bcast[7]={'Q'<<1,'S'<<1,'T'<<1,' '<<1,' '<<1,' '<<1,'0'<<1};
  static char ax25_test[7]={'L'<<1,'I'<<1,'N'<<1,'U'<<1,'X'<<1,' '<<1,'1'<<1};
#endif

  sl = &sl_ctrl[dev->base_addr];

  if (already++ == 0) {
	printk("SLIP: version %s (%d channels)\n",
				SLIP_VERSION, SL_NRUNIT);
	printk("CSLIP: code copyright 1989 Regents of the University of California\n");
#ifdef CONFIG_AX25
	printk("AX25: KISS encapsulation enabled\n");
#endif	
	/* Fill in our LDISC request block. */
	sl_ldisc.flags	= 0;
	sl_ldisc.open	= slip_open;
	sl_ldisc.close	= slip_close;
	sl_ldisc.read	= NULL;
	sl_ldisc.write	= NULL;
	sl_ldisc.ioctl	= (int (*)(struct tty_struct *, struct file *,
				   unsigned int, unsigned long)) slip_ioctl;
	sl_ldisc.select = NULL;
	sl_ldisc.handler = slip_recv;
	if ((i = tty_register_ldisc(N_SLIP, &sl_ldisc)) != 0)
		printk("ERROR: %d\n", i);
  }

  /* Set up the "SLIP Control Block". */
  sl_initialize(sl, dev);

  /* Clear all statistics. */
  sl->rcount		= 0;			/* SLIP receiver count	*/
  sl->rpacket		= 0;			/* #frames received	*/
  sl->roverrun		= 0;			/* "overrun" counter	*/
  sl->spacket		= 0;			/* #frames sent out	*/
  sl->sbusy		= 0;			/* "xmit busy" counter	*/
  sl->errors		= 0;			/* not used at present	*/

  /* Finish setting up the DEVICE info. */
  dev->mtu		= SL_MTU;
  dev->hard_start_xmit	= sl_xmit;
  dev->open		= sl_open;
  dev->stop		= sl_close;
  dev->hard_header	= sl_header;
  dev->add_arp		= sl_add_arp;
  dev->type_trans	= sl_type_trans;
  dev->get_stats	= sl_get_stats;
#ifdef HAVE_SET_MAC_ADDR
#ifdef CONFIG_AX25
  dev->set_mac_address  = sl_set_mac_address;
#endif
#endif
  dev->hard_header_len	= 0;
  dev->addr_len		= 0;
  dev->type		= 0;
#ifdef CONFIG_AX25  
  memcpy(dev->broadcast,ax25_bcast,7);		/* Only activated in AX.25 mode */
  memcpy(dev->dev_addr,ax25_test,7);		/*    ""      ""       ""    "" */
#endif  
  dev->queue_xmit	= dev_queue_xmit;
  dev->rebuild_header	= sl_rebuild_header;
  for (i = 0; i < DEV_NUMBUFFS; i++)
		dev->buffs[i] = NULL;

  /* New-style flags. */
  dev->flags		= 0;
  dev->family		= AF_INET;
  dev->pa_addr		= 0;
  dev->pa_brdaddr	= 0;
  dev->pa_mask		= 0;
  dev->pa_alen		= sizeof(unsigned long);

  return(0);
}
