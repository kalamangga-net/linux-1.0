static char *version =
	"d_link.c: $Revision: 0.32 $,  Bjorn Ekwall (bj0rn@blox.se)\n";
/*
 *	d_link.c
 *
 *	Linux driver for the D-Link DE-600 Ethernet pocket adapter.
 *
 *	Portions (C) Copyright 1993 by Bjorn Ekwall
 *	The Author may be reached as bj0rn@blox.se
 *
 *	Based on adapter information gathered from DE600.ASM by D-Link Inc.,
 *	as included on disk C in the v.2.11 of PC/TCP from FTP Software.
 *	For DE600.asm:
 *		Portions (C) Copyright 1990 D-Link, Inc.
 *		Copyright, 1988-1992, Russell Nelson, Crynwr Software
 *
 *	Adapted to the sample network driver core for linux,
 *	written by: Donald Becker <becker@super.org>
 *	C/O Supercomputing Research Ctr., 17100 Science Dr., Bowie MD 20715
 *
 *	compile-command:
 *	"gcc -D__KERNEL__  -Wall -Wstrict-prototypes -O6 -fomit-frame-pointer \
 *	 -m486 -DD_LINK_IO=0x378 -DD_LINK_IRQ=7 -UD_LINK_DEBUG -S d_link.c
 *
 **************************************************************/
/*
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2, or (at your option)
 *	any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. 
 *
 **************************************************************/
/* Add another "; SLOW_DOWN_IO" here if your adapter won't work OK: */
#define D_LINK_SLOW_DOWN SLOW_DOWN_IO; SLOW_DOWN_IO; SLOW_DOWN_IO

 /*
 * If you still have trouble reading/writing to the adapter,
 * modify the following "#define": (see <asm/io.h> for more info)
#define REALLY_SLOW_IO
 */
#define SLOW_IO_BY_JUMPING /* Looks "better" than dummy write to port 0x80 :-) */

/*
 * For fix to TCP "slowdown", take a look at the "#define D_LINK_MAX_WINDOW"
 * near the end of the file...
 */

/* use 0 for production, 1 for verification, >2 for debug */
#ifdef D_LINK_DEBUG
#define PRINTK(x) if (d_link_debug >= 2) printk x
#else
#define D_LINK_DEBUG 0
#define PRINTK(x) /**/
#endif
static unsigned int d_link_debug = D_LINK_DEBUG;

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/string.h>
#include <linux/interrupt.h>
#include <asm/io.h>
#include <netinet/in.h>
#include <linux/ptrace.h>
#include <asm/system.h>
#include <errno.h>

#include "inet.h"
#include "dev.h"
#include "eth.h"
#include "ip.h"
#include "route.h"
#include "protocol.h"
#include "tcp.h"
#include "skbuff.h"
#include "sock.h"
#include "arp.h"

#define netstats enet_statistics

#ifndef HAVE_ALLOC_SKB
#define alloc_skb(size,pri)	(struct sk_buff *)kmalloc(size,pri)
#endif

/**************************************************
 *                                                *
 * Definition of D-Link Ethernet Pocket adapter   *
 *                                                *
 **************************************************/
/*
 * D-Link Ethernet pocket adapter ports
 */
/*
 * OK, so I'm cheating, but there are an awful lot of
 * reads and writes in order to get anything in and out
 * of the DE-600 with 4 bits at a time in the parallel port,
 * so every saved instruction really helps :-)
 *
 * That is, I don't care what the device struct says
 * but hope that Space.c will keep the rest of the drivers happy.
 */
#ifndef D_LINK_IO
#define D_LINK_IO 0x378
#endif

#define DATA_PORT	(D_LINK_IO)
#define STATUS_PORT	(D_LINK_IO + 1)
#define COMMAND_PORT	(D_LINK_IO + 2)

#ifndef D_LINK_IRQ
#define D_LINK_IRQ	7
#endif
/*
 * It really should look like this, and autoprobing as well...
 *
#define DATA_PORT	(dev->base_addr + 0)
#define STATUS_PORT	(dev->base_addr + 1)
#define COMMAND_PORT	(dev->base_addr + 2)
#define D_LINK_IRQ	dev->irq
 */

/*
 * D-Link COMMAND_PORT commands
 */
#define SELECT_NIC	0x04 /* select Network Interface Card */
#define SELECT_PRN	0x1c /* select Printer */
#define NML_PRN		0xec /* normal Printer situation */
#define IRQEN		0x10 /* enable IRQ line */

/*
 * D-Link STATUS_PORT
 */
#define RX_BUSY		0x80
#define RX_GOOD		0x40
#define TX_FAILED16	0x10
#define TX_BUSY		0x08

/*
 * D-Link DATA_PORT commands
 * command in low 4 bits
 * data in high 4 bits
 * select current data nibble with HI_NIBBLE bit
 */
#define WRITE_DATA	0x00 /* write memory */
#define READ_DATA	0x01 /* read memory */
#define STATUS		0x02 /* read  status register */
#define COMMAND		0x03 /* write command register (see COMMAND below) */
#define NULL_COMMAND	0x04 /* null command */
#define RX_LEN		0x05 /* read  received packet length */
#define TX_ADDR		0x06 /* set adapter transmit memory address */
#define RW_ADDR		0x07 /* set adapter read/write memory address */
#define HI_NIBBLE	0x08 /* read/write the high nibble of data,
				or-ed with rest of command */

/*
 * command register, accessed through DATA_PORT with low bits = COMMAND
 */
#define RX_ALL		0x01 /* PROMISCIOUS */
#define RX_BP		0x02 /* default: BROADCAST & PHYSICAL ADRESS */
#define RX_MBP		0x03 /* MULTICAST, BROADCAST & PHYSICAL ADRESS */

#define TX_ENABLE	0x04 /* bit 2 */
#define RX_ENABLE	0x08 /* bit 3 */

#define RESET		0x80 /* set bit 7 high */
#define STOP_RESET	0x00 /* set bit 7 low */

/*
 * data to command register
 * (high 4 bits in write to DATA_PORT)
 */
#define RX_PAGE2_SELECT	0x10 /* bit 4, only 2 pages to select */
#define RX_BASE_PAGE	0x20 /* bit 5, always set when specifying RX_ADDR */
#define FLIP_IRQ	0x40 /* bit 6 */

/*
 * D-Link adapter internal memory:
 *
 * 0-2K 1:st transmit page (send from pointer up to 2K)
 * 2-4K	2:nd transmit page (send from pointer up to 4K)
 *
 * 4-6K 1:st receive page (data from 4K upwards)
 * 6-8K 2:nd receive page (data from 6K upwards)
 *
 * 8K+	Adapter ROM (contains magic code and last 3 bytes of Ethernet address)
 */
#define MEM_2K		0x0800 /* 2048 */
#define MEM_4K		0x1000 /* 4096 */
#define MEM_6K		0x1800 /* 6144 */
#define NODE_ADDRESS	0x2000 /* 8192 */

#define RUNT 60		/* Too small Ethernet packet */

/**************************************************
 *                                                *
 *             End of definition                  *
 *                                                *
 **************************************************/

/*
 * Index to functions, as function prototypes.
 */

/* For tricking tcp.c to announce a small max window (max 2 fast packets please :-) */
static unsigned long	d_link_rspace(struct sock *sk);

/* Routines used internally. (See "convenience macros") */
static int		d_link_read_status(struct device *dev);
static unsigned	char	d_link_read_byte(unsigned char type, struct device *dev);

/* Put in the device structure. */
static int	d_link_open(struct device *dev);
static int	d_link_close(struct device *dev);
static struct netstats *get_stats(struct device *dev);
static int	d_link_start_xmit(struct sk_buff *skb, struct device *dev);

/* Dispatch from interrupts. */
static void	d_link_interrupt(int reg_ptr);
static int	d_link_tx_intr(struct device *dev, int irq_status);
static void	d_link_rx_intr(struct device *dev);

/* Initialization */
static void	trigger_interrupt(struct device *dev);
int		d_link_init(struct device *dev);
static void	adapter_init(struct device *dev);

/*
 * D-Link driver variables:
 */
extern struct device		*irq2dev_map[16];
static volatile int		rx_page		= 0;

#define TX_PAGES 2
static volatile int		tx_fifo[TX_PAGES];
static volatile int		tx_fifo_in = 0;
static volatile int		tx_fifo_out = 0;
static volatile int		free_tx_pages = TX_PAGES;

/*
 * Convenience macros/functions for D-Link adapter
 */

#define select_prn() outb_p(SELECT_PRN, COMMAND_PORT); D_LINK_SLOW_DOWN
#define select_nic() outb_p(SELECT_NIC, COMMAND_PORT); D_LINK_SLOW_DOWN

/* Thanks for hints from Mark Burton <markb@ordern.demon.co.uk> */
#define d_link_put_byte(data) ( \
	outb_p(((data) << 4)   | WRITE_DATA            , DATA_PORT), \
	outb_p(((data) & 0xf0) | WRITE_DATA | HI_NIBBLE, DATA_PORT))

/*
 * The first two outb_p()'s below could perhaps be deleted if there
 * would be more delay in the last two. Not certain about it yet...
 */
#define d_link_put_command(cmd) ( \
	outb_p(( rx_page        << 4)   | COMMAND            , DATA_PORT), \
	outb_p(( rx_page        & 0xf0) | COMMAND | HI_NIBBLE, DATA_PORT), \
	outb_p(((rx_page | cmd) << 4)   | COMMAND            , DATA_PORT), \
	outb_p(((rx_page | cmd) & 0xf0) | COMMAND | HI_NIBBLE, DATA_PORT))

#define d_link_setup_address(addr,type) ( \
	outb_p((((addr) << 4) & 0xf0) | type            , DATA_PORT), \
	outb_p(( (addr)       & 0xf0) | type | HI_NIBBLE, DATA_PORT), \
	outb_p((((addr) >> 4) & 0xf0) | type            , DATA_PORT), \
	outb_p((((addr) >> 8) & 0xf0) | type | HI_NIBBLE, DATA_PORT))

#define rx_page_adr() ((rx_page & RX_PAGE2_SELECT)?(MEM_6K):(MEM_4K))

/* Flip bit, only 2 pages */
#define next_rx_page() (rx_page ^= RX_PAGE2_SELECT)

#define tx_page_adr(a) (((a) + 1) * MEM_2K)

static inline int
d_link_read_status(struct device *dev)
{
	int	status;

	outb_p(STATUS, DATA_PORT);
	status = inb(STATUS_PORT);
	outb_p(NULL_COMMAND | HI_NIBBLE, DATA_PORT);

	return status;
}

static inline unsigned char
d_link_read_byte(unsigned char type, struct device *dev) { /* dev used by macros */
	unsigned char	lo;

	(void)outb_p((type), DATA_PORT);
	lo = ((unsigned char)inb(STATUS_PORT)) >> 4;
	(void)outb_p((type) | HI_NIBBLE, DATA_PORT);
	return ((unsigned char)inb(STATUS_PORT) & (unsigned char)0xf0) | lo;
}

/*
 * Open/initialize the board.  This is called (in the current kernel)
 * after booting when 'ifconfig <dev->name> $IP_ADDR' is run (in rc.inet1).
 *
 * This routine should set everything up anew at each open, even
 * registers that "should" only need to be set once at boot, so that
 * there is a non-reboot way to recover if something goes wrong.
 */
static int
d_link_open(struct device *dev)
{
	extern struct proto tcp_prot;

	if (request_irq(D_LINK_IRQ, d_link_interrupt)) {
		printk ("%s: unable to get IRQ %d\n", dev->name, D_LINK_IRQ);
		return 1;
	}
	irq2dev_map[D_LINK_IRQ] = dev;

	adapter_init(dev);

	/*
	 * Yes, I know!
	 * This is really not nice, but since a machine that uses DE-600
	 * rarely uses any other TCP/IP connection device simultaneously,
	 * this hack shouldn't really slow anything up.
	 * (I don't know about slip though... but it won't break it)
	 *
	 * This fix is better than changing in tcp.h IMHO
	 */
	tcp_prot.rspace = d_link_rspace; /* was: sock_rspace */

	return 0;
}

/*
 * The inverse routine to d_link_open().
 */
static int
d_link_close(struct device *dev)
{
	select_nic();
	rx_page = 0;
	d_link_put_command(RESET);
	d_link_put_command(STOP_RESET);
	d_link_put_command(0);
	select_prn();

	free_irq(D_LINK_IRQ);
	irq2dev_map[D_LINK_IRQ] = NULL;
	dev->start = 0;
	tcp_prot.rspace = sock_rspace; /* see comment above! */

	return 0;
}

static struct netstats *
get_stats(struct device *dev)
{
    return (struct netstats *)(dev->priv);
}

static inline void
trigger_interrupt(struct device *dev)
{
	d_link_put_command(FLIP_IRQ);
	select_prn();
	D_LINK_SLOW_DOWN;
	select_nic();
	d_link_put_command(0);
}

/*
 * Copy a buffer to the adapter transmit page memory.
 * Start sending.
 */
static int
d_link_start_xmit(struct sk_buff *skb, struct device *dev)
{
	int		transmit_from;
	int		len;
	int		tickssofar;
	unsigned char	*buffer = skb->data;

	/*
	 * If some higher layer thinks we've missed a
	 * tx-done interrupt we are passed NULL.
	 * Caution: dev_tint() handles the cli()/sti() itself.
	 */

	if (skb == NULL) {
		dev_tint(dev);
		return 0;
	}

	/* For ethernet, fill in the header (hardware addresses) with an arp. */
	if (!skb->arp)
		if(dev->rebuild_header(skb->data, dev)) {
			skb->dev = dev;
			arp_queue (skb);
			return 0;
		}
	skb->arp = 1;

	if (free_tx_pages <= 0) {	/* Do timeouts, to avoid hangs. */
		tickssofar = jiffies - dev->trans_start;

		if (tickssofar < 5)
			return 1;

		/* else */
		printk("%s: transmit timed out (%d), %s?\n",
			dev->name,
			tickssofar,
			"network cable problem"
			);
		/* Restart the adapter. */
		adapter_init(dev);
	}

	/* Start real output */
	PRINTK(("d_link_start_xmit:len=%d, page %d/%d\n", skb->len, tx_fifo_in, free_tx_pages));

	if ((len = skb->len) < RUNT)
		len = RUNT;

	cli();
	select_nic();

	tx_fifo[tx_fifo_in] = transmit_from = tx_page_adr(tx_fifo_in) - len;
	tx_fifo_in = (tx_fifo_in + 1) % TX_PAGES; /* Next free tx page */

	d_link_setup_address(transmit_from, RW_ADDR);
	for ( ; len > 0; --len, ++buffer)
		d_link_put_byte(*buffer);

	if (free_tx_pages-- == TX_PAGES) { /* No transmission going on */
		dev->trans_start = jiffies;
		dev->tbusy = 0;	/* allow more packets into adapter */
		/* Send page and generate an interrupt */
		d_link_setup_address(transmit_from, TX_ADDR);
		d_link_put_command(TX_ENABLE);
	}
	else {
		dev->tbusy = !free_tx_pages;
		select_prn();
	}
	
	sti(); /* interrupts back on */
	
	if (skb->free)
		kfree_skb (skb, FREE_WRITE);

	return 0;
}

/*
 * The typical workload of the driver:
 * Handle the network interface interrupts.
 */
static void
d_link_interrupt(int reg_ptr)
{
	int		irq = -(((struct pt_regs *)reg_ptr)->orig_eax+2);
	struct device	*dev = irq2dev_map[irq];
	unsigned char	irq_status;
	int		retrig = 0;
	int		boguscount = 0;

	/* This might just as well be deleted now, no crummy drivers present :-) */
	if ((dev == NULL) || (dev->start == 0) || (D_LINK_IRQ != irq)) {
		printk("%s: bogus interrupt %d\n", dev?dev->name:"DE-600", irq);
		return;
	}

	dev->interrupt = 1;
	select_nic();
	irq_status = d_link_read_status(dev);

	do {
		PRINTK(("d_link_interrupt (%2.2X)\n", irq_status));

		if (irq_status & RX_GOOD)
			d_link_rx_intr(dev);
		else if (!(irq_status & RX_BUSY))
			d_link_put_command(RX_ENABLE);

		/* Any transmission in progress? */
		if (free_tx_pages < TX_PAGES)
			retrig = d_link_tx_intr(dev, irq_status);
		else
			retrig = 0;

		irq_status = d_link_read_status(dev);
	} while ( (irq_status & RX_GOOD) || ((++boguscount < 10) && retrig) );
	/*
	 * Yeah, it _looks_ like busy waiting, smells like busy waiting
	 * and I know it's not PC, but please, it will only occur once
	 * in a while and then only for a loop or so (< 1ms for sure!)
	 */

	/* Enable adapter interrupts */
	dev->interrupt = 0;
	select_prn();

	if (retrig)
		trigger_interrupt(dev);

	sti();
	return;
}

static int
d_link_tx_intr(struct device *dev, int irq_status)
{
	/*
	 * Returns 1 if tx still not done
	 */

	mark_bh(INET_BH);
	/* Check if current transmission is done yet */
	if (irq_status & TX_BUSY)
		return 1; /* tx not done, try again */

	/* else */
	/* If last transmission OK then bump fifo index */
	if (!(irq_status & TX_FAILED16)) {
		tx_fifo_out = (tx_fifo_out + 1) % TX_PAGES;
		++free_tx_pages;
		((struct netstats *)(dev->priv))->tx_packets++;
		dev->tbusy = 0;
	}

	/* More to send, or resend last packet? */
	if ((free_tx_pages < TX_PAGES) || (irq_status & TX_FAILED16)) {
		dev->trans_start = jiffies;
		d_link_setup_address(tx_fifo[tx_fifo_out], TX_ADDR);
		d_link_put_command(TX_ENABLE);
		return 1;
	}
	/* else */

	return 0;
}

/*
 * We have a good packet, get it out of the adapter.
 */
static void
d_link_rx_intr(struct device *dev)
{
	struct sk_buff	*skb;
	int		i;
	int		read_from;
	int		size;
	int		sksize;
	register unsigned char	*buffer;

	cli();
	/* Get size of received packet */
	size = d_link_read_byte(RX_LEN, dev);	/* low byte */
	size += (d_link_read_byte(RX_LEN, dev) << 8);	/* high byte */
	size -= 4;	/* Ignore trailing 4 CRC-bytes */

	/* Tell adapter where to store next incoming packet, enable receiver */
	read_from = rx_page_adr();
	next_rx_page();
	d_link_put_command(RX_ENABLE);
	sti();

	if ((size < 32)  ||  (size > 1535))
		printk("%s: Bogus packet size %d.\n", dev->name, size);

	sksize = sizeof(struct sk_buff) + size;
	skb = alloc_skb(sksize, GFP_ATOMIC);
	sti();
	if (skb == NULL) {
		printk("%s: Couldn't allocate a sk_buff of size %d.\n",
			dev->name, sksize);
		return;
	}
	/* else */

	skb->lock = 0;
	skb->mem_len = sksize;
	skb->mem_addr = skb;
	/* 'skb->data' points to the start of sk_buff data area. */
	buffer = skb->data;

	/* copy the packet into the buffer */
	d_link_setup_address(read_from, RW_ADDR);
	for (i = size; i > 0; --i, ++buffer)
		*buffer = d_link_read_byte(READ_DATA, dev);
	
	((struct netstats *)(dev->priv))->rx_packets++; /* count all receives */

	if (dev_rint((unsigned char *)skb, size, IN_SKBUFF, dev))
		printk("%s: receive buffers full.\n", dev->name);
	/*
	 * If any worth-while packets have been received, dev_rint()
	 * has done a mark_bh(INET_BH) for us and will work on them
	 * when we get to the bottom-half routine.
	 */
}

int
d_link_init(struct device *dev)
{
	int	i;

	printk("%s: D-Link DE-600 pocket adapter", dev->name);
	/* Alpha testers must have the version number to report bugs. */
	if (d_link_debug > 1)
		printk(version);

	/* probe for adapter */
	rx_page = 0;
	select_nic();
	(void)d_link_read_status(dev);
	d_link_put_command(RESET);
	d_link_put_command(STOP_RESET);
	if (d_link_read_status(dev) & 0xf0) {
		printk(": not at I/O %#3x.\n", DATA_PORT);
		return ENODEV;
	}

	/*
	 * Maybe we found one,
	 * have to check if it is a D-Link DE-600 adapter...
	 */

	/* Get the adapter ethernet address from the ROM */
	d_link_setup_address(NODE_ADDRESS, RW_ADDR);
	for (i = 0; i < ETH_ALEN; i++) {
		dev->dev_addr[i] = d_link_read_byte(READ_DATA, dev);
		dev->broadcast[i] = 0xff;
	}

	/* Check magic code */
	if ((dev->dev_addr[1] == 0xde) && (dev->dev_addr[2] == 0x15)) {
		/* OK, install real address */
		dev->dev_addr[0] = 0x00;
		dev->dev_addr[1] = 0x80;
		dev->dev_addr[2] = 0xc8;
		dev->dev_addr[3] &= 0x0f;
		dev->dev_addr[3] |= 0x70;
	} else {
		printk(" not identified in the printer port\n");
		return ENODEV;
	}

	printk(", Ethernet Address: %2.2X", dev->dev_addr[0]);
	for (i = 1; i < ETH_ALEN; i++)
		printk(":%2.2X",dev->dev_addr[i]);
	printk("\n");

	/* Initialize the device structure. */
	dev->priv = kmalloc(sizeof(struct netstats), GFP_KERNEL);
	memset(dev->priv, 0, sizeof(struct netstats));

	for (i = 0; i < DEV_NUMBUFFS; i++)
		dev->buffs[i] = NULL;

	dev->get_stats = get_stats;
	dev->hard_header = eth_header;
	dev->add_arp = eth_add_arp;
	dev->queue_xmit = dev_queue_xmit;
	dev->rebuild_header = eth_rebuild_header;
	dev->type_trans = eth_type_trans;

	dev->open = d_link_open;
	dev->stop = d_link_close;
	dev->hard_start_xmit = &d_link_start_xmit;

	/* These are ethernet specific. */
	dev->type = ARPHRD_ETHER;
	dev->hard_header_len = ETH_HLEN;
	dev->mtu = 1500; /* eth_mtu */
	dev->addr_len	= ETH_ALEN;

	/* New-style flags. */
	dev->flags = IFF_BROADCAST;
	dev->family = AF_INET;
	dev->pa_addr = 0;
	dev->pa_brdaddr = 0;
	dev->pa_mask = 0;
	dev->pa_alen = sizeof(unsigned long);

	select_prn();
	return 0;
}

static void
adapter_init(struct device *dev)
{
	int	i;

	cli();
	dev->tbusy = 0;		/* Transmit busy...  */
	dev->interrupt = 0;
	dev->start = 1;

	select_nic();
	rx_page = 0; /* used by RESET */
	d_link_put_command(RESET);
	d_link_put_command(STOP_RESET);

	tx_fifo_in = 0;
	tx_fifo_out = 0;
	free_tx_pages = TX_PAGES;

	/* set the ether address. */
	d_link_setup_address(NODE_ADDRESS, RW_ADDR);
	for (i = 0; i < ETH_ALEN; i++)
		d_link_put_byte(dev->dev_addr[i]);

	/* where to start saving incoming packets */
	rx_page = RX_BP | RX_BASE_PAGE;
	d_link_setup_address(MEM_4K, RW_ADDR);
	/* Enable receiver */
	d_link_put_command(RX_ENABLE);
	select_prn();
	sti();
}

#define D_LINK_MIN_WINDOW 1024
#define D_LINK_MAX_WINDOW 2048
#define D_LINK_TCP_WINDOW_DIFF 1024
/*
 * Copied from sock.c
 *
 * Sets a lower max receive window in order to achieve <= 2
 * packets arriving at the adapter in fast succession.
 * (No way that a DE-600 can cope with an ethernet saturated with its packets :-)
 *
 * Since there are only 2 receive buffers in the DE-600
 * and it takes some time to copy from the adapter,
 * this is absolutely necessary for any TCP performance whatsoever!
 *
 */
#define min(a,b)	((a)<(b)?(a):(b))
static unsigned long
d_link_rspace(struct sock *sk)
{
  int amt;

  if (sk != NULL) {
/*
 * Hack! You might want to play with commenting away the following line,
 * if you know what you do!
 */
  	sk->max_unacked = D_LINK_MAX_WINDOW - D_LINK_TCP_WINDOW_DIFF;

	if (sk->rmem_alloc >= SK_RMEM_MAX-2*D_LINK_MIN_WINDOW) return(0);
	amt = min((SK_RMEM_MAX-sk->rmem_alloc)/2-D_LINK_MIN_WINDOW, D_LINK_MAX_WINDOW);
	if (amt < 0) return(0);
	return(amt);
  }
  return(0);
}
