/* 3c507.c: An EtherLink16 device driver for Linux. */
/*
	Written 1993 by Donald Becker.
	Copyright 1993 United States Government as represented by the Director,
	National Security Agency.  This software may only be used and distributed
	according to the terms of the GNU Public License as modified by SRC,
	incorported herein by reference.

	The author may be reached as becker@super.org or
	C/O Supercomputing Research Ctr., 17100 Science Dr., Bowie MD 20715

	Thanks go to jennings@Montrouge.SMR.slb.com ( Patrick Jennings)
	and jrs@world.std.com (Rick Sladkey) for testing and bugfixes.
	Mark Salazar <leslie@access.digex.net> made the changes for cards with
	only 16K packet buffers.

	Things remaining to do:
	Verify that the tx and rx buffers don't have fencepost errors.
	Move the theory of operation and memory map documentation.
	The statistics need to be updated correctly.
*/

static char *version =
	"3c507.c:v0.99-15f 2/17/94 Donald Becker (becker@super.org)\n";

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
#define kfree_skbmem(addr, size) kfree_s(addr,size);
#else
#include <linux/malloc.h>
#endif

/* use 0 for production, 1 for verification, 2..7 for debug */
#ifndef NET_DEBUG
#define NET_DEBUG 1
#endif
static unsigned int net_debug = NET_DEBUG;

/*
  			Details of the i82586.

   You'll really need the databook to understand the details of this part,
   but the outline is that the i82586 has two seperate processing units.
   Both are started from a list of three configuration tables, of which only
   the last, the System Control Block (SCB), is used after reset-time.  The SCB
   has the following fileds:
		Status word
		Command word
		Tx/Command block addr.
		Rx block addr.
   The command word accepts the following controls for the Tx and Rx units:
  */

#define	 CUC_START	 0x0100
#define	 CUC_RESUME	 0x0200
#define	 CUC_SUSPEND 0x0300
#define	 RX_START	 0x0010
#define	 RX_RESUME	 0x0020
#define	 RX_SUSPEND	 0x0030

/* The Rx unit uses a list of frame descriptors and a list of data buffer
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
	ushort rx_head;
	ushort rx_tail;
	ushort tx_head;
	ushort tx_cmd_link;
	ushort tx_reap;
};

/*
  		Details of the EtherLink16 Implementation
  The 3c507 is a generic shared-memory i82586 implementation.
  The host can map 16K, 32K, 48K, or 64K of the 64K memory into
  0x0[CD][08]0000, or all 64K into 0xF[02468]0000.
  */

/* Offsets from the base I/O address. */
#define	SA_DATA		0	/* Station address data, or 3Com signature. */
#define MISC_CTRL	6	/* Switch the SA_DATA banks, and bus config bits. */
#define RESET_IRQ	10	/* Reset the latched IRQ line. */
#define SIGNAL_CA	11	/* Frob the 82586 Channel Attention line. */
#define ROM_CONFIG	13
#define MEM_CONFIG	14
#define IRQ_CONFIG	15

/* The ID port is used at boot-time to locate the ethercard. */
#define ID_PORT		0x100

/* Offsets to registers in the mailbox (SCB). */
#define iSCB_STATUS	0x8
#define iSCB_CMD		0xA
#define iSCB_CBL		0xC	/* Command BLock offset. */
#define iSCB_RFA		0xE	/* Rx Frame Area offset. */

/*  Since the 3c507 maps the shared memory window so that the last byte is
	at 82586 address FFFF, the first byte is at 82586 address 0, 16K, 32K, or
	48K cooresponding to window sizes of 64K, 48K, 32K and 16K respectively. 
	We can account for this be setting the 'SBC Base' entry in the ISCP table
	below for all the 16 bit offset addresses, and also adding the 'SCB Base'
	value to all 24 bit physical addresses (in the SCP table and the TX and RX
	Buffer Descriptors).
					-Mark	
	*/
#define SCB_BASE		((unsigned)64*1024 - (dev->mem_end - dev->mem_start))
 
/*
  What follows in 'init_words[]' is the "program" that is downloaded to the
  82586 memory.	 It's mostly tables and command blocks, and starts at the
  reset address 0xfffff6.  This is designed to be similar to the EtherExpress,
  thus the unusual location of the SCB at 0x0008.

  Even with the additional "don't care" values, doing it this way takes less
  program space than initializing the individual tables, and I feel it's much
  cleaner.

  The databook is particularly useless for the first two structures, I had
  to use the Crynwr driver as an example.

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
#define TX_BUF_SIZE 	(1518+14+20+16) /* packet+header+TBD */

#define RX_BUF_START	0x2000
#define RX_BUF_SIZE 	(1518+14+18)	/* packet+header+RBD */
#define RX_BUF_END		(dev->mem_end - dev->mem_start)

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

unsigned short init_words[] = {
	/*	System Configuration Pointer (SCP). */
	0x0000,					/* Set bus size to 16 bits. */
	0,0,					/* pad words. */
	0x0000,0x0000,			/* ISCP phys addr, set in init_82586_mem(). */

	/*	Intermediate System Configuration Pointer (ISCP). */
	0x0001,					/* Status word that's cleared when init is done. */
	0x0008,0,0,				/* SCB offset, (skip, skip) */

	/* System Control Block (SCB). */
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
	0, CmdMulticastList, IDLELOOP, 0,
};

/* Index to functions, as function prototypes. */

extern int el16_probe(struct device *dev);	/* Called from Space.c */

static int	el16_probe1(struct device *dev, short ioaddr);
static int	el16_open(struct device *dev);
static int	el16_send_packet(struct sk_buff *skb, struct device *dev);
static void	el16_interrupt(int reg_ptr);
static void el16_rx(struct device *dev);
static int	el16_close(struct device *dev);
static struct enet_statistics *el16_get_stats(struct device *dev);

static void hardware_send_packet(struct device *dev, void *buf, short length);
void init_82586_mem(struct device *dev);


/* Check for a network adaptor of this type, and return '0' iff one exists.
	If dev->base_addr == 0, probe all likely locations.
	If dev->base_addr == 1, always return failure.
	If dev->base_addr == 2, (detachable devices only) alloate space for the
	device and return success.
	*/
int
el16_probe(struct device *dev)
{
	/* Don't probe all settable addresses, 0x[23][0-F]0, just common ones. */
	int *port, ports[] = {0x300, 0x320, 0x340, 0x280, 0};
	int base_addr = dev->base_addr;
	ushort lrs_state = 0xff, i;

	if (base_addr > 0x1ff)	/* Check a single specified location. */
		return el16_probe1(dev, base_addr);
	else if (base_addr > 0)
		return ENXIO;		/* Don't probe at all. */

	/* Send the ID sequence to the ID_PORT to enable the board. */
	outb(0x00, ID_PORT);
	for(i = 0; i < 255; i++) {
		outb(lrs_state, ID_PORT);
		lrs_state <<= 1;
		if (lrs_state & 0x100)
			lrs_state ^= 0xe7;
	}
	outb(0x00, ID_PORT);

	for (port = &ports[0]; *port; port++) {
		short ioaddr = *port;
#if 0
		/* This is my original code. */
		if (inb(ioaddr) == '*' && inb(ioaddr+1) == '3'
			&& inb(ioaddr+2) == 'C' && inb(ioaddr+3) == 'O'
			&& el16_probe1(dev, *port) == 0)
			return 0;
#else
	/* This is code from jennings@Montrouge.SMR.slb.com, done so that
	   the string can be printed out. */
		char res[5];
		res[0] = inb(ioaddr); res[1] = inb(ioaddr+1);
		res[2] = inb(ioaddr+2); res[3] = inb(ioaddr+3);
		res[4] = 0;
		if (res[0] == '*' && res[1] == '3'
			&& res[2] == 'C' && res[3] == 'O'
			&& el16_probe1(dev, *port) == 0)
		  return 0;
#endif
	}

	return ENODEV;			/* ENODEV would be more accurate. */
}

int el16_probe1(struct device *dev, short ioaddr)
{
	int i, irq, irqval;

	printk("%s: 3c507 at %#x,", dev->name, ioaddr);

	/* We should make a few more checks here, like the first three octets of
	   the S.A. for the manufactor's code. */ 

	irq = inb(ioaddr + IRQ_CONFIG) & 0x0f;

	irqval = request_irq(irq, &el16_interrupt);
	if (irqval) {
		printk ("unable to get IRQ %d (irqval=%d).\n", irq, irqval);
		return EAGAIN;
	}
	
	/* We've committed to using the board, and can start filling in *dev. */
	snarf_region(ioaddr, 16);
	dev->base_addr = ioaddr;

	outb(0x01, ioaddr + MISC_CTRL);
	for (i = 0; i < 6; i++) {
		dev->dev_addr[i] = inb(ioaddr + i);
		printk(" %02x", dev->dev_addr[i]);
	}

	if ((dev->mem_start & 0xf) > 0)
		net_debug = dev->mem_start & 7;

#ifdef MEM_BASE
	dev->mem_start = MEM_BASE;
	dev->mem_end = dev->mem_start + 0x10000;
#else
	{
		int base;
		int size;
		char mem_config = inb(ioaddr + MEM_CONFIG);
		if (mem_config & 0x20) {
			size = 64*1024;
			base = 0xf00000 + (mem_config & 0x08 ? 0x080000
							   : ((mem_config & 3) << 17));
		} else {
			size = ((mem_config & 3) + 1) << 14;
			base = 0x0c0000 + ( (mem_config & 0x18) << 12);
		}
		dev->mem_start = base;
		dev->mem_end = base + size;
	}
#endif

	dev->if_port = (inb(ioaddr + ROM_CONFIG) & 0x80) ? 1 : 0;
	dev->irq = inb(ioaddr + IRQ_CONFIG) & 0x0f;

	printk(", IRQ %d, %sternal xcvr, memory %#lx-%#lx.\n", dev->irq,
		   dev->if_port ? "ex" : "in", dev->mem_start, dev->mem_end-1);

	if (net_debug)
		printk(version);

	/* Initialize the device structure. */
	dev->priv = kmalloc(sizeof(struct net_local), GFP_KERNEL);
	memset(dev->priv, 0, sizeof(struct net_local));

	dev->open		= el16_open;
	dev->stop		= el16_close;
	dev->hard_start_xmit = el16_send_packet;
	dev->get_stats	= el16_get_stats;

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



static int
el16_open(struct device *dev)
{
	irq2dev_map[dev->irq] = dev;

	/* Initialize the 82586 memory and start it. */
	init_82586_mem(dev);

	dev->tbusy = 0;
	dev->interrupt = 0;
	dev->start = 1;
	return 0;
}

static int
el16_send_packet(struct sk_buff *skb, struct device *dev)
{
	struct net_local *lp = (struct net_local *)dev->priv;
	int ioaddr = dev->base_addr;
	short *shmem = (short*)dev->mem_start;

	if (dev->tbusy) {
		/* If we get here, some higher level has decided we are broken.
		   There should really be a "kick me" function call instead. */
		int tickssofar = jiffies - dev->trans_start;
		if (tickssofar < 5)
			return 1;
		if (net_debug > 1)
			printk("%s: transmit timed out, %s?  ", dev->name,
				   shmem[iSCB_STATUS>>1] & 0x8000 ? "IRQ conflict" :
				   "network cable problem");
		/* Try to restart the adaptor. */
		if (lp->last_restart == lp->stats.tx_packets) {
			if (net_debug > 1) printk("Resetting board.\n");
			/* Completely reset the adaptor. */
			init_82586_mem(dev);
		} else {
			/* Issue the channel attention signal and hope it "gets better". */
			if (net_debug > 1) printk("Kicking board.\n");
			shmem[iSCB_CMD>>1] = 0xf000|CUC_START|RX_START;
			outb(0, ioaddr + SIGNAL_CA);			/* Issue channel-attn. */
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
		outb(0x80, ioaddr + MISC_CTRL);
		hardware_send_packet(dev, buf, length);
		dev->trans_start = jiffies;
		/* Enable the 82586 interrupt input. */
		outb(0x84, ioaddr + MISC_CTRL);
	}

	if (skb->free)
		kfree_skb (skb, FREE_WRITE);

	/* You might need to clean up and record Tx statistics here. */

	return 0;
}

/*	The typical workload of the driver:
	Handle the network interface interrupts. */
static void
el16_interrupt(int reg_ptr)
{
	int irq = -(((struct pt_regs *)reg_ptr)->orig_eax+2);
	struct device *dev = (struct device *)(irq2dev_map[irq]);
	struct net_local *lp;
	int ioaddr, status, boguscount = 0;
	ushort ack_cmd = 0;
	ushort *shmem;
	
	if (dev == NULL) {
		printk ("net_interrupt(): irq %d for unknown device.\n", irq);
		return;
	}
	dev->interrupt = 1;
	
	ioaddr = dev->base_addr;
	lp = (struct net_local *)dev->priv;
	shmem = ((ushort*)dev->mem_start);
	
	status = shmem[iSCB_STATUS>>1];
	
	if (net_debug > 4) {
		printk("%s: 3c507 interrupt, status %4.4x.\n", dev->name, status);
	}

	/* Disable the 82586's input to the interrupt line. */
	outb(0x80, ioaddr + MISC_CTRL);

	/* Reap the Tx packet buffers. */
	while (lp->tx_reap != lp->tx_head) {
	  unsigned short tx_status = shmem[lp->tx_reap>>1];

	  if (tx_status == 0) {
		if (net_debug > 5)  printk("Couldn't reap %#x.\n", lp->tx_reap);
		break;
	  }
	  if (tx_status & 0x2000) {
		lp->stats.tx_packets++;
		lp->stats.collisions += tx_status & 0xf;
		dev->tbusy = 0;
		mark_bh(INET_BH);		/* Inform upper layers. */
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
	  if (lp->tx_reap > RX_BUF_START - TX_BUF_SIZE)
		lp->tx_reap = TX_BUF_START;
	  if (++boguscount > 4)
		break;
	}

	if (status & 0x4000) { /* Packet received. */
		if (net_debug > 5)
			printk("Received packet, rx_head %04x.\n", lp->rx_head);
		el16_rx(dev);
	}

	/* Acknowledge the interrupt sources. */
	ack_cmd = status & 0xf000;

	if ((status & 0x0700) != 0x0200 && dev->start) {
		if (net_debug)
			printk("%s: Command unit stopped, status %04x, restarting.\n",
				   dev->name, status);
		/* If this ever occurs we should really re-write the idle loop, reset
		   the Tx list, and do a complete restart of the command unit.
		   For now we rely on the Tx timeout if the resume doesn't work. */
		ack_cmd |= CUC_RESUME;
	}

	if ((status & 0x0070) != 0x0040  &&  dev->start) {
	  static void init_rx_bufs(struct device *);
		/* The Rx unit is not ready, it must be hung.  Restart the receiver by
		   initializing the rx buffers, and issuing an Rx start command. */
		if (net_debug)
			printk("%s: Rx unit stopped, status %04x, restarting.\n",
				   dev->name, status);
		init_rx_bufs(dev);
		shmem[iSCB_RFA >> 1] = RX_BUF_START;
		ack_cmd |= RX_START;
	}

	shmem[iSCB_CMD>>1] = ack_cmd;
	outb(0, ioaddr + SIGNAL_CA);			/* Issue channel-attn. */

	/* Clear the latched interrupt. */
	outb(0, ioaddr + RESET_IRQ);

	/* Enable the 82586's interrupt input. */
	outb(0x84, ioaddr + MISC_CTRL);

	return;
}

static int
el16_close(struct device *dev)
{
	int ioaddr = dev->base_addr;
	ushort *shmem = (short*)dev->mem_start;

	dev->tbusy = 1;
	dev->start = 0;

	/* Flush the Tx and disable Rx. */
	shmem[iSCB_CMD >> 1] = RX_SUSPEND | CUC_SUSPEND;
	outb(0, ioaddr + SIGNAL_CA);

	/* Disable the 82586's input to the interrupt line. */
	outb(0x80, ioaddr + MISC_CTRL);

	/* We always physically use the IRQ line, so we don't do free_irq().
	   We do remove ourselves from the map. */

	irq2dev_map[dev->irq] = 0;

	/* Update the statistics here. */

	return 0;
}

/* Get the current statistics.	This may be called with the card open or
   closed. */
static struct enet_statistics *
el16_get_stats(struct device *dev)
{
	struct net_local *lp = (struct net_local *)dev->priv;

	/* ToDo: decide if there are any useful statistics from the SCB. */

	return &lp->stats;
}

/* Initialize the Rx-block list. */
static void
init_rx_bufs(struct device *dev)
{
	struct net_local *lp = (struct net_local *)dev->priv;
	unsigned short *write_ptr;
	unsigned short SCB_base = SCB_BASE;

	int cur_rxbuf = lp->rx_head = RX_BUF_START;
	
	/* Initialize each Rx frame + data buffer. */
	do {	/* While there is room for one more. */

	  write_ptr = (unsigned short *)(dev->mem_start + cur_rxbuf);

		*write_ptr++ = 0x0000; 				/* Status */
		*write_ptr++ = 0x0000;				/* Command */
		*write_ptr++ = cur_rxbuf + RX_BUF_SIZE; /* Link */
		*write_ptr++ = cur_rxbuf + 22;		/* Buffer offset */
		*write_ptr++ = 0x0000; 				/* Pad for dest addr. */
		*write_ptr++ = 0x0000;
		*write_ptr++ = 0x0000;
		*write_ptr++ = 0x0000; 				/* Pad for source addr. */
		*write_ptr++ = 0x0000;
		*write_ptr++ = 0x0000;
		*write_ptr++ = 0x0000;				/* Pad for protocol. */
		
		*write_ptr++ = 0x0000;				/* Buffer: Actual count */
		*write_ptr++ = -1;					/* Buffer: Next (none). */
		*write_ptr++ = cur_rxbuf + 0x20 + SCB_base;	/* Buffer: Address low */
		*write_ptr++ = 0x0000;
		/* Finally, the number of bytes in the buffer. */
		*write_ptr++ = 0x8000 + RX_BUF_SIZE-0x20;
		
		lp->rx_tail = cur_rxbuf;
		cur_rxbuf += RX_BUF_SIZE;
	} while (cur_rxbuf <= RX_BUF_END - RX_BUF_SIZE);
	
	/* Terminate the list by setting the EOL bit, and wrap the pointer to make
	   the list a ring. */
	write_ptr = (unsigned short *)
	  (dev->mem_start + lp->rx_tail + 2);
	*write_ptr++ = 0xC000;					/* Command, mark as last. */
	*write_ptr++ = lp->rx_head;				/* Link */

}

void
init_82586_mem(struct device *dev)
{
	struct net_local *lp = (struct net_local *)dev->priv;
	short ioaddr = dev->base_addr;
	ushort *shmem = (short*)dev->mem_start;

	/* Enable loopback to protect the wire while starting up,
	   and hold the 586 in reset during the memory initialization. */
	outb(0x20, ioaddr + MISC_CTRL);

	/* Fix the ISCP address and base. */
	init_words[3] = SCB_BASE;
	init_words[7] = SCB_BASE;

	/* Write the words at 0xfff6 (address-aliased to 0xfffff6). */
	memcpy((void*)dev->mem_end-10, init_words, 10);

	/* Write the words at 0x0000. */
	memcpy((char*)dev->mem_start, init_words + 5, sizeof(init_words) - 10);

	/* Fill in the station address. */
	memcpy((char*)dev->mem_start+SA_OFFSET, dev->dev_addr,
		   sizeof(dev->dev_addr));

	/* The Tx-block list is written as needed.  We just set up the values. */
	lp->tx_cmd_link = IDLELOOP + 4;
	lp->tx_head = lp->tx_reap = TX_BUF_START;

	init_rx_bufs(dev);

	/* Start the 586 by releasing the reset line, but leave loopback. */
	outb(0xA0, ioaddr + MISC_CTRL);

	/* This was time consuming to track down: you need to give two channel
	   attention signals to reliably start up the i82586. */
	outb(0, ioaddr + SIGNAL_CA);

	{
		int boguscnt = 50;
		while (shmem[iSCB_STATUS>>1] == 0)
			if (--boguscnt == 0) {
				printk("%s: i82586 initialization timed out with status %04x,"
					   "cmd %04x.\n", dev->name,
					   shmem[iSCB_STATUS>>1], shmem[iSCB_CMD>>1]);
				break;
			}
		/* Issue channel-attn -- the 82586 won't start. */
		outb(0, ioaddr + SIGNAL_CA);
	}

	/* Disable loopback and enable interrupts. */
	outb(0x84, ioaddr + MISC_CTRL);
	if (net_debug > 4)
		printk("%s: Initialized 82586, status %04x.\n", dev->name,
			   shmem[iSCB_STATUS>>1]);
	return;
}

static void
hardware_send_packet(struct device *dev, void *buf, short length)
{
	struct net_local *lp = (struct net_local *)dev->priv;
	short ioaddr = dev->base_addr;
	ushort tx_block = lp->tx_head;
	ushort *write_ptr =	  (ushort *)(dev->mem_start + tx_block);

	/* Set the write pointer to the Tx block, and put out the header. */
	*write_ptr++ = 0x0000;				/* Tx status */
	*write_ptr++ = CMD_INTR|CmdTx;		/* Tx command */
	*write_ptr++ = tx_block+16;			/* Next command is a NoOp. */
	*write_ptr++ = tx_block+8;			/* Data Buffer offset. */

	/* Output the data buffer descriptor. */
	*write_ptr++ = length | 0x8000;		/* Byte count parameter. */
	*write_ptr++ = -1;					/* No next data buffer. */
	*write_ptr++ = tx_block+22+SCB_BASE;/* Buffer follows the NoOp command. */
	*write_ptr++ = 0x0000;				/* Buffer address high bits (always zero). */

	/* Output the Loop-back NoOp command. */
	*write_ptr++ = 0x0000;				/* Tx status */
	*write_ptr++ = CmdNOp;				/* Tx command */
	*write_ptr++ = tx_block+16;			/* Next is myself. */

	/* Output the packet at the write pointer. */
	memcpy(write_ptr, buf, length);

	/* Set the old command link pointing to this send packet. */
	*(ushort*)(dev->mem_start + lp->tx_cmd_link) = tx_block;
	lp->tx_cmd_link = tx_block + 20;

	/* Set the next free tx region. */
	lp->tx_head = tx_block + TX_BUF_SIZE;
	if (lp->tx_head > RX_BUF_START - TX_BUF_SIZE)
		lp->tx_head = TX_BUF_START;

	if (net_debug > 4) {
		printk("%s: 3c507 @%x send length = %d, tx_block %3x, next %3x.\n",
			   dev->name, ioaddr, length, tx_block, lp->tx_head);
	}

	if (lp->tx_head != lp->tx_reap)
		dev->tbusy = 0;
}

static void
el16_rx(struct device *dev)
{
	struct net_local *lp = (struct net_local *)dev->priv;
	short *shmem = (short*)dev->mem_start;
	ushort rx_head = lp->rx_head;
	ushort rx_tail = lp->rx_tail;
	ushort boguscount = 10;
	short frame_status;

	while ((frame_status = shmem[rx_head>>1]) < 0) {   /* Command complete */
		ushort *read_frame =  (short *)(dev->mem_start + rx_head);
		ushort rfd_cmd = read_frame[1];
		ushort next_rx_frame = read_frame[2];
		ushort data_buffer_addr = read_frame[3];
		ushort *data_frame = (short *)(dev->mem_start + data_buffer_addr);
		ushort pkt_len = data_frame[0];

		if (rfd_cmd != 0 || data_buffer_addr != rx_head + 22
			|| pkt_len & 0xC000 != 0xC000) {
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

			/* 'skb->data' points to the start of sk_buff data area. */
			memcpy(skb->data, data_frame + 5, pkt_len);
		
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
			lp->stats.rx_packets++;
		}

		/* Clear the status word and set End-of-List on the rx frame. */
		read_frame[0] = 0;
		read_frame[1] = 0xC000;
		/* Clear the end-of-list on the prev. RFD. */
		*(short*)(dev->mem_start + rx_tail + 2) = 0x0000;

		rx_tail = rx_head;
		rx_head = next_rx_frame;
		if (--boguscount == 0)
			break;
	}

	lp->rx_head = rx_head;
	lp->rx_tail = rx_tail;
}

/*
 * Local variables:
 *  compile-command: "gcc -D__KERNEL__ -I/usr/src/linux/net/inet -I/usr/src/linux/drivers/net -Wall -Wstrict-prototypes -O6 -m486 -c 3c507.c"
 *  version-control: t
 *  kept-new-versions: 5
 *  tab-width: 4
 *  c-indent-level: 4
 * End:
 */

