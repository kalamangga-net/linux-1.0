/* 3c509.c: A 3c509 EtherLink3 ethernet driver for linux. */
/*
	Written 1993 by Donald Becker.

	Copyright 1993 United States Government as represented by the
	Director, National Security Agency.	 This software may be used and
	distributed according to the terms of the GNU Public License,
	incorporated herein by reference.
	
	This driver is for the 3Com EtherLinkIII series.

	The author may be reached as becker@super.org or
	C/O Supercomputing Research Ctr., 17100 Science Dr., Bowie MD 20715
*/

static char *version = "3c509.c:pl15k 3/5/94 becker@super.org\n";

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>
#include <linux/errno.h>
#include <linux/in.h>
#include <linux/malloc.h>
#include <linux/ioport.h>
#include <asm/bitops.h>
#include <asm/io.h>

#include "dev.h"
#include "eth.h"
#include "skbuff.h"
#include "arp.h"

#ifndef HAVE_ALLOC_SKB
#define alloc_skb(size, priority) (struct sk_buff *) kmalloc(size,priority)
#endif


#ifdef EL3_DEBUG
int el3_debug = EL3_DEBUG;
#else
int el3_debug = 2;
#endif

/* To minimize the size of the driver source I only define operating
   constants if they are used several times.  You'll need the manual
   if you want to understand driver details. */
/* Offsets from base I/O address. */
#define EL3_DATA 0x00
#define EL3_CMD 0x0e
#define EL3_STATUS 0x0e
#define ID_PORT 0x100
#define	 EEPROM_READ 0x80

#define EL3WINDOW(win_num) outw(0x0800+(win_num), ioaddr + EL3_CMD)

/* Register window 1 offsets, the window used in normal operation. */
#define TX_FIFO		0x00
#define RX_FIFO		0x00
#define RX_STATUS 	0x08
#define TX_STATUS 	0x0B
#define TX_FREE		0x0C		/* Remaining free bytes in Tx buffer. */

#define WN4_MEDIA	0x0A		/* Window 4: Various transceiver/media bits. */
#define  MEDIA_TP	0x00C0		/* Enable link beat and jabber for 10baseT. */

struct el3_private {
	struct enet_statistics stats;
};

static ushort id_read_eeprom(int index);
static ushort read_eeprom(short ioaddr, int index);
static int el3_open(struct device *dev);
static int el3_start_xmit(struct sk_buff *skb, struct device *dev);
static void el3_interrupt(int reg_ptr);
static void update_stats(int addr, struct device *dev);
static struct enet_statistics *el3_get_stats(struct device *dev);
static int el3_rx(struct device *dev);
static int el3_close(struct device *dev);
#ifdef HAVE_MULTICAST
static void set_multicast_list(struct device *dev, int num_addrs, void *addrs);
#endif



int el3_probe(struct device *dev)
{
	short lrs_state = 0xff, i;
	ushort ioaddr, irq, if_port;
	short *phys_addr = (short *)dev->dev_addr;
	static int current_tag = 0;

	/* First check for a board on the EISA bus. */
	if (EISA_bus) {
		for (ioaddr = 0x1000; ioaddr < 0x9000; ioaddr += 0x1000) {
			if (inw(ioaddr) != 0x6d50)
				continue;

			irq = inw(ioaddr + 8) >> 12;
			if_port = inw(ioaddr + 6)>>14;
			for (i = 0; i < 3; i++)
				phys_addr[i] = htons(read_eeprom(ioaddr, i));

			/* Restore the "Manufacturer ID" to the EEPROM read register. */
			/* The manual says to restore "Product ID" (reg. 3). !???! */
			read_eeprom(ioaddr, 7);

			/* Was the EISA code an add-on hack?  Nahhhhh... */
			goto found;
		}
	}

#ifdef CONFIG_MCA
	if (MCA_bus) {
		mca_adaptor_select_mode(1);
		for (i = 0; i < 8; i++)
			if ((mca_adaptor_id(i) | 1) == 0x627c) {
				ioaddr = mca_pos_base_addr(i);
				irq = inw(ioaddr + 8) >> 12;
				if_port = inw(ioaddr + 6)>>14;
				for (i = 0; i < 3; i++)
					phys_addr[i] = htons(read_eeprom(ioaddr, i));

				mca_adaptor_select_mode(0);
				goto found;
			}
		mca_adaptor_select_mode(0);

	}
#endif	  

	/* Send the ID sequence to the ID_PORT. */
	outb(0x00, ID_PORT);
	outb(0x00, ID_PORT);
	for(i = 0; i < 255; i++) {
		outb(lrs_state, ID_PORT);
		lrs_state <<= 1;
		lrs_state = lrs_state & 0x100 ? lrs_state ^ 0xcf : lrs_state;
	}

	/* For the first probe, clear all board's tag registers. */
	if (current_tag == 0)
		outb(0xd0, ID_PORT);
	else				/* Otherwise kill off already-found boards. */
		outb(0xd8, ID_PORT);

	if (id_read_eeprom(7) != 0x6d50) {
		return -ENODEV;
	}

	/* Read in EEPROM data, which does contention-select.
	   Only the lowest address board will stay "on-line".
	   3Com got the byte order backwards. */
	for (i = 0; i < 3; i++) {
		phys_addr[i] = htons(id_read_eeprom(i));
	}

	{
		unsigned short iobase = id_read_eeprom(8);
		if_port = iobase >> 14;
		ioaddr = 0x200 + ((iobase & 0x1f) << 4);
	}
	irq = id_read_eeprom(9) >> 12;

	/* The current Space.c structure makes it difficult to have more
	   than one adaptor initialized.  Send me email if you have a need for
	   multiple adaptors, and we'll work out something.	 -becker@super.org */
	if (dev->base_addr != 0
		&&	dev->base_addr != (unsigned short)ioaddr) {
		return -ENODEV;
	}

	/* Set the adaptor tag so that the next card can be found. */
	outb(0xd0 + ++current_tag, ID_PORT);

	/* Activate the adaptor at the EEPROM location. */
	outb(0xff, ID_PORT);

	EL3WINDOW(0);
	if (inw(ioaddr) != 0x6d50)
		return -ENODEV;

 found:
	dev->base_addr = ioaddr;
	dev->irq = irq;
	dev->if_port = if_port;
	snarf_region(dev->base_addr, 16);

	{
		char *if_names[] = {"10baseT", "AUI", "undefined", "BNC"};
		printk("%s: 3c509 at %#3.3x tag %d, %s port, address ",
			   dev->name, dev->base_addr, current_tag, if_names[dev->if_port]);
	}

	/* Read in the station address. */
	for (i = 0; i < 6; i++)
		printk(" %2.2x", dev->dev_addr[i]);
	printk(", IRQ %d.\n", dev->irq);

	/* Make up a EL3-specific-data structure. */
	dev->priv = kmalloc(sizeof(struct el3_private), GFP_KERNEL);
	memset(dev->priv, 0, sizeof(struct el3_private));

	if (el3_debug > 0)
		printk(version);

	/* The EL3-specific entries in the device structure. */
	dev->open = &el3_open;
	dev->hard_start_xmit = &el3_start_xmit;
	dev->stop = &el3_close;
	dev->get_stats = &el3_get_stats;
#ifdef HAVE_MULTICAST
		dev->set_multicast_list = &set_multicast_list;
#endif

	/* Fill in the generic fields of the device structure. */
	for (i = 0; i < DEV_NUMBUFFS; i++)
		dev->buffs[i] = NULL;

	dev->hard_header	= eth_header;
	dev->add_arp		= eth_add_arp;
	dev->queue_xmit		= dev_queue_xmit;
	dev->rebuild_header = eth_rebuild_header;
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

/* Read a word from the EEPROM using the regular EEPROM access register.
   Assume that we are in register window zero.
 */
static ushort read_eeprom(short ioaddr, int index)
{
	int timer;

	outw(EEPROM_READ + index, ioaddr + 10);
	/* Pause for at least 162 us. for the read to take place. */
	for (timer = 0; timer < 162*4 + 400; timer++)
		SLOW_DOWN_IO;
	return inw(ioaddr + 12);
}

/* Read a word from the EEPROM when in the ISA ID probe state. */
static ushort id_read_eeprom(int index)
{
	int timer, bit, word = 0;
	
	/* Issue read command, and pause for at least 162 us. for it to complete.
	   Assume extra-fast 16Mhz bus. */
	outb(EEPROM_READ + index, ID_PORT);

	/* This should really be done by looking at one of the timer channels. */
	for (timer = 0; timer < 162*4 + 400; timer++)
		SLOW_DOWN_IO;

	for (bit = 15; bit >= 0; bit--)
		word = (word << 1) + (inb(ID_PORT) & 0x01);
		
	if (el3_debug > 3)
		printk("  3c509 EEPROM word %d %#4.4x.\n", index, word);

	return word;
}



static int
el3_open(struct device *dev)
{
	int ioaddr = dev->base_addr;
	int i;

	if (request_irq(dev->irq, &el3_interrupt)) {
		return -EAGAIN;
	}

	EL3WINDOW(0);
	if (el3_debug > 3)
		printk("%s: Opening, IRQ %d	 status@%x %4.4x.\n", dev->name,
			   dev->irq, ioaddr + EL3_STATUS, inw(ioaddr + EL3_STATUS));

	/* Activate board: this is probably unnecessary. */
	outw(0x0001, ioaddr + 4);

	irq2dev_map[dev->irq] = dev;

	/* Set the IRQ line. */
	outw((dev->irq << 12) | 0x0f00, ioaddr + 8);

	/* Set the station address in window 2 each time opened. */
	EL3WINDOW(2);

	for (i = 0; i < 6; i++)
		outb(dev->dev_addr[i], ioaddr + i);

	if (dev->if_port == 3)
		/* Start the thinnet transceiver. We should really wait 50ms...*/
		outw(0x1000, ioaddr + EL3_CMD);
	else if (dev->if_port == 0) {
		/* 10baseT interface, enabled link beat and jabber check. */
		EL3WINDOW(4);
		outw(inw(ioaddr + WN4_MEDIA) | MEDIA_TP, ioaddr + WN4_MEDIA);
	}

	/* Switch to register set 1 for normal use. */
	EL3WINDOW(1);

	outw(0x8005, ioaddr + EL3_CMD); /* Accept b-case and phys addr only. */
	outw(0xA800, ioaddr + EL3_CMD); /* Turn on statistics. */
	outw(0x2000, ioaddr + EL3_CMD); /* Enable the receiver. */
	outw(0x4800, ioaddr + EL3_CMD); /* Enable transmitter. */
	outw(0x78ff, ioaddr + EL3_CMD); /* Allow all status bits to be seen. */
	dev->interrupt = 0;
	dev->tbusy = 0;
	dev->start = 1;
	outw(0x7098, ioaddr + EL3_CMD); /* Set interrupt mask. */

	if (el3_debug > 3)
		printk("%s: Opened 3c509  IRQ %d  status %4.4x.\n",
			   dev->name, dev->irq, inw(ioaddr + EL3_STATUS));

	return 0;					/* Always succeed */
}

static int
el3_start_xmit(struct sk_buff *skb, struct device *dev)
{
	struct el3_private *lp = (struct el3_private *)dev->priv;
	int ioaddr = dev->base_addr;

	/* Transmitter timeout, serious problems. */
	if (dev->tbusy) {
		int tickssofar = jiffies - dev->trans_start;
		if (tickssofar < 10)
			return 1;
		printk("%s: transmit timed out, tx_status %2.2x status %4.4x.\n",
			   dev->name, inb(ioaddr + TX_STATUS), inw(ioaddr + EL3_STATUS));
		dev->trans_start = jiffies;
		/* Issue TX_RESET and TX_START commands. */
		outw(0x5800, ioaddr + EL3_CMD); /* TX_RESET */
		outw(0x4800, ioaddr + EL3_CMD); /* TX_START */
		dev->tbusy = 0;
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

	if (el3_debug > 4) {
		printk("%s: el3_start_xmit(lenght = %ld) called, status %4.4x.\n",
			   dev->name, skb->len, inw(ioaddr + EL3_STATUS));
	}
#ifndef final_version
	{	/* Error-checking code, delete for 1.00. */
		ushort status = inw(ioaddr + EL3_STATUS);
		if (status & 0x0001 		/* IRQ line active, missed one. */
			&& inw(ioaddr + EL3_STATUS) & 1) { 			/* Make sure. */
			printk("%s: Missed interrupt, status then %04x now %04x"
				   "  Tx %2.2x Rx %4.4x.\n", dev->name, status,
				   inw(ioaddr + EL3_STATUS), inb(ioaddr + TX_STATUS),
				   inw(ioaddr + RX_STATUS));
			outw(0x7800, ioaddr + EL3_CMD); /* Fake interrupt trigger. */
			outw(0x6899, ioaddr + EL3_CMD); /* Ack IRQ */
			outw(0x78ff, ioaddr + EL3_CMD); /* Set all status bits visible. */
		}
	}
#endif

	/* Avoid timer-based retransmission conflicts. */
	if (set_bit(0, (void*)&dev->tbusy) != 0)
		printk("%s: Transmitter access conflict.\n", dev->name);
	else {
		/* Put out the doubleword header... */
		outw(skb->len, ioaddr + TX_FIFO);
		outw(0x00, ioaddr + TX_FIFO);
		/* ... and the packet rounded to a doubleword. */
		outsl(ioaddr + TX_FIFO, skb->data, (skb->len + 3) >> 2);
	
		dev->trans_start = jiffies;
		if (inw(ioaddr + TX_FREE) > 1536) {
			dev->tbusy=0;
		} else
			/* Interrupt us when the FIFO has room for max-sized packet. */
			outw(0x9000 + 1536, ioaddr + EL3_CMD);
	}

	if (skb->free)
		kfree_skb (skb, FREE_WRITE);

	/* Clear the Tx status stack. */
	{
		short tx_status;
		int i = 4;

		while (--i > 0	&&	(tx_status = inb(ioaddr + TX_STATUS)) > 0) {
			if (el3_debug > 5)
				printk("		Tx status %4.4x.\n", tx_status);
			if (tx_status & 0x38) lp->stats.tx_aborted_errors++;
			if (tx_status & 0x30) outw(0x5800, ioaddr + EL3_CMD);
			if (tx_status & 0x3C) outw(0x4800, ioaddr + EL3_CMD);
			outb(0x00, ioaddr + TX_STATUS); /* Pop the status stack. */
		}
	}
	return 0;
}

/* The EL3 interrupt handler. */
static void
el3_interrupt(int reg_ptr)
{
	int irq = -(((struct pt_regs *)reg_ptr)->orig_eax+2);
	struct device *dev = (struct device *)(irq2dev_map[irq]);
	int ioaddr, status;
	int i = 0;

	if (dev == NULL) {
		printk ("el3_interrupt(): irq %d for unknown device.\n", irq);
		return;
	}

	if (dev->interrupt)
		printk("%s: Re-entering the interrupt handler.\n", dev->name);
	dev->interrupt = 1;

	ioaddr = dev->base_addr;
	status = inw(ioaddr + EL3_STATUS);

	if (el3_debug > 4)
		printk("%s: interrupt, status %4.4x.\n", dev->name, status);
	
	while ((status = inw(ioaddr + EL3_STATUS)) & 0x01) {

		if (status & 0x10)
			el3_rx(dev);

		if (status & 0x08) {
			if (el3_debug > 5)
				printk("	TX room bit was handled.\n");
			/* There's room in the FIFO for a full-sized packet. */
			outw(0x6808, ioaddr + EL3_CMD); /* Ack IRQ */
			dev->tbusy = 0;
			mark_bh(INET_BH);
		}
		if (status & 0x80)				/* Statistics full. */
			update_stats(ioaddr, dev);
		
		if (++i > 10) {
			printk("%s: Infinite loop in interrupt, status %4.4x.\n",
				   dev->name, status);
			/* Clear all interrupts we have handled. */
			outw(0x68FF, ioaddr + EL3_CMD);
			break;
		}
		/* Acknowledge the IRQ. */
		outw(0x6891, ioaddr + EL3_CMD); /* Ack IRQ */
	}

	if (el3_debug > 4) {
		printk("%s: exiting interrupt, status %4.4x.\n", dev->name,
			   inw(ioaddr + EL3_STATUS));
	}
	
	dev->interrupt = 0;
	return;
}


static struct enet_statistics *
el3_get_stats(struct device *dev)
{
	struct el3_private *lp = (struct el3_private *)dev->priv;

	sti();
	update_stats(dev->base_addr, dev);
	cli();
	return &lp->stats;
}

/*  Update statistics.  We change to register window 6, so this should be run
	single-threaded if the device is active. This is expected to be a rare
	operation, and it's simpler for the rest of the driver to assume that
	window 1 is always valid rather than use a special window-state variable.
	*/
static void update_stats(int ioaddr, struct device *dev)
{
	struct el3_private *lp = (struct el3_private *)dev->priv;

	if (el3_debug > 5)
		printk("   Updating the statistics.\n");
	/* Turn off statistics updates while reading. */
	outw(0xB000, ioaddr + EL3_CMD);
	/* Switch to the stats window, and read everything. */
	EL3WINDOW(6);
	lp->stats.tx_carrier_errors 	+= inb(ioaddr + 0);
	lp->stats.tx_heartbeat_errors	+= inb(ioaddr + 1);
	/* Multiple collisions. */	   	inb(ioaddr + 2);
	lp->stats.collisions			+= inb(ioaddr + 3);
	lp->stats.tx_window_errors		+= inb(ioaddr + 4);
	lp->stats.rx_fifo_errors		+= inb(ioaddr + 5);
	lp->stats.tx_packets			+= inb(ioaddr + 6);
	lp->stats.rx_packets			+= inb(ioaddr + 7);
	/* Tx deferrals */				inb(ioaddr + 8);
	inw(ioaddr + 10);	/* Total Rx and Tx octets. */
	inw(ioaddr + 12);

	/* Back to window 1, and turn statistics back on. */
	EL3WINDOW(1);
	outw(0xA800, ioaddr + EL3_CMD);
	return;
}

static int
el3_rx(struct device *dev)
{
	struct el3_private *lp = (struct el3_private *)dev->priv;
	int ioaddr = dev->base_addr;
	short rx_status;

	if (el3_debug > 5)
		printk("	   In rx_packet(), status %4.4x, rx_status %4.4x.\n",
			   inw(ioaddr+EL3_STATUS), inw(ioaddr+RX_STATUS));
	while ((rx_status = inw(ioaddr + RX_STATUS)) > 0) {
		if (rx_status & 0x4000) { /* Error, update stats. */
			short error = rx_status & 0x3800;
			lp->stats.rx_errors++;
			switch (error) {
			case 0x0000:		lp->stats.rx_over_errors++; break;
			case 0x0800:		lp->stats.rx_length_errors++; break;
			case 0x1000:		lp->stats.rx_frame_errors++; break;
			case 0x1800:		lp->stats.rx_length_errors++; break;
			case 0x2000:		lp->stats.rx_frame_errors++; break;
			case 0x2800:		lp->stats.rx_crc_errors++; break;
			}
		}
		if ( (! (rx_status & 0x4000))
			|| ! (rx_status & 0x1000)) { /* Dribble bits are OK. */
			short pkt_len = rx_status & 0x7ff;
			int sksize = sizeof(struct sk_buff) + pkt_len + 3;
			struct sk_buff *skb;

			skb = alloc_skb(sksize, GFP_ATOMIC);
			if (el3_debug > 4)
				printk("	   Receiving packet size %d status %4.4x.\n",
					   pkt_len, rx_status);
			if (skb != NULL) {
				skb->mem_len = sksize;
				skb->mem_addr = skb;
				skb->len = pkt_len;
				skb->dev = dev;

				/* 'skb->data' points to the start of sk_buff data area. */
				insl(ioaddr+RX_FIFO, skb->data,
							(pkt_len + 3) >> 2);

#ifdef HAVE_NETIF_RX
				netif_rx(skb);
				outw(0x4000, ioaddr + EL3_CMD); /* Rx discard */
				continue;
#else
				skb->lock = 0;
				if (dev_rint((unsigned char *)skb, pkt_len,
							 IN_SKBUFF,dev)== 0){
					if (el3_debug > 6)
						printk("	 dev_rint() happy, status %4.4x.\n",
						inb(ioaddr + EL3_STATUS));
					outw(0x4000, ioaddr + EL3_CMD); /* Rx discard */
					while (inw(ioaddr + EL3_STATUS) & 0x1000)
					  printk("	Waiting for 3c509 to discard packet, status %x.\n",
							 inw(ioaddr + EL3_STATUS) );
					if (el3_debug > 6)
						printk("	 discarded packet, status %4.4x.\n",
						inb(ioaddr + EL3_STATUS));
					continue;
				} else {
					printk("%s: receive buffers full.\n", dev->name);
					kfree_s(skb, sksize);
				}
#endif
			} else if (el3_debug)
				printk("%s: Couldn't allocate a sk_buff of size %d.\n",
					   dev->name, sksize);
		}
		lp->stats.rx_dropped++;
		outw(0x4000, ioaddr + EL3_CMD); /* Rx discard */
		while (inw(ioaddr + EL3_STATUS) & 0x1000)
		  printk("	Waiting for 3c509 to discard packet, status %x.\n",
				 inw(ioaddr + EL3_STATUS) );
	}

	if (el3_debug > 5)
		printk("	   Exiting rx_packet(), status %4.4x, rx_status %4.4x.\n",
			   inw(ioaddr+EL3_STATUS), inw(ioaddr+8));

	return 0;
}

#ifdef HAVE_MULTICAST
/* Set or clear the multicast filter for this adaptor.
   num_addrs == -1		Promiscuous mode, receive all packets
   num_addrs == 0		Normal mode, clear multicast list
   num_addrs > 0		Multicast mode, receive normal and MC packets, and do
						best-effort filtering.
 */
static void
set_multicast_list(struct device *dev, int num_addrs, void *addrs)
{
	short ioaddr = dev->base_addr;
	if (num_addrs > 0) {
		outw(0x8007, ioaddr + EL3_CMD);
	} else if (num_addrs < 0) {
		outw(0x8008, ioaddr + EL3_CMD);
	} else
		outw(0x8005, ioaddr + EL3_CMD);
}
#endif

static int
el3_close(struct device *dev)
{
	int ioaddr = dev->base_addr;

	if (el3_debug > 2)
		printk("%s: Shutting down ethercard.\n", dev->name);

	dev->tbusy = 1;
	dev->start = 0;

	/* Turn off statistics.	 We update lp->stats below. */
	outw(0xB000, ioaddr + EL3_CMD);

	/* Disable the receiver and transmitter. */
	outw(0x1800, ioaddr + EL3_CMD);
	outw(0x5000, ioaddr + EL3_CMD);

	if (dev->if_port == 3)
		/* Turn off thinnet power. */
		outw(0xb800, ioaddr + EL3_CMD);
	else if (dev->if_port == 0) {
		/* Disable link beat and jabber, if_port may change ere next open(). */
		EL3WINDOW(4);
		outw(inw(ioaddr + WN4_MEDIA) & ~MEDIA_TP, ioaddr + WN4_MEDIA);
	}

	free_irq(dev->irq);
	/* Switching back to window 0 disables the IRQ. */
	EL3WINDOW(0);
	/* But we explicitly zero the IRQ line select anyway. */
	outw(0x0f00, ioaddr + 8);


	irq2dev_map[dev->irq] = 0;

	update_stats(ioaddr, dev);
	return 0;
}

/*
 * Local variables:
 *  compile-command: "gcc -D__KERNEL__ -I/usr/src/linux/net/inet -Wall -Wstrict-prototypes -O6 -m486 -c 3c509.c"
 *  version-control: t
 *  kept-new-versions: 5
 *  tab-width: 4
 * End:
 */
