/* 3c501.c: A 3Com 3c501 ethernet driver for linux. */
/*
    Copyright (C) 1992,1993  Donald Becker

    Copyright 1993 United States Government as represented by the
    Director, National Security Agency.  This software may be used and
    distributed according to the terms of the GNU Public License,
    incorporated herein by reference.

    This is a device driver for the 3Com Etherlink 3c501.
    Do not purchase this card, even as a joke.  It's performance is horrible,
    and it breaks in many ways.  

    The Author may be reached as becker@super.org or
    C/O Supercomputing Research Ctr., 17100 Science Dr., Bowie MD 20715
    I'll only accept bug fixes, not reports, for the 3c501 driver.
*/

static char *version =
    "3c501.c: 3/3/94 Donald Becker (becker@super.org).\n";

/*
  Braindamage remaining:
  The 3c501 board.
  */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/ptrace.h>
#include <linux/fcntl.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/malloc.h>
#include <linux/ioport.h>
#include <asm/bitops.h>
#include <asm/io.h>
#include <errno.h>

#include "dev.h"
#include "eth.h"
#include "skbuff.h"
#include "arp.h"

#ifndef HAVE_AUTOIRQ
/* From auto_irq.c, should be in a *.h file. */
extern void autoirq_setup(int waittime);
extern int autoirq_report(int waittime);
extern struct device *irq2dev_map[16];
#endif

#ifndef HAVE_ALLOC_SKB
#define alloc_skb(size, priority) (struct sk_buff *) kmalloc(size,priority)
#define kfree_skbmem(addr, size) kfree_s(addr,size);
#endif


/* Index to functions. */
int el1_probe(struct device *dev);
static int  el_open(struct device *dev);
static int  el_start_xmit(struct sk_buff *skb, struct device *dev);
static void el_interrupt(int reg_ptr);
static void el_receive(struct device *dev);
static void el_reset(struct device *dev);
static int  el1_close(struct device *dev);
static struct enet_statistics *el1_get_stats(struct device *dev);
#ifdef HAVE_MULTICAST
static void set_multicast_list(struct device *dev, int num_addrs, void *addrs);
#endif

#define EL_NAME "EtherLink 3c501"

#ifndef EL_DEBUG
#define EL_DEBUG  2	/* use 0 for production, 1 for devel., >2 for debug */
#endif			/* Anything above 5 is wordy death! */
static int el_debug = EL_DEBUG;
static int el_base;
static struct device *eldev;	/* Only for consistency checking.  */
 
/* We could easily have this struct kmalloc()ed per-board, but
   who would want more than one 3c501?. */
static struct {
    struct enet_statistics stats;
    int tx_pkt_start;		/* The length of the current Tx packet. */
    int collisions;		/* Tx collisions this packet */
} el_status;			/* This should be stored per-board */


#define RX_STATUS (el_base + 0x06)
#define RX_CMD	  RX_STATUS
#define TX_STATUS (el_base + 0x07)
#define TX_CMD	  TX_STATUS
#define GP_LOW 	  (el_base + 0x08)
#define GP_HIGH   (el_base + 0x09)
#define RX_BUF_CLR (el_base + 0x0A)
#define RX_LOW	  (el_base + 0x0A)
#define RX_HIGH   (el_base + 0x0B)
#define SAPROM	  (el_base + 0x0C)
#define AX_STATUS (el_base + 0x0E)
#define AX_CMD	  AX_STATUS
#define DATAPORT  (el_base + 0x0F)
#define TX_RDY 0x08		/* In TX_STATUS */

#define EL1_DATAPTR	0x08
#define EL1_RXPTR	0x0A
#define EL1_SAPROM	0x0C
#define EL1_DATAPORT 	0x0f

/* Writes to the ax command register. */
#define AX_OFF	0x00			/* Irq off, buffer access on */
#define AX_SYS  0x40			/* Load the buffer */
#define AX_XMIT 0x44			/* Transmit a packet */
#define AX_RX	0x48			/* Receive a packet */
#define AX_LOOP	0x0C			/* Loopback mode */
#define AX_RESET 0x80

/* Normal receive mode written to RX_STATUS.  We must intr on short packets
   to avoid bogus rx lockups. */
#define RX_NORM 0xA8		/* 0x68 == all addrs, 0xA8 only to me. */
#define RX_PROM 0x68		/* Senior Prom, uhmm promiscuous mode. */
#define RX_MULT 0xE8		/* Accept multicast packets. */
#define TX_NORM 0x0A	/* Interrupt on everything that might hang the chip */

/* TX_STATUS register. */
#define TX_COLLISION 0x02
#define TX_16COLLISIONS 0x04
#define TX_READY 0x08

#define RX_RUNT 0x08
#define RX_MISSED 0x01		/* Missed a packet due to 3c501 braindamage. */
#define RX_GOOD	0x30		/* Good packet 0x20, or simple overflow 0x10. */


int
el1_probe(struct device *dev)
{
    int i;
    int ioaddr;
    unsigned char station_addr[6];
    int autoirq = 0;

    eldev = dev;		/* Store for debugging. */
    el_base = dev->base_addr;

    if (el_base < 0x40)		/* Invalid?  Probe for it. */
	el_base = 0x280;

    ioaddr = el_base;

    /* Read the station address PROM data from the special port.  */
    for (i = 0; i < 6; i++) {
	outw(i, ioaddr + EL1_DATAPTR);
	station_addr[i] = inb(ioaddr + EL1_SAPROM);
    }
    /* Check the first three octets of the S.A. for 3Com's code. */ 
    if (station_addr[0] != 0x02  ||  station_addr[1] != 0x60
	|| station_addr[2] != 0x8c) {
	return ENODEV;
    }

#ifdef HAVE_PORTRESERVE
    /* Grab the region so we can find the another board if autoIRQ fails. */
    snarf_region(ioaddr, 16);
#endif

    /* We auto-IRQ by shutting off the interrupt line and letting it float
       high. */
    if (dev->irq < 2) {

	autoirq_setup(2);

	inb(RX_STATUS);		/* Clear pending interrupts. */
	inb(TX_STATUS);
	outb(AX_LOOP + 1, AX_CMD);

	outb(0x00, AX_CMD);

	autoirq = autoirq_report(1);

	if (autoirq == 0) {
	    printk("%s: 3c501 probe failed to detect IRQ line.\n", dev->name);
	    return EAGAIN;
	}
	dev->irq = autoirq;
    }

    outb(AX_RESET+AX_LOOP, AX_CMD);			/* Loopback mode. */

    dev->base_addr = el_base;
    memcpy(dev->dev_addr, station_addr, ETH_ALEN);
    if (dev->mem_start & 0xf)
	el_debug = dev->mem_start & 0x7;

    printk("%s: 3c501 EtherLink at %#x, using %sIRQ %d, melting ethernet.\n",
	   dev->name, dev->base_addr, autoirq ? "auto":"assigned ", dev->irq);

    if (el_debug)
	printk("%s", version);

    /* The EL1-specific entries in the device structure. */
    dev->open = &el_open;
    dev->hard_start_xmit = &el_start_xmit;
    dev->stop = &el1_close;
    dev->get_stats = &el1_get_stats;
#ifdef HAVE_MULTICAST
    dev->set_multicast_list = &set_multicast_list;
#endif

    /* Fill in the generic field of the device structure. */
    for (i = 0; i < DEV_NUMBUFFS; i++)
	dev->buffs[i] = NULL;

    dev->hard_header	= eth_header;
    dev->add_arp	= eth_add_arp;
    dev->queue_xmit	= dev_queue_xmit;
    dev->rebuild_header	= eth_rebuild_header;
    dev->type_trans	= eth_type_trans;

    dev->type		= ARPHRD_ETHER;
    dev->hard_header_len = ETH_HLEN;
    dev->mtu		= 1500; /* eth_mtu */
    dev->addr_len	= ETH_ALEN;
    for (i = 0; i < ETH_ALEN; i++) {
	dev->broadcast[i]=0xff;
    }

    /* New-style flags. */
    dev->flags		= IFF_BROADCAST;
    dev->family		= AF_INET;
    dev->pa_addr	= 0;
    dev->pa_brdaddr	= 0;
    dev->pa_mask	= 0;
    dev->pa_alen	= sizeof(unsigned long);

    return 0;
}

/* Open/initialize the board. */
static int
el_open(struct device *dev)
{

  if (el_debug > 2)
      printk("%s: Doing el_open()...", dev->name);

  if (request_irq(dev->irq, &el_interrupt)) {
      if (el_debug > 2)
	  printk("interrupt busy, exiting el_open().\n");
      return -EAGAIN;
  }
  irq2dev_map[dev->irq] = dev;

  el_reset(dev);

  dev->start = 1;

  outb(AX_RX, AX_CMD);	/* Aux control, irq and receive enabled */
  if (el_debug > 2)
     printk("finished el_open().\n");
  return (0);
}

static int
el_start_xmit(struct sk_buff *skb, struct device *dev)
{

    if (dev->tbusy) {
	if (jiffies - dev->trans_start < 20) {
	    if (el_debug > 2)
		printk(" transmitter busy, deferred.\n");
	    return 1;
	}
	if (el_debug)
	    printk ("%s: transmit timed out, txsr %#2x axsr=%02x rxsr=%02x.\n",
		    dev->name, inb(TX_STATUS), inb(AX_STATUS), inb(RX_STATUS));
	el_status.stats.tx_errors++;
#ifdef oldway
	el_reset(dev);
#else
	outb(TX_NORM, TX_CMD);
	outb(RX_NORM, RX_CMD);
	outb(AX_OFF, AX_CMD);	/* Just trigger a false interrupt. */
#endif
	outb(AX_RX, AX_CMD);	/* Aux control, irq and receive enabled */
	dev->tbusy = 0;
	dev->trans_start = jiffies;
    }

    if (skb == NULL) {
	dev_tint(dev);
	return 0;
    }

    /* Fill in the ethernet header. */
    if (!skb->arp  &&  dev->rebuild_header(skb->data, dev)) {
	skb->dev = dev;
	arp_queue (skb);
	return 0;
    }
    skb->arp=1;

    if (skb->len <= 0)
	return 0;

    /* Avoid timer-based retransmission conflicts. */
    if (set_bit(0, (void*)&dev->tbusy) != 0)
	printk("%s: Transmitter access conflict.\n", dev->name);
    else {
	int gp_start = 0x800 - (ETH_ZLEN < skb->len ? skb->len : ETH_ZLEN);
	unsigned char *buf = skb->data;

	el_status.tx_pkt_start = gp_start;
    	el_status.collisions = 0;

	outb(AX_SYS, AX_CMD);
	inb(RX_STATUS);
	inb(TX_STATUS);
	outb(0x00, RX_BUF_CLR);	/* Set rx packet area to 0. */
	outw(gp_start, GP_LOW);
	outsb(DATAPORT,buf,skb->len);
	outw(gp_start, GP_LOW);
	outb(AX_XMIT, AX_CMD);		/* Trigger xmit.  */
	dev->trans_start = jiffies;
    }

    if (el_debug > 2)
	printk(" queued xmit.\n");
    if (skb->free)
	kfree_skb (skb, FREE_WRITE);
    return 0;
}


/* The typical workload of the driver:
   Handle the ether interface interrupts. */
static void
el_interrupt(int reg_ptr)
{
    int irq = -(((struct pt_regs *)reg_ptr)->orig_eax+2);
    /*struct device *dev = (struct device *)(irq2dev_map[irq]);*/
    struct device *dev = eldev;
    int axsr;			/* Aux. status reg. */
    short ioaddr;

    if (eldev->irq != irq) {
	printk (EL_NAME ": irq %d for unknown device\n", irq);
	return;
    }

    ioaddr = dev->base_addr;

    axsr = inb(AX_STATUS);

    if (el_debug > 3)
      printk("%s: el_interrupt() aux=%#02x", dev->name, axsr);
    if (dev->interrupt)
	printk("%s: Reentering the interrupt driver!\n", dev->name);
    dev->interrupt = 1;

    if (dev->tbusy) {
	int txsr = inb(TX_STATUS);

	if (el_debug > 6)
	    printk(" txsr=%02x gp=%04x rp=%04x", txsr, inw(GP_LOW),
		   inw(RX_LOW));

	if ((axsr & 0x80) && (txsr & TX_READY) == 0) {
	    printk("%s: Unusual interrupt during Tx, txsr=%02x axsr=%02x"
		   " gp=%03x rp=%03x.\n", dev->name, txsr, axsr,
		   inw(ioaddr + EL1_DATAPTR), inw(ioaddr + EL1_RXPTR));
	    dev->tbusy = 0;
	    mark_bh(INET_BH);
	} else if (txsr & TX_16COLLISIONS) {
	    if (el_debug)
		printk("%s: Transmit failed 16 times, ethernet jammed?\n",
		       dev->name);
	    outb(AX_SYS, AX_CMD);
	    el_status.stats.tx_aborted_errors++;
	} else if (txsr & TX_COLLISION) {	/* Retrigger xmit. */
	    if (el_debug > 6)
		printk(" retransmitting after a collision.\n");
	    outb(AX_SYS, AX_CMD);
	    outw(el_status.tx_pkt_start, GP_LOW);
	    outb(AX_XMIT, AX_CMD);
	    el_status.stats.collisions++;
	    dev->interrupt = 0;
	    return;
	} else {
	    el_status.stats.tx_packets++;
	    if (el_debug > 6)
		printk(" Tx succeeded %s\n",
		       (txsr & TX_RDY) ? "." : "but tx is busy!");
	    dev->tbusy = 0;
	    mark_bh(INET_BH);
	}
    } else {
	int rxsr = inb(RX_STATUS);
	if (el_debug > 5)
	    printk(" rxsr=%02x txsr=%02x rp=%04x", rxsr, inb(TX_STATUS),
		   inw(RX_LOW));

	/* Just reading rx_status fixes most errors. */
	if (rxsr & RX_MISSED)
	    el_status.stats.rx_missed_errors++;
	if (rxsr & RX_RUNT) {	/* Handled to avoid board lock-up. */
	    el_status.stats.rx_length_errors++;
	    if (el_debug > 5) printk(" runt.\n");
	} else if (rxsr & RX_GOOD) {
	    el_receive(eldev);
	} else {			/* Nothing?  Something is broken! */
	    if (el_debug > 2)
		printk("%s: No packet seen, rxsr=%02x **resetting 3c501***\n",
		       dev->name, rxsr);
	    el_reset(eldev);
	}
	if (el_debug > 3)
	    printk(".\n");
    }

    outb(AX_RX, AX_CMD);
    outb(0x00, RX_BUF_CLR);
    inb(RX_STATUS);		/* Be certain that interrupts are cleared. */
    inb(TX_STATUS);
    dev->interrupt = 0;
    return;
}


/* We have a good packet. Well, not really "good", just mostly not broken.
   We must check everything to see if it is good. */
static void
el_receive(struct device *dev)
{
    int sksize, pkt_len;
    struct sk_buff *skb;

    pkt_len = inw(RX_LOW);

    if (el_debug > 4)
      printk(" el_receive %d.\n", pkt_len);

    if ((pkt_len < 60)  ||  (pkt_len > 1536)) {
	if (el_debug)
	  printk("%s: bogus packet, length=%d\n", dev->name, pkt_len);
	el_status.stats.rx_over_errors++;
	return;
    }
    outb(AX_SYS, AX_CMD);

    sksize = sizeof(struct sk_buff) + pkt_len;
    skb = alloc_skb(sksize, GFP_ATOMIC);
    outw(0x00, GP_LOW);
    if (skb == NULL) {
	printk("%s: Memory squeeze, dropping packet.\n", dev->name);
	el_status.stats.rx_dropped++;
	return;
    } else {
	skb->mem_len = sksize;
	skb->mem_addr = skb;
	skb->len = pkt_len;
	skb->dev = dev;

	insb(DATAPORT, skb->data, pkt_len);

#ifdef HAVE_NETIF_RX
	    netif_rx(skb);
#else
	    skb->lock = 0;
	    if (dev_rint((unsigned char*)skb, pkt_len, IN_SKBUFF, dev) != 0) {
		kfree_skbmem(skb, sksize);
		lp->stats.rx_dropped++;
		break;
	    }
#endif
	el_status.stats.rx_packets++;
    }
    return;
}

static void 
el_reset(struct device *dev)
{
    if (el_debug> 2)
	printk("3c501 reset...");
    outb(AX_RESET, AX_CMD);	/* Reset the chip */
    outb(AX_LOOP, AX_CMD);	/* Aux control, irq and loopback enabled */
    {
	int i;
	for (i = 0; i < 6; i++)	/* Set the station address. */
	    outb(dev->dev_addr[i], el_base + i);
    }
    
    outb(0, RX_BUF_CLR);		/* Set rx packet area to 0. */
    cli();			/* Avoid glitch on writes to CMD regs */
    outb(TX_NORM, TX_CMD);		/* tx irq on done, collision */
    outb(RX_NORM, RX_CMD);	/* Set Rx commands. */
    inb(RX_STATUS);		/* Clear status. */
    inb(TX_STATUS);
    dev->interrupt = 0;
    dev->tbusy = 0;
    sti();
}

static int
el1_close(struct device *dev)
{
    int ioaddr = dev->base_addr;

    if (el_debug > 2)
	printk("%s: Shutting down ethercard at %#x.\n", dev->name, ioaddr);

    dev->tbusy = 1;
    dev->start = 0;

    /* Free and disable the IRQ. */
    free_irq(dev->irq);
    outb(AX_RESET, AX_CMD);	/* Reset the chip */
    irq2dev_map[dev->irq] = 0;

    return 0;
}

static struct enet_statistics *
el1_get_stats(struct device *dev)
{
    return &el_status.stats;
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
    if (num_addrs > 0) {
	outb(RX_MULT, RX_CMD);
	inb(RX_STATUS);		/* Clear status. */
    } else if (num_addrs < 0) {
	outb(RX_PROM, RX_CMD);
	inb(RX_STATUS);
    } else {
	outb(RX_NORM, RX_CMD);
	inb(RX_STATUS);
    }
}
#endif

/*
 * Local variables:
 *  compile-command: "gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O6 -fomit-frame-pointer  -m486 -c -o 3c501.o 3c501.c"
 *  kept-new-versions: 5
 * End:
 */
