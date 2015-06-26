/* skeleton.c: A sample network driver core for linux. */
/*
	Written 1993 by Donald Becker.
	Copyright 1993 United States Government as represented by the Director,
	National Security Agency.  This software may only be used and distributed
	according to the terms of the GNU Public License as modified by SRC,
	incorporated herein by reference.

	The author may be reached as becker@super.org or
	C/O Supercomputing Research Ctr., 17100 Science Dr., Bowie MD 20715

	This file is an outline for writing a network device driver for the
	the Linux operating system.

	To write (or understand) a driver, have a look at the "loopback.c" file to
	get a feel of what is going on, and then use the code below as a skeleton
	for the new driver.

*/

static char *version =
	"skeleton.c:v0.05 11/16/93 Donald Becker (becker@super.org)\n";

/* Always include 'config.h' first in case the user wants to turn on
   or override something. */
#include <linux/config.h>

/*
  Sources:
	List your sources of programming information to document that
	the driver is your own creation, and give due credit to others
	that contributed to the work.  Remember that GNU project code
	cannot use proprietary or trade secret information.	 Interface
	definitions are generally considered non-copyrightable to the
	extent that the same names and structures must be used to be
	compatible.

	Finally, keep in mind that the Linux kernel is has an API, not
	ABI.  Proprietary object-code-only distributions are not permitted
	under the GPL.
*/

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>
#include <linux/ioport.h>
#include <linux/in.h>
#include <linux/malloc.h>
#include <linux/string.h>
#include <asm/system.h>
#include <asm/bitops.h>
#include <asm/io.h>
#include <asm/dma.h>
#include <errno.h>

#include "dev.h"
#include "eth.h"
#include "skbuff.h"
#include "arp.h"

#ifndef HAVE_AUTOIRQ
/* From auto_irq.c, in ioport.h for later versions. */
extern void autoirq_setup(int waittime);
extern int autoirq_report(int waittime);
/* The map from IRQ number (as passed to the interrupt handler) to
   'struct device'. */
extern struct device *irq2dev_map[16];
#endif

#ifndef HAVE_ALLOC_SKB
#define alloc_skb(size, priority) (struct sk_buff *) kmalloc(size,priority)
#define kfree_skbmem(addr, size) kfree_s(addr,size);
#endif

#ifndef HAVE_PORTRESERVE
#define check_region(ioaddr, size) 		0
#define	snarf_region(ioaddr, size);		do ; while (0)
#endif

/* use 0 for production, 1 for verification, >2 for debug */
#ifndef NET_DEBUG
#define NET_DEBUG 2
#endif
static unsigned int net_debug = NET_DEBUG;

/* Information that need to be kept for each board. */
struct net_local {
	struct enet_statistics stats;
	long open_time;				/* Useless example local info. */
};

/* The number of low I/O ports used by the ethercard. */
#define ETHERCARD_TOTAL_SIZE	16

/* The station (ethernet) address prefix, used for IDing the board. */
#define SA_ADDR0 0x00
#define SA_ADDR1 0x42
#define SA_ADDR2 0x65

/* Index to functions, as function prototypes. */

extern int netcard_probe(struct device *dev);

static int netcard_probe1(struct device *dev, short ioaddr);
static int net_open(struct device *dev);
static int	net_send_packet(struct sk_buff *skb, struct device *dev);
static void net_interrupt(int reg_ptr);
static void net_rx(struct device *dev);
static int net_close(struct device *dev);
static struct enet_statistics *net_get_stats(struct device *dev);
#ifdef HAVE_MULTICAST
static void set_multicast_list(struct device *dev, int num_addrs, void *addrs);
#endif

/* Example routines you must write ;->. */
#define tx_done(dev) 1
extern void	hardware_send_packet(short ioaddr, char *buf, int length);
extern void chipset_init(struct device *dev, int startp);


/* Check for a network adaptor of this type, and return '0' iff one exists.
   If dev->base_addr == 0, probe all likely locations.
   If dev->base_addr == 1, always return failure.
   If dev->base_addr == 2, alloate space for the device and return success
   (detachable devices only).
   */
int
netcard_probe(struct device *dev)
{
	int *port, ports[] = {0x300, 0x280, 0};
	int base_addr = dev->base_addr;

	if (base_addr > 0x1ff)		/* Check a single specified location. */
		return netcard_probe1(dev, base_addr);
	else if (base_addr > 0)		/* Don't probe at all. */
		return ENXIO;

	for (port = &ports[0]; *port; port++) {
		int ioaddr = *port;
		if (check_region(ioaddr, ETHERCARD_TOTAL_SIZE))
			continue;
		if (inb(ioaddr) != 0x57)
			continue;
		dev->base_addr = ioaddr;
		if (netcard_probe1(dev, ioaddr) == 0)
			return 0;
	}

	dev->base_addr = base_addr;
	return ENODEV;
}

int netcard_probe1(struct device *dev, short ioaddr)
{
	unsigned char station_addr[6];
	int i;

	/* Read the station address PROM.  */
	for (i = 0; i < 6; i++) {
		station_addr[i] = inb(ioaddr + i);
	}
	/* Check the first three octets of the S.A. for the manufactor's code. */ 
	if (station_addr[0] != SA_ADDR0
		||	 station_addr[1] != SA_ADDR1 || station_addr[2] != SA_ADDR2) {
		return ENODEV;
	}

	printk("%s: %s found at %#3x, IRQ %d.\n", dev->name,
		   "network card", dev->base_addr, dev->irq);

#ifdef jumpered_interrupts
	/* If this board has jumpered interrupts, snarf the interrupt vector
	   now.	 There is no point in waiting since no other device can use
	   the interrupt, and this marks the 'irqaction' as busy. */

	if (dev->irq == -1)
		;			/* Do nothing: a user-level program will set it. */
	else if (dev->irq < 2) {	/* "Auto-IRQ" */
		autoirq_setup(0);
		/* Trigger an interrupt here. */

		dev->irq = autoirq_report(0);
		if (net_debug >= 2)
			printk(" autoirq is %d", dev->irq);
  } else if (dev->irq == 2)
	  /* Fixup for users that don't know that IRQ 2 is really IRQ 9,
	 or don't know which one to set. */
	  dev->irq = 9;

	{	 int irqval = request_irq(dev->irq, &net_interrupt);
		 if (irqval) {
			 printk ("%s: unable to get IRQ %d (irqval=%d).\n", dev->name,
					 dev->irq, irqval);
			 return EAGAIN;
		 }
	 }
#endif	/* jumpered interrupt */

	/* Grab the region so we can find another board if autoIRQ fails. */
	snarf_region(ioaddr, ETHERCARD_TOTAL_SIZE);

	if (net_debug)
		printk(version);

	/* Initialize the device structure. */
	dev->priv = kmalloc(sizeof(struct net_local), GFP_KERNEL);
	memset(dev->priv, 0, sizeof(struct net_local));

	dev->open		= net_open;
	dev->stop		= net_close;
	dev->hard_start_xmit = net_send_packet;
	dev->get_stats	= net_get_stats;
#ifdef HAVE_MULTICAST
	dev->set_multicast_list = &set_multicast_list;
#endif

	/* Fill in the fields of the device structure with ethernet-generic values.
	   This should be in a common file instead of per-driver.  */
	for (i = 0; i < DEV_NUMBUFFS; i++)
		dev->buffs[i] = NULL;

	dev->hard_header	= eth_header;
	dev->add_arp		= eth_add_arp;
	dev->queue_xmit		= dev_queue_xmit;
	dev->rebuild_header	= eth_rebuild_header;
	dev->type_trans		= eth_type_trans;

	dev->type			= ARPHRD_ETHER;
	dev->hard_header_len = ETH_HLEN;
	dev->mtu			= 1500; /* eth_mtu */
	dev->addr_len		= ETH_ALEN;
	for (i = 0; i < ETH_ALEN; i++) {
		dev->broadcast[i]=0xff;
	}

	/* New-style flags. */
	dev->flags			= IFF_BROADCAST;
	dev->family			= AF_INET;
	dev->pa_addr		= 0;
	dev->pa_brdaddr		= 0;
	dev->pa_mask		= 0;
	dev->pa_alen		= sizeof(unsigned long);

	return 0;
}


/* Open/initialize the board.  This is called (in the current kernel)
   sometime after booting when the 'ifconfig' program is run.

   This routine should set everything up anew at each open, even
   registers that "should" only need to be set once at boot, so that
   there is non-reboot way to recover if something goes wrong.
   */
static int
net_open(struct device *dev)
{
	struct net_local *lp = (struct net_local *)dev->priv;
	int ioaddr = dev->base_addr;

	/* This is used if the interrupt line can turned off (shared).
	   See 3c503.c for an example of selecting the IRQ at config-time. */
	if (request_irq(dev->irq, &net_interrupt)) {
		return -EAGAIN;
	}


	/* Always snarf a DMA channel after the IRQ. */
	if (request_dma(dev->dma)) {
		free_irq(dev->irq);
		return -EAGAIN;
	}
	irq2dev_map[dev->irq] = dev;

	/* Reset the hardware here. */
	/*chipset_init(dev, 1);*/
	outb(0x00, ioaddr);
	lp->open_time = jiffies;

	dev->tbusy = 0;
	dev->interrupt = 0;
	dev->start = 1;
	return 0;
}

static int
net_send_packet(struct sk_buff *skb, struct device *dev)
{
	struct net_local *lp = (struct net_local *)dev->priv;
	int ioaddr = dev->base_addr;

	if (dev->tbusy) {
		/* If we get here, some higher level has decided we are broken.
		   There should really be a "kick me" function call instead. */
		int tickssofar = jiffies - dev->trans_start;
		if (tickssofar < 5)
			return 1;
		printk("%s: transmit timed out, %s?\n", dev->name,
			   tx_done(dev) ? "IRQ conflict" : "network cable problem");
		/* Try to restart the adaptor. */
		chipset_init(dev, 1);
		dev->tbusy=0;
		dev->trans_start = jiffies;
	}

	/* If some higher layer thinks we've missed an tx-done interrupt
	   we are passed NULL. Caution: dev_tint() handles the cli()/sti()
	   itself. */
	if (skb == NULL) {
		dev_tint(dev);
		return 0;
	}

	/* For ethernet, fill in the header.  This should really be done by a
	   higher level, rather than duplicated for each ethernet adaptor. */
	if (!skb->arp  &&  dev->rebuild_header(skb->data, dev)) {
		skb->dev = dev;
		arp_queue (skb);
		return 0;
	}
	skb->arp=1;

	/* Block a timer-based transmit from overlapping.  This could better be
	   done with atomic_swap(1, dev->tbusy), but set_bit() works as well. */
	if (set_bit(0, (void*)&dev->tbusy) != 0)
		printk("%s: Transmitter access conflict.\n", dev->name);
	else {
		short length = ETH_ZLEN < skb->len ? skb->len : ETH_ZLEN;
		unsigned char *buf = skb->data;

		hardware_send_packet(ioaddr, buf, length);
		dev->trans_start = jiffies;
	}
	if (skb->free)
		kfree_skb (skb, FREE_WRITE);

	/* You might need to clean up and record Tx statistics here. */
	if (inw(ioaddr) == /*RU*/81)
		lp->stats.tx_aborted_errors++;

	return 0;
}

/* The typical workload of the driver:
   Handle the network interface interrupts. */
static void
net_interrupt(int reg_ptr)
{
	int irq = -(((struct pt_regs *)reg_ptr)->orig_eax+2);
	struct device *dev = (struct device *)(irq2dev_map[irq]);
	struct net_local *lp;
	int ioaddr, status, boguscount = 0;

	if (dev == NULL) {
		printk ("net_interrupt(): irq %d for unknown device.\n", irq);
		return;
	}
	dev->interrupt = 1;

	ioaddr = dev->base_addr;
	lp = (struct net_local *)dev->priv;
	status = inw(ioaddr + 0);

	do {
		if (status /*& RX_INTR*/) {
			/* Got a packet(s). */
			net_rx(dev);
		}
		if (status /*& TX_INTR*/) {
			lp->stats.tx_packets++;
			dev->tbusy = 0;
			mark_bh(INET_BH);	/* Inform upper layers. */
		}
		if (status /*& COUNTERS_INTR*/) {
			/* Increment the appropriate 'localstats' field. */
			lp->stats.tx_window_errors++;
		}
	} while (++boguscount < 20) ;

	return;
}

/* We have a good packet(s), get it/them out of the buffers. */
static void
net_rx(struct device *dev)
{
	struct net_local *lp = (struct net_local *)dev->priv;
	int ioaddr = dev->base_addr;
	int boguscount = 10;

	do {
		int status = inw(ioaddr);
		int pkt_len = inw(ioaddr);
	  
		if (pkt_len == 0)		/* Read all the frames? */
			break;			/* Done for now */

		if (status & 0x40) {	/* There was an error. */
			lp->stats.rx_errors++;
			if (status & 0x20) lp->stats.rx_frame_errors++;
			if (status & 0x10) lp->stats.rx_over_errors++;
			if (status & 0x08) lp->stats.rx_crc_errors++;
			if (status & 0x04) lp->stats.rx_fifo_errors++;
		} else {
			/* Malloc up new buffer. */
			int sksize = sizeof(struct sk_buff) + pkt_len;
			struct sk_buff *skb;

			skb = alloc_skb(sksize, GFP_ATOMIC);
			if (skb == NULL) {
				printk("%s: Memory squeeze, dropping packet.\n", dev->name);
				lp->stats.rx_dropped++;
				break;
			}
			skb->mem_len = sksize;
			skb->mem_addr = skb;
			skb->len = pkt_len;
			skb->dev = dev;

			/* 'skb->data' points to the start of sk_buff data area. */
			memcpy(skb->data, (void*)dev->rmem_start,
				   pkt_len);
			/* or */
			insw(ioaddr, skb->data, (pkt_len + 1) >> 1);

#ifdef HAVE_NETIF_RX
			netif_rx(skb);
#else
			skb->lock = 0;
			if (dev_rint((unsigned char*)skb, pkt_len, IN_SKBUFF, dev) != 0) {
				kfree_s(skb, sksize);
				lp->stats.rx_dropped++;
				break;
			}
#endif
			lp->stats.rx_packets++;
		}
	} while (--boguscount);

	/* If any worth-while packets have been received, dev_rint()
	   has done a mark_bh(INET_BH) for us and will work on them
	   when we get to the bottom-half routine. */
	return;
}

/* The inverse routine to net_open(). */
static int
net_close(struct device *dev)
{
	struct net_local *lp = (struct net_local *)dev->priv;
	int ioaddr = dev->base_addr;

	lp->open_time = 0;

	dev->tbusy = 1;
	dev->start = 0;

	/* Flush the Tx and disable Rx here. */

	disable_dma(dev->dma);

	/* If not IRQ or DMA jumpered, free up the line. */
	outw(0x00, ioaddr+0);		/* Release the physical interrupt line. */

	free_irq(dev->irq);
	free_dma(dev->dma);

	irq2dev_map[dev->irq] = 0;

	/* Update the statistics here. */

	return 0;

}

/* Get the current statistics.	This may be called with the card open or
   closed. */
static struct enet_statistics *
net_get_stats(struct device *dev)
{
	struct net_local *lp = (struct net_local *)dev->priv;
	short ioaddr = dev->base_addr;

	cli();
	/* Update the statistics from the device registers. */
	lp->stats.rx_missed_errors = inw(ioaddr+1);
	sti();

	return &lp->stats;
}

#ifdef HAVE_MULTICAST
/* Set or clear the multicast filter for this adaptor.
   num_addrs == -1	Promiscuous mode, receive all packets
   num_addrs == 0	Normal mode, clear multicast list
   num_addrs > 0	Multicast mode, receive normal and MC packets, and do
			best-effort filtering.
 */
static void
set_multicast_list(struct device *dev, int num_addrs, void *addrs)
{
	short ioaddr = dev->base_addr;
	if (num_addrs) {
		outw(69, ioaddr);		/* Enable promiscuous mode */
	} else
		outw(99, ioaddr);		/* Disable promiscuous mode, use normal mode */
}
#endif

/*
 * Local variables:
 *  compile-command: "gcc -D__KERNEL__ -I/usr/src/linux/net/inet -Wall -Wstrict-prototypes -O6 -m486 -c skeleton.c"
 *  version-control: t
 *  kept-new-versions: 5
 *  tab-width: 4
 * End:
 */
