/* eexpress.c: Intel EtherExpress device driver for Linux. */
/*
	Written 1993 by Donald Becker.
	Copyright 1993 United States Government as represented by the Director,
	National Security Agency.  This software may only be used and distributed
	according to the terms of the GNU Public License as modified by SRC,
	incorported herein by reference.

	The author may be reached as becker@super.org or
	C/O Supercomputing Research Ctr., 17100 Science Dr., Bowie MD 20715

	Things remaining to do:
	Check that the 586 and ASIC are reset/unreset at the right times.
	Check tx and rx buffer setup.
	The current Tx is single-buffer-only.
	Move the theory of operation and memory map documentation.
	Rework the board error reset
	The statistics need to be updated correctly.
*/

static char *version =
	"eexpress.c:v0.07 1/19/94 Donald Becker (becker@super.org)\n";

#include <linux/config.h>

/*
  Sources:
	This driver wouldn't have been written with the availability of the
	Crynwr driver source code.	It provided a known-working implementation
	that filled in the gaping holes of the Intel documention.  Three cheers
	for Russ Nelson.

	Intel Microcommunications Databook, Vol. 1, 1990. It provides just enough
	info that the casual reader might think that it documents the i82586.
*/

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>
#include <linux/ioport.h>
#include <linux/in.h>
#include <asm/system.h>
#include <asm/bitops.h>
#include <asm/io.h>
#include <asm/dma.h>
#include <errno.h>
#include <memory.h>

#include "dev.h"
#include "eth.h"
#include "skbuff.h"
#include "arp.h"

#ifndef HAVE_ALLOC_SKB
#define alloc_skb(size, priority) (struct sk_buff *) kmalloc(size,priority)
#else
/* This isn't quite right, but it's the best version define I can find right now. */
#include <linux/malloc.h>
#endif

/* use 0 for production, 1 for verification, 2..7 for debug */
#ifndef NET_DEBUG
#define NET_DEBUG 2
#endif
static unsigned int net_debug = NET_DEBUG;

/*
  			Details of the i82586.

   You'll really need the databook to understand the details of this part,
   but the outline is that the i82586 has two seperate processing units.

   The Rx unit uses a list of frame descriptors and a list of data buffer
   descriptors.  We use full-sized (1518 byte) data buffers, so there is
   a one-to-one pairing of frame descriptors to buffer descriptors.

   The Tx ("command") unit executes a list of commands that look like:
		Status word		Written by the 82586 when the command is done.
		Command word	Command in lower 3 bits, post-command action in upper 3
		Link word		The address of the next command.
		Parameters		(as needed).

	Some definitions related to the Command Word are:
 */
#define CMD_EOL		0x8000			/* The last command of the list, stop. */
#define CMD_SUSP	0x4000			/* Suspend after doing cmd. */
#define CMD_INTR	0x2000			/* Interrupt after doing cmd. */

enum commands {
	CmdNOp = 0, CmdSASetup = 1, CmdConfigure = 2, CmdMulticastList = 3,
	CmdTx = 4, CmdTDR = 5, CmdDump = 6, CmdDiagnose = 7};

/* Information that need to be kept for each board. */
struct net_local {
	struct enet_statistics stats;
	int last_restart;
	short rx_head;
	short rx_tail;
	short tx_head;
	short tx_cmd_link;
	short tx_reap;
};

/*
  		Details of the EtherExpress Implementation
  The EtherExpress takes an unusual approach to host access to packet buffer
  memory.  The host can use either the Dataport, with independent
  autoincrementing read and write pointers, or it can I/O map 32 bytes of the
  memory using the "Shadow Memory Pointer" (SMB) as follows:
			ioaddr						Normal EtherExpress registers
			ioaddr+0x4000...0x400f		Buffer Memory at SMB...SMB+15
			ioaddr+0x8000...0x800f		Buffer Memory at SMB+16...SMB+31
			ioaddr+0xC000...0xC007		"" SMB+16...SMB+23 (hardware flaw?)
			ioaddr+0xC008...0xC00f		Buffer Memory at 0x0008...0x000f
  The last I/O map set is useful if you put the i82586 System Command Block
  (the command mailbox) exactly at 0x0008.  (There seems to be some
  undocumented init structure at 0x0000-7, so I had to use the Crywnr memory
  setup verbatim for those four words anyway.)

  A problem with using either one of these mechanisms is that you must run
  single-threaded, or the interrupt handler must restore a changed value of
  the read, write, or SMB pointers.

  Unlike the Crynwr driver, my driver mostly ignores the I/O mapped "feature"
  and relies heavily on the dataport for buffer memory access.  To minimize
  switching, the read_pointer is dedicated to the Rx interrupt handler, and
  the write_pointer is used by the send_packet() routine (it's carefully saved
  and restored when it's needed by the interrupt handler).
  */

/* Offsets from the base I/O address. */
#define DATAPORT	0	/* Data Transfer Register. */
#define WRITE_PTR	2	/* Write Address Pointer. */
#define READ_PTR	4	/* Read Address Pointer. */
#define SIGNAL_CA	6	/* Frob the 82586 Channel Attention line. */
#define SET_IRQ		7	/* IRQ Select. */
#define SHADOW_PTR	8	/* Shadow Memory Bank Pointer. */
#define MEM_Ctrl	11
#define MEM_Page_Ctrl	12
#define Config		13
#define EEPROM_Ctrl		14
#define ID_PORT		15

/*	EEPROM_Ctrl bits. */

#define EE_SHIFT_CLK	0x01	/* EEPROM shift clock. */
#define EE_CS			0x02	/* EEPROM chip select. */
#define EE_DATA_WRITE	0x04	/* EEPROM chip data in. */
#define EE_DATA_READ	0x08	/* EEPROM chip data out. */
#define EE_CTRL_BITS	(EE_SHIFT_CLK | EE_CS | EE_DATA_WRITE | EE_DATA_READ)
#define ASIC_RESET		0x40
#define _586_RESET		0x80

/* Offsets to elements of the System Control Block structure. */
#define SCB_STATUS	0xc008
#define SCB_CMD		0xc00A
#define	 CUC_START	 0x0100
#define	 CUC_RESUME	 0x0200
#define	 CUC_SUSPEND 0x0300
#define	 RX_START	 0x0010
#define	 RX_RESUME	 0x0020
#define	 RX_SUSPEND	 0x0030
#define SCB_CBL		0xc00C	/* Command BLock offset. */
#define SCB_RFA		0xc00E	/* Rx Frame Area offset. */

/*
  What follows in 'init_words[]' is the "program" that is downloaded to the
  82586 memory.	 It's mostly tables and command blocks, and starts at the
  reset address 0xfffff6.

  Even with the additional "don't care" values, doing it this way takes less
  program space than initializing the individual tables, and I feel it's much
  cleaner.

  The databook is particularly useless for the first two structures; they are
  completely undocumented.  I had to use the Crynwr driver as an example.

   The memory setup is as follows:
   */

#define CONFIG_CMD	0x0018
#define SET_SA_CMD	0x0024
#define SA_OFFSET	0x002A
#define IDLELOOP	0x30
#define TDR_CMD		0x38
#define TDR_TIME	0x3C
#define DUMP_CMD	0x40
#define DIAG_CMD	0x48
#define SET_MC_CMD	0x4E
#define DUMP_DATA	0x56	/* A 170 byte buffer for dump and Set-MC into. */

#define TX_BUF_START	0x0100
#define NUM_TX_BUFS 	4
#define TX_BUF_SIZE		0x0680	/* packet+header+TBD+extra (1518+14+20+16) */
#define TX_BUF_END		0x2000

#define RX_BUF_START	0x2000
#define RX_BUF_SIZE 	(0x640)	/* packet+header+RBD+extra */
#define RX_BUF_END		0x4000

/*
  That's it: only 86 bytes to set up the beast, including every extra
  command available.  The 170 byte buffer at DUMP_DATA is shared between the
  Dump command (called only by the diagnostic program) and the SetMulticastList
  command.

  To complete the memory setup you only have to write the station address at
  SA_OFFSET and create the Tx & Rx buffer lists.

  The Tx command chain and buffer list is setup as follows:
  A Tx command table, with the data buffer pointing to...
  A Tx data buffer descriptor.  The packet is in a single buffer, rather than
     chaining together several smaller buffers.
  A NoOp command, which initially points to itself,
  And the packet data.

  A transmit is done by filling in the Tx command table and data buffer,
  re-writing the NoOp command, and finally changing the offset of the last
  command to point to the current Tx command.  When the Tx command is finished,
  it jumps to the NoOp, when it loops until the next Tx command changes the
  "link offset" in the NoOp.  This way the 82586 never has to go through the
  slow restart sequence.

  The Rx buffer list is set up in the obvious ring structure.  We have enough
  memory (and low enough interrupt latency) that we can avoid the complicated
  Rx buffer linked lists by alway associating a full-size Rx data buffer with
  each Rx data frame.

  I current use four transmit buffers starting at TX_BUF_START (0x0100), and
  use the rest of memory, from RX_BUF_START to RX_BUF_END, for Rx buffers.

  */

static short init_words[] = {
	0x0000,					/* Set bus size to 16 bits. */
	0x0000,0x0000,			/* Set control mailbox (SCB) addr. */
	0,0,					/* pad to 0x000000. */
	0x0001,					/* Status word that's cleared when init is done. */
	0x0008,0,0,				/* SCB offset, (skip, skip) */

	0,0xf000|RX_START|CUC_START,	/* SCB status and cmd. */
	CONFIG_CMD,				/* Command list pointer, points to Configure. */
	RX_BUF_START,				/* Rx block list. */
	0,0,0,0,				/* Error count: CRC, align, buffer, overrun. */

	/* 0x0018: Configure command.  Change to put MAC data with packet. */
	0, CmdConfigure,		/* Status, command.		*/
	SET_SA_CMD,				/* Next command is Set Station Addr. */
	0x0804,					/* "4" bytes of config data, 8 byte FIFO. */
	0x2e40,					/* Magic values, including MAC data location. */
	0,						/* Unused pad word. */

	/* 0x0024: Setup station address command. */
	0, CmdSASetup,
	SET_MC_CMD,				/* Next command. */
	0xaa00,0xb000,0x0bad,	/* Station address (to be filled in) */

	/* 0x0030: NOP, looping back to itself.	 Point to first Tx buffer to Tx. */
	0, CmdNOp, IDLELOOP, 0 /* pad */,

	/* 0x0038: A unused Time-Domain Reflectometer command. */
	0, CmdTDR, IDLELOOP, 0,

	/* 0x0040: An unused Dump State command. */
	0, CmdDump, IDLELOOP, DUMP_DATA,

	/* 0x0048: An unused Diagnose command. */
	0, CmdDiagnose, IDLELOOP,

	/* 0x004E: An empty set-multicast-list command. */
#ifdef initial_text_tx
	0, CmdMulticastList, DUMP_DATA, 0,
#else
	0, CmdMulticastList, IDLELOOP, 0,
#endif

	/* 0x0056: A continuous transmit command, only here for testing. */
	0, CmdTx, DUMP_DATA, DUMP_DATA+8, 0x83ff, -1, DUMP_DATA, 0,
};

/* Index to functions, as function prototypes. */

extern int express_probe(struct device *dev);	/* Called from Space.c */

static int	eexp_probe1(struct device *dev, short ioaddr);
static int	eexp_open(struct device *dev);
static int	eexp_send_packet(struct sk_buff *skb, struct device *dev);
static void	eexp_interrupt(int reg_ptr);
static void eexp_rx(struct device *dev);
static int	eexp_close(struct device *dev);
static struct enet_statistics *eexp_get_stats(struct device *dev);
#ifdef HAVE_MULTICAST
static void set_multicast_list(struct device *dev, int num_addrs, void *addrs);
#endif

static int read_eeprom(int ioaddr, int location);
static void hardware_send_packet(struct device *dev, void *buf, short length);
static void init_82586_mem(struct device *dev);
static void init_rx_bufs(struct device *dev);


/* Check for a network adaptor of this type, and return '0' iff one exists.
   If dev->base_addr == 0, probe all likely locations.
   If dev->base_addr == 1, always return failure.
   If dev->base_addr == 2, (detachable devices only) alloate space for the
   device and return success.
   */
int
express_probe(struct device *dev)
{
	/* Don't probe all settable addresses, 0x[23][0-7]0, just common ones. */
	int *port, ports[] = {0x300, 0x270, 0x320, 0x340, 0};
	int base_addr = dev->base_addr;

	if (base_addr > 0x1ff)	/* Check a single specified location. */
		return eexp_probe1(dev, base_addr);
	else if (base_addr > 0)
		return ENXIO;		/* Don't probe at all. */

	for (port = &ports[0]; *port; port++) {
		short id_addr = *port + ID_PORT;
		unsigned short sum = 0;
		int i;
#ifdef notdef
		for (i = 16; i > 0; i--)
			sum += inb(id_addr);
		printk("EtherExpress ID checksum is %04x.\n", sum);
#else
		for (i = 4; i > 0; i--) {
			short id_val = inb(id_addr);
			sum |= (id_val >> 4) << ((id_val & 3) << 2);
		}
#endif
		if (sum == 0xbaba
			&& eexp_probe1(dev, *port) == 0)
			return 0;
	}

	return ENODEV;			/* ENODEV would be more accurate. */
}

int eexp_probe1(struct device *dev, short ioaddr)
{
	unsigned short station_addr[3];
	int i;

	printk("%s: EtherExpress at %#x,", dev->name, ioaddr);

	/* The station address is stored !backwards! in the EEPROM, reverse
	   after reading.  (Hmmm, a little brain-damage there at Intel, eh?) */
	station_addr[0] = read_eeprom(ioaddr, 2);
	station_addr[1] = read_eeprom(ioaddr, 3);
	station_addr[2] = read_eeprom(ioaddr, 4);

	/* Check the first three octets of the S.A. for the manufactor's code. */
	if (station_addr[2] != 0x00aa || (station_addr[1] & 0xff00) != 0x0000) {
		printk(" rejected (invalid address %04x%04x%04x).\n",
			   station_addr[2], station_addr[1], station_addr[0]);
		return ENODEV;
	}

	/* We've committed to using the board, and can start filling in *dev. */
	snarf_region(ioaddr, 16);
	dev->base_addr = ioaddr;

	for (i = 0; i < 6; i++) {
		dev->dev_addr[i] = ((unsigned char*)station_addr)[5-i];
		printk(" %02x", dev->dev_addr[i]);
	}

	/* There is no reason for the driver to care, but I print out the
	   interface to minimize bogus bug reports. */
	{
		char irqmap[] = {0, 9, 3, 4, 5, 10, 11, 0};
		char *ifmap[] = {"AUI", "BNC", "10baseT"};
		enum iftype {AUI=0, BNC=1, TP=2};
		unsigned short setupval = read_eeprom(ioaddr, 0);

		dev->irq = irqmap[setupval >> 13];
		dev->if_port = (setupval & 0x1000) == 0 ? AUI :
			read_eeprom(ioaddr, 5) & 0x1 ? TP : BNC;
		printk(", IRQ %d, Interface %s.\n", dev->irq, ifmap[dev->if_port]);
		/* Release the IRQ line so that it can be shared if we don't use the
		   ethercard. */
		outb(0x00, ioaddr + SET_IRQ);
	}

	/* It's now OK to leave the board in reset, pending the open(). */
	outb(ASIC_RESET, ioaddr + EEPROM_Ctrl);

	if ((dev->mem_start & 0xf) > 0)
		net_debug = dev->mem_start & 7;

	if (net_debug)
		printk(version);

	/* Initialize the device structure. */
	dev->priv = kmalloc(sizeof(struct net_local), GFP_KERNEL);
	memset(dev->priv, 0, sizeof(struct net_local));

	dev->open		= eexp_open;
	dev->stop		= eexp_close;
	dev->hard_start_xmit = eexp_send_packet;
	dev->get_stats	= eexp_get_stats;
#ifdef HAVE_MULTICAST
	dev->set_multicast_list = &set_multicast_list;
#endif

	/* Fill in the fields of the device structure with ethernet-generic values.
	   This should be in a common file instead of per-driver.  */
	for (i = 0; i < DEV_NUMBUFFS; i++)
		dev->buffs[i] = NULL;

	dev->hard_header	= eth_header;
	dev->add_arp	= eth_add_arp;
	dev->queue_xmit = dev_queue_xmit;
	dev->rebuild_header = eth_rebuild_header;
	dev->type_trans = eth_type_trans;

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
	dev->pa_brdaddr = 0;
	dev->pa_mask	= 0;
	dev->pa_alen	= sizeof(unsigned long);

	return 0;
}


/* Reverse IRQ map: the value to put in the SET_IRQ reg. for IRQ<index>. */
static char irqrmap[]={0,0,1,2,3,4,0,0,0,1,5,6,0,0,0,0};

static int
eexp_open(struct device *dev)
{
	int ioaddr = dev->base_addr;

	if (dev->irq == 0  ||  irqrmap[dev->irq] == 0)
		return -ENXIO;

	if (irq2dev_map[dev->irq] != 0
		/* This is always true, but avoid the false IRQ. */
		|| (irq2dev_map[dev->irq] = dev) == 0
		|| request_irq(dev->irq, &eexp_interrupt)) {
		return -EAGAIN;
	}

	/* Initialize the 82586 memory and start it. */
	init_82586_mem(dev);

	/* Enable the interrupt line. */
	outb(irqrmap[dev->irq] | 0x08, ioaddr + SET_IRQ);

	dev->tbusy = 0;
	dev->interrupt = 0;
	dev->start = 1;
	return 0;
}

static int
eexp_send_packet(struct sk_buff *skb, struct device *dev)
{
	struct net_local *lp = (struct net_local *)dev->priv;
	int ioaddr = dev->base_addr;

	if (dev->tbusy) {
		/* If we get here, some higher level has decided we are broken.
		   There should really be a "kick me" function call instead. */
		int tickssofar = jiffies - dev->trans_start;
		if (tickssofar < 5)
			return 1;
		if (net_debug > 1)
			printk("%s: transmit timed out, %s?  ", dev->name,
				   inw(ioaddr+SCB_STATUS) & 0x8000 ? "IRQ conflict" :
				   "network cable problem");
		lp->stats.tx_errors++;
		/* Try to restart the adaptor. */
		if (lp->last_restart == lp->stats.tx_packets) {
			if (net_debug > 1) printk("Resetting board.\n");
			/* Completely reset the adaptor. */
			init_82586_mem(dev);
		} else {
			/* Issue the channel attention signal and hope it "gets better". */
			if (net_debug > 1) printk("Kicking board.\n");
			outw(0xf000|CUC_START|RX_START, ioaddr + SCB_CMD);
			outb(0, ioaddr + SIGNAL_CA);
			lp->last_restart = lp->stats.tx_packets;
		}
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

	/* Block a timer-based transmit from overlapping. */
	if (set_bit(0, (void*)&dev->tbusy) != 0)
		printk("%s: Transmitter access conflict.\n", dev->name);
	else {
		short length = ETH_ZLEN < skb->len ? skb->len : ETH_ZLEN;
		unsigned char *buf = skb->data;

		/* Disable the 82586's input to the interrupt line. */
		outb(irqrmap[dev->irq], ioaddr + SET_IRQ);
		hardware_send_packet(dev, buf, length);
		dev->trans_start = jiffies;
		/* Enable the 82586 interrupt input. */
		outb(0x08 | irqrmap[dev->irq], ioaddr + SET_IRQ);
	}

	if (skb->free)
		kfree_skb (skb, FREE_WRITE);

	/* You might need to clean up and record Tx statistics here. */
	lp->stats.tx_aborted_errors++;

	return 0;
}

/*	The typical workload of the driver:
	Handle the network interface interrupts. */
static void
eexp_interrupt(int reg_ptr)
{
	int irq = -(((struct pt_regs *)reg_ptr)->orig_eax+2);
	struct device *dev = (struct device *)(irq2dev_map[irq]);
	struct net_local *lp;
	int ioaddr, status, boguscount = 0;
	short ack_cmd;

	if (dev == NULL) {
		printk ("net_interrupt(): irq %d for unknown device.\n", irq);
		return;
	}
	dev->interrupt = 1;
	
	ioaddr = dev->base_addr;
	lp = (struct net_local *)dev->priv;
	
	status = inw(ioaddr + SCB_STATUS);
	
    if (net_debug > 4) {
		printk("%s: EExp interrupt, status %4.4x.\n", dev->name, status);
    }

	/* Disable the 82586's input to the interrupt line. */
	outb(irqrmap[dev->irq], ioaddr + SET_IRQ);

	/* Reap the Tx packet buffers. */
	while (lp->tx_reap != lp->tx_head) { 	/* if (status & 0x8000) */
		unsigned short tx_status;
		outw(lp->tx_reap, ioaddr + READ_PTR);
		tx_status = inw(ioaddr);
		if (tx_status == 0) {
			if (net_debug > 5)  printk("Couldn't reap %#x.\n", lp->tx_reap);
			break;
		}
		if (tx_status & 0x2000) {
			lp->stats.tx_packets++;
			lp->stats.collisions += tx_status & 0xf;
			dev->tbusy = 0;
			mark_bh(INET_BH);	/* Inform upper layers. */
		} else {
			lp->stats.tx_errors++;
			if (tx_status & 0x0600)  lp->stats.tx_carrier_errors++;
			if (tx_status & 0x0100)  lp->stats.tx_fifo_errors++;
			if (!(tx_status & 0x0040))  lp->stats.tx_heartbeat_errors++;
			if (tx_status & 0x0020)  lp->stats.tx_aborted_errors++;
		}
		if (net_debug > 5)
			printk("Reaped %x, Tx status %04x.\n" , lp->tx_reap, tx_status);
		lp->tx_reap += TX_BUF_SIZE;
		if (lp->tx_reap > TX_BUF_END - TX_BUF_SIZE)
			lp->tx_reap = TX_BUF_START;
		if (++boguscount > 4)
			break;
	}

	if (status & 0x4000) { /* Packet received. */
		if (net_debug > 5)
			printk("Received packet, rx_head %04x.\n", lp->rx_head);
		eexp_rx(dev);
	}

	/* Acknowledge the interrupt sources. */
	ack_cmd = status & 0xf000;

	if ((status & 0x0700) != 0x0200  &&  dev->start) {
		short saved_write_ptr = inw(ioaddr + WRITE_PTR);
		if (net_debug > 1)
			printk("%s: Command unit stopped, status %04x, restarting.\n",
				   dev->name, status);
		/* If this ever occurs we must re-write the idle loop, reset
		   the Tx list, and do a complete restart of the command unit. */
		outw(IDLELOOP, ioaddr + WRITE_PTR);
		outw(0, ioaddr);
		outw(CmdNOp, ioaddr);
		outw(IDLELOOP, ioaddr);
		outw(IDLELOOP, SCB_CBL);
		lp->tx_cmd_link = IDLELOOP + 4;
		lp->tx_head = lp->tx_reap = TX_BUF_START;
		/* Restore the saved write pointer. */
		outw(saved_write_ptr, ioaddr + WRITE_PTR);
		ack_cmd |= CUC_START;
	}

	if ((status & 0x0070) != 0x0040  &&  dev->start) {
		short saved_write_ptr = inw(ioaddr + WRITE_PTR);
		/* The Rx unit is not ready, it must be hung.  Restart the receiver by
		   initializing the rx buffers, and issuing an Rx start command. */
		lp->stats.rx_errors++;
		if (net_debug > 1) {
			int cur_rxbuf = RX_BUF_START;
			printk("%s: Rx unit stopped status %04x rx head %04x tail %04x.\n",
				   dev->name, status, lp->rx_head, lp->rx_tail);
			while (cur_rxbuf <= RX_BUF_END - RX_BUF_SIZE) {
				int i;
				printk("  Rx buf at %04x:", cur_rxbuf);
				outw(cur_rxbuf, ioaddr + READ_PTR);
				for (i = 0; i < 0x20; i += 2)
					printk(" %04x", inw(ioaddr));
				printk(".\n");
				cur_rxbuf += RX_BUF_SIZE;
			}
		}
		init_rx_bufs(dev);
		outw(RX_BUF_START, SCB_RFA);
		outw(saved_write_ptr, ioaddr + WRITE_PTR);
		ack_cmd |= RX_START;
	}

	outw(ack_cmd, ioaddr + SCB_CMD);
	outb(0, ioaddr + SIGNAL_CA);

    if (net_debug > 5) {
		printk("%s: EExp exiting interrupt, status %4.4x.\n", dev->name,
			   inw(ioaddr + SCB_CMD));
    }
	/* Enable the 82586's input to the interrupt line. */
	outb(irqrmap[dev->irq] | 0x08, ioaddr + SET_IRQ);
	return;
}

static int
eexp_close(struct device *dev)
{
	int ioaddr = dev->base_addr;

	dev->tbusy = 1;
	dev->start = 0;

	/* Flush the Tx and disable Rx. */
	outw(RX_SUSPEND | CUC_SUSPEND, ioaddr + SCB_CMD);
	outb(0, ioaddr + SIGNAL_CA);

	/* Disable the physical interrupt line. */
	outb(0, ioaddr + SET_IRQ);

	free_irq(dev->irq);

	irq2dev_map[dev->irq] = 0;

	/* Update the statistics here. */

	return 0;
}

/* Get the current statistics.	This may be called with the card open or
   closed. */
static struct enet_statistics *
eexp_get_stats(struct device *dev)
{
	struct net_local *lp = (struct net_local *)dev->priv;

	/* ToDo: decide if there are any useful statistics from the SCB. */

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
	if (num_addrs < 0) {
		/* Not written yet, this requires expanding the init_words config
		   cmd. */
	} else if (num_addrs > 0) {
		/* Fill in the SET_MC_CMD with the number of address bytes, followed
		   by the list of multicast addresses to be accepted. */
		outw(SET_MC_CMD + 6, ioaddr + WRITE_PTR);
		outw(num_addrs * 6, ioaddr);
		outsw(ioaddr, addrs, num_addrs*3);		/* 3 = addr len in words */
		/* We must trigger a whole 586 reset due to a bug. */
	} else {
		/* Not written yet, this requires expanding the init_words config
		   cmd. */
		outw(99, ioaddr);		/* Disable promiscuous mode, use normal mode */
	}
}
#endif

/* The horrible routine to read a word from the serial EEPROM. */

/* The delay between EEPROM clock transitions. */
#define eeprom_delay()	{ int _i = 40; while (--_i > 0) { __SLOW_DOWN_IO; }}
#define EE_READ_CMD (6 << 6)

int
read_eeprom(int ioaddr, int location)
{
	int i;
	unsigned short retval = 0;
	short ee_addr = ioaddr + EEPROM_Ctrl;
	int read_cmd = location | EE_READ_CMD;
	short ctrl_val = EE_CS | _586_RESET;
	
	outb(ctrl_val, ee_addr);
	
	/* Shift the read command bits out. */
	for (i = 8; i >= 0; i--) {
		short outval = (read_cmd & (1 << i)) ? ctrl_val | EE_DATA_WRITE
			: ctrl_val;
		outb(outval, ee_addr);
		outb(outval | EE_SHIFT_CLK, ee_addr);	/* EEPROM clock tick. */
		eeprom_delay();
		outb(outval, ee_addr);	/* Finish EEPROM a clock tick. */
		eeprom_delay();
	}
	outb(ctrl_val, ee_addr);
	
	for (i = 16; i > 0; i--) {
		outb(ctrl_val | EE_SHIFT_CLK, ee_addr);	 eeprom_delay();
		retval = (retval << 1) | ((inb(ee_addr) & EE_DATA_READ) ? 1 : 0);
		outb(ctrl_val, ee_addr);  eeprom_delay();
	}

	/* Terminate the EEPROM access. */
	ctrl_val &= ~EE_CS;
	outb(ctrl_val | EE_SHIFT_CLK, ee_addr);
	eeprom_delay();
	outb(ctrl_val, ee_addr);
	eeprom_delay();
	return retval;
}

static void
init_82586_mem(struct device *dev)
{
	struct net_local *lp = (struct net_local *)dev->priv;
	short ioaddr = dev->base_addr;

	/* Enable loopback to protect the wire while starting up.
	   This is Superstition From Crynwr. */
	outb(inb(ioaddr + Config) | 0x02, ioaddr + Config);

	/* Hold the 586 in reset during the memory initialization. */
	outb(_586_RESET, ioaddr + EEPROM_Ctrl);

	/* Place the write pointer at 0xfff6 (address-aliased to 0xfffff6). */
	outw(0xfff6, ioaddr + WRITE_PTR);
	outsw(ioaddr, init_words, sizeof(init_words)>>1);

	/* Fill in the station address. */
	outw(SA_OFFSET, ioaddr + WRITE_PTR);
	outsw(ioaddr, dev->dev_addr, 3);

	/* The Tx-block list is written as needed.  We just set up the values. */
#ifdef initial_text_tx
	lp->tx_cmd_link = DUMP_DATA + 4;
#else
	lp->tx_cmd_link = IDLELOOP + 4;
#endif
	lp->tx_head = lp->tx_reap = TX_BUF_START;

	init_rx_bufs(dev);

	/* Start the 586 by releasing the reset line. */
	outb(0x00, ioaddr + EEPROM_Ctrl);

	/* This was time consuming to track down: you need to give two channel
	   attention signals to reliably start up the i82586. */
	outb(0, ioaddr + SIGNAL_CA);

	{
		int boguscnt = 50;
		while (inw(ioaddr + SCB_STATUS) == 0)
			if (--boguscnt == 0) {
				printk("%s: i82586 initialization timed out with status %04x, cmd %04x.\n",
					   dev->name, inw(ioaddr + SCB_STATUS), inw(ioaddr + SCB_CMD));
				break;
			}
		/* Issue channel-attn -- the 82586 won't start without it. */
		outb(0, ioaddr + SIGNAL_CA);
	}

	/* Disable loopback. */
	outb(inb(ioaddr + Config) & ~0x02, ioaddr + Config);
	if (net_debug > 4)
		printk("%s: Initialized 82586, status %04x.\n", dev->name,
			   inw(ioaddr + SCB_STATUS));
	return;
}

/* Initialize the Rx-block list. */
static void init_rx_bufs(struct device *dev)
{
	struct net_local *lp = (struct net_local *)dev->priv;
	short ioaddr = dev->base_addr;

	int cur_rxbuf = lp->rx_head = RX_BUF_START;
	
	/* Initialize each Rx frame + data buffer. */
	do {	/* While there is room for one more. */
		outw(cur_rxbuf, ioaddr + WRITE_PTR);
		outw(0x0000, ioaddr); 				/* Status */
		outw(0x0000, ioaddr);				/* Command */
		outw(cur_rxbuf + RX_BUF_SIZE, ioaddr); /* Link */
		outw(cur_rxbuf + 22, ioaddr);		/* Buffer offset */
		outw(0xFeed, ioaddr); 				/* Pad for dest addr. */
		outw(0xF00d, ioaddr);
		outw(0xF001, ioaddr);
		outw(0x0505, ioaddr); 				/* Pad for source addr. */
		outw(0x2424, ioaddr);
		outw(0x6565, ioaddr);
		outw(0xdeaf, ioaddr);				/* Pad for protocol. */

		outw(0x0000, ioaddr);				/* Buffer: Actual count */
		outw(-1, ioaddr);					/* Buffer: Next (none). */
		outw(cur_rxbuf + 0x20, ioaddr);		/* Buffer: Address low */
		outw(0x0000, ioaddr);
		/* Finally, the number of bytes in the buffer. */
		outw(0x8000 + RX_BUF_SIZE-0x20, ioaddr);
		
		lp->rx_tail = cur_rxbuf;
		cur_rxbuf += RX_BUF_SIZE;
	} while (cur_rxbuf <= RX_BUF_END - RX_BUF_SIZE);
	
	/* Terminate the list by setting the EOL bit, and wrap the pointer to make
	   the list a ring. */
	outw(lp->rx_tail + 2, ioaddr + WRITE_PTR);
	outw(0xC000, ioaddr);					/* Command, mark as last. */
	outw(lp->rx_head, ioaddr);				/* Link */
}

static void
hardware_send_packet(struct device *dev, void *buf, short length)
{
	struct net_local *lp = (struct net_local *)dev->priv;
	short ioaddr = dev->base_addr;
	short tx_block = lp->tx_head;

	/* Set the write pointer to the Tx block, and put out the header. */
	outw(tx_block, ioaddr + WRITE_PTR);
	outw(0x0000, ioaddr);		/* Tx status */
	outw(CMD_INTR|CmdTx, ioaddr);		/* Tx command */
	outw(tx_block+16, ioaddr);	/* Next command is a NoOp. */
	outw(tx_block+8, ioaddr);	/* Data Buffer offset. */

	/* Output the data buffer descriptor. */
	outw(length | 0x8000, ioaddr); /* Byte count parameter. */
	outw(-1, ioaddr);			/* No next data buffer. */
	outw(tx_block+22, ioaddr);	/* Buffer follows the NoOp command. */
	outw(0x0000, ioaddr);		/* Buffer address high bits (always zero). */

	/* Output the Loop-back NoOp command. */
	outw(0x0000, ioaddr);		/* Tx status */
	outw(CmdNOp, ioaddr);		/* Tx command */
	outw(tx_block+16, ioaddr);	/* Next is myself. */

	/* Output the packet using the write pointer.
	   Hmmm, it feels a little like a 3c501! */
	outsw(ioaddr + DATAPORT, buf, (length + 1) >> 1);

	/* Set the old command link pointing to this send packet. */
	outw(lp->tx_cmd_link, ioaddr + WRITE_PTR);
	outw(tx_block, ioaddr);
	lp->tx_cmd_link = tx_block + 20;

	/* Set the next free tx region. */
	lp->tx_head = tx_block + TX_BUF_SIZE;
	if (lp->tx_head > TX_BUF_END - TX_BUF_SIZE)
		lp->tx_head = TX_BUF_START;

    if (net_debug > 4) {
		printk("%s: EExp @%x send length = %d, tx_block %3x, next %3x, "
			   "reap %4x status %4.4x.\n", dev->name, ioaddr, length,
			   tx_block, lp->tx_head, lp->tx_reap, inw(ioaddr + SCB_STATUS));
    }

	if (lp->tx_head != lp->tx_reap)
		dev->tbusy = 0;
}

static void
eexp_rx(struct device *dev)
{
	struct net_local *lp = (struct net_local *)dev->priv;
	short ioaddr = dev->base_addr;
	short saved_write_ptr = inw(ioaddr + WRITE_PTR);
	short rx_head = lp->rx_head;
	short rx_tail = lp->rx_tail;
	short boguscount = 10;
	short frame_status;

	/* Set the read pointer to the Rx frame. */
	outw(rx_head, ioaddr + READ_PTR);
	while ((frame_status = inw(ioaddr)) < 0) {		/* Command complete */
		short rfd_cmd = inw(ioaddr);
		short next_rx_frame = inw(ioaddr);
		short data_buffer_addr = inw(ioaddr);
		short pkt_len;
		
		/* Set the read pointer the data buffer. */
		outw(data_buffer_addr, ioaddr + READ_PTR);
		pkt_len = inw(ioaddr);

		if (rfd_cmd != 0  ||  data_buffer_addr != rx_head + 22
			||  pkt_len & 0xC000 != 0xC000) {
			printk("%s: Rx frame at %#x corrupted, status %04x cmd %04x"
				   "next %04x data-buf @%04x %04x.\n", dev->name, rx_head,
				   frame_status, rfd_cmd, next_rx_frame, data_buffer_addr,
				   pkt_len);
		} else if ((frame_status & 0x2000) == 0) {
			/* Frame Rxed, but with error. */
			lp->stats.rx_errors++;
			if (frame_status & 0x0800) lp->stats.rx_crc_errors++;
			if (frame_status & 0x0400) lp->stats.rx_frame_errors++;
			if (frame_status & 0x0200) lp->stats.rx_fifo_errors++;
			if (frame_status & 0x0100) lp->stats.rx_over_errors++;
			if (frame_status & 0x0080) lp->stats.rx_length_errors++;
		} else {
			/* Malloc up new buffer. */
			int sksize;
			struct sk_buff *skb;

			pkt_len &= 0x3fff;
			sksize = sizeof(struct sk_buff) + pkt_len;
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

			outw(data_buffer_addr + 10, ioaddr + READ_PTR);

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

		/* Clear the status word and set End-of-List on the rx frame. */
		outw(rx_head, ioaddr + WRITE_PTR);
		outw(0x0000, ioaddr);
		outw(0xC000, ioaddr);
#ifndef final_version
		if (next_rx_frame != rx_head + RX_BUF_SIZE
			&& next_rx_frame != RX_BUF_START) {
			printk("%s: Rx next frame at %#x is %#x instead of %#x.\n", dev->name,
				   rx_head, next_rx_frame, rx_head + RX_BUF_SIZE);
			next_rx_frame = rx_head + RX_BUF_SIZE;
			if (next_rx_frame >= RX_BUF_END - RX_BUF_SIZE)
				next_rx_frame = RX_BUF_START;
		}
#endif
		outw(rx_tail+2, ioaddr + WRITE_PTR);
		outw(0x0000, ioaddr);	/* Clear the end-of-list on the prev. RFD. */

#ifndef final_version
		outw(rx_tail+4, ioaddr + READ_PTR);
		if (inw(ioaddr) != rx_head) {
			printk("%s: Rx buf link mismatch, at %04x link %04x instead of %04x.\n",
				   dev->name, rx_tail, (outw(rx_tail+4, ioaddr + READ_PTR),inw(ioaddr)),
				   rx_head);
			outw(rx_head, ioaddr);
		}
#endif

		rx_tail = rx_head;
		rx_head = next_rx_frame;
		if (--boguscount == 0)
			break;
		outw(rx_head, ioaddr + READ_PTR);
	}
	
	lp->rx_head = rx_head;
	lp->rx_tail = rx_tail;
	
	/* Restore the original write pointer. */
	outw(saved_write_ptr, ioaddr + WRITE_PTR);
}

/*
 * Local variables:
 *  compile-command: "gcc -D__KERNEL__ -I/usr/src/linux/net/inet -I/usr/src/linux/drivers/net -Wall -Wstrict-prototypes -O6 -m486 -c eexpress.c"
 *  version-control: t
 *  kept-new-versions: 5
 *  tab-width: 4
 * End:
 */
