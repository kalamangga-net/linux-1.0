/* lance.c: An AMD LANCE ethernet driver for linux. */
/*
    Written 1993 by Donald Becker.

    Copyright 1993 United States Government as represented by the
    Director, National Security Agency.  This software may be used and
    distributed according to the terms of the GNU Public License,
    incorporated herein by reference.

    This driver is for the Allied Telesis AT1500 and HP J2405A, and should work
    with most other LANCE-based bus-master (NE2100 clone) ethercards.

    The author may be reached as becker@super.org or
    C/O Supercomputing Research Ctr., 17100 Science Dr., Bowie MD 20715
*/

static char *version = "lance.c:v0.14g 12/21/93 becker@super.org\n";

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/ptrace.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/malloc.h>
#include <linux/interrupt.h>
#include <asm/bitops.h>
#include <asm/io.h>
#include <asm/dma.h>

#include "dev.h"
#include "eth.h"
#include "skbuff.h"
#include "arp.h"

#ifndef HAVE_PORTRESERVE
#define check_region(addr, size)	0
#define snarf_region(addr, size)	do ; while(0)
#endif

#ifndef HAVE_ALLOC_SKB
#define alloc_skb(size, priority) (struct sk_buff *) kmalloc(size,priority)
#define kfree_skbmem(buff, size) kfree_s(buff,size)
#endif

struct device *init_etherdev(struct device *dev, int sizeof_private,
			     unsigned long *mem_startp);

#ifdef LANCE_DEBUG
int lance_debug = LANCE_DEBUG;
#else
int lance_debug = 1;
#endif

#ifndef LANCE_DMA
#define LANCE_DMA	5
#endif

/*
  		Theory of Operation

I. Board Compatibility

This device driver is designed for the AMD 79C960, the "PCnet-ISA
single-chip ethernet controller for ISA".  This chip is used in a wide
variety of boards from vendors such as Allied Telesis, HP, Kingston,
and Boca.  This driver is also intended to work with older AMD 7990
designs, such as the NE1500 and NE2100.  For convenience, I use the name
LANCE to refer to either AMD chip.

II. Board-specific settings

The driver is designed to work the boards that use the faster
bus-master mode, rather than in shared memory mode.  (Only older designs
have on-board buffer memory needed to support the slower shared memory mode.)

Most boards have jumpered settings for the I/O base, IRQ line, and DMA channel.
This driver probes the likely base addresses, {0x300, 0x320, 0x340, 0x360}.
After the board is found it generates an DMA-timeout interrupt and uses
autoIRQ to find the IRQ line.  The DMA channel defaults to LANCE_DMA, or it
can be set with the low bits of the otherwise-unused dev->mem_start value.

The HP-J2405A board is an exception: with this board it's easy to read the
EEPROM-set values for the base, IRQ, and DMA.  Of course you must already
_know_ the base address, but that entry is for changing the EEPROM.

III. Driver operation

IIIa. Ring buffers
The LANCE uses ring buffers of Tx and Rx descriptors.  Each entry describes
the base and length of the data buffer, along with status bits.  The length
of these buffers is set by LANCE_LOG_{RX,TX}_BUFFERS, which is log_2() of
the buffer length (rather than being directly the buffer length) for
implementation ease.  The current values are 2 (Tx) and 4 (Rx), which leads to
ring sizes of 4 (Tx) and 16 (Rx).  Increasing the number of ring entries
needlessly uses extra space and reduces the chance that an upper layer will
be able to reorder queued Tx packets based on priority.  Decreasing the number
of entries makes it more difficult to achieve back-to-back packet transmission
and increases the chance that Rx ring will overflow.  (Consider the worst case
of receiving back-to-back minimum-sized packets.)

The LANCE has the capability to "chain" both Rx and Tx buffers, but this driver
statically allocates full-sized (slightly oversized -- PKT_BUF_SZ) buffers to
avoid the administrative overhead. For the Rx side this avoids dynamically
allocating full-sized buffers "just in case", at the expense of a
memory-to-memory data copy for each packet received.  For most systems this
is an good tradeoff: the Rx buffer will always be in low memory, the copy
is inexpensive, and it primes the cache for later packet processing.  For Tx
the buffers are only used when needed as low-memory bounce buffers.

IIIB. 16M memory limitations.
For the ISA bus master mode all structures used directly by the LANCE,
the initialization block, Rx and Tx rings, and data buffers, must be
accessable from the ISA bus, i.e. in the lower 16M of real memory.
This is a problem for current Linux kernels on >16M machines. The network
devices are initialized after memory initialization, and the kernel doles out
memory from the top of memory downward.  The current solution is to have a
special network initialization routine that's called before memory
initialization; this will eventually be generalized for all network devices.
As mentioned before, low-memory "bounce-buffers" are used when needed.

IIIC. Synchronization
The driver runs as two independent, single-threaded flows of control.  One
is the send-packet routine, which enforces single-threaded use by the
dev->tbusy flag.  The other thread is the interrupt handler, which is single
threaded by the hardware and other software.

The send packet thread has partial control over the Tx ring and 'dev->tbusy'
flag.  It sets the tbusy flag whenever it's queuing a Tx packet. If the next
queue slot is empty, it clears the tbusy flag when finished otherwise it sets
the 'lp->tx_full' flag.

The interrupt handler has exclusive control over the Rx ring and records stats
from the Tx ring.  (The Tx-done interrupt can't be selectively turned off, so
we can't avoid the interrupt overhead by having the Tx routine reap the Tx
stats.)  After reaping the stats, it marks the queue entry as empty by setting
the 'base' to zero.  Iff the 'lp->tx_full' flag is set, it clears both the
tx_full and tbusy flags.

*/

/* Set the number of Tx and Rx buffers, using Log_2(# buffers).
   Reasonable default values are 4 Tx buffers, and 16 Rx buffers.
   That translates to 2 (4 == 2^^2) and 4 (16 == 2^^4). */
#ifndef LANCE_LOG_TX_BUFFERS
#define LANCE_LOG_TX_BUFFERS 4
#define LANCE_LOG_RX_BUFFERS 4
#endif

#define TX_RING_SIZE		(1 << (LANCE_LOG_TX_BUFFERS))
#define TX_RING_MOD_MASK	(TX_RING_SIZE - 1)
#define TX_RING_LEN_BITS	((LANCE_LOG_TX_BUFFERS) << 29)

#define RX_RING_SIZE		(1 << (LANCE_LOG_RX_BUFFERS))
#define RX_RING_MOD_MASK	(RX_RING_SIZE - 1)
#define RX_RING_LEN_BITS	((LANCE_LOG_RX_BUFFERS) << 29)

#define PKT_BUF_SZ	1544

/* Offsets from base I/O address. */
#define LANCE_DATA 0x10
#define LANCE_ADDR 0x12
#define LANCE_RESET 0x14
#define LANCE_BUS_IF 0x16
#define LANCE_TOTAL_SIZE 0x18

/* The LANCE Rx and Tx ring descriptors. */
struct lance_rx_head {
    int	base;
    short buf_length;		/* This length is 2's complement (negative)! */
    short msg_length;		/* This length is "normal". */
};

struct lance_tx_head {
    int	  base;
    short length;		/* Length is 2's complement (negative)! */
    short misc;
};

/* The LANCE initialization block, described in databook. */
struct lance_init_block {
    unsigned short mode;	/* Pre-set mode (reg. 15) */
    unsigned char phys_addr[6];	/* Physical ethernet address */
    unsigned filter[2];		/* Multicast filter (unused). */
    /* Receive and transmit ring base, along with extra bits. */
    unsigned rx_ring;		/* Tx and Rx ring base pointers */
    unsigned tx_ring;
};

struct lance_private {
    char devname[8];
    /* These must aligned on 8-byte boundaries. */
    struct lance_rx_head rx_ring[RX_RING_SIZE];
    struct lance_tx_head tx_ring[TX_RING_SIZE];
    struct lance_init_block	init_block;
    long rx_buffs;		/* Address of Rx and Tx buffers. */
    /* Tx low-memory "bounce buffer" address. */
    char (*tx_bounce_buffs)[PKT_BUF_SZ];
    int	cur_rx, cur_tx;		/* The next free ring entry */
    int dirty_rx, dirty_tx;	/* The ring entries to be free()ed. */
    int dma;
    struct enet_statistics stats;
    char old_lance;
    int pad0, pad1;		/* Used for alignment */
};

unsigned long lance_probe1(short ioaddr, unsigned long mem_start);
static int lance_open(struct device *dev);
static void lance_init_ring(struct device *dev);
static int lance_start_xmit(struct sk_buff *skb, struct device *dev);
static int lance_rx(struct device *dev);
static void lance_interrupt(int reg_ptr);
static int lance_close(struct device *dev);
static struct enet_statistics *lance_get_stats(struct device *dev);
#ifdef HAVE_MULTICAST
static void set_multicast_list(struct device *dev, int num_addrs, void *addrs);
#endif



unsigned long lance_init(unsigned long mem_start, unsigned long mem_end)
{
    int *port, ports[] = {0x300, 0x320, 0x340, 0x360, 0};

    for (port = &ports[0]; *port; port++) {
	int ioaddr = *port;

	if (   check_region(ioaddr, LANCE_TOTAL_SIZE) == 0
	    && inb(ioaddr + 14) == 0x57
	    && inb(ioaddr + 15) == 0x57) {
	    mem_start = lance_probe1(ioaddr, mem_start);
	}
    }

    return mem_start;
}

unsigned long lance_probe1(short ioaddr, unsigned long mem_start)
{
    struct device *dev;
    struct lance_private *lp;
    int hpJ2405A = 0;
    int i, reset_val;

    hpJ2405A = (inb(ioaddr) == 0x08 && inb(ioaddr+1) == 0x00
		&& inb(ioaddr+2) == 0x09);

    /* Reset the LANCE.  */
    reset_val = inw(ioaddr+LANCE_RESET); /* Reset the LANCE */

    /* The Un-Reset needed is only needed for the real NE2100, and will
       confuse the HP board. */
    if (!hpJ2405A)
	outw(reset_val, ioaddr+LANCE_RESET);

    outw(0x0000, ioaddr+LANCE_ADDR); /* Switch to window 0 */
    if (inw(ioaddr+LANCE_DATA) != 0x0004)
	return mem_start;

    dev = init_etherdev(0, sizeof(struct lance_private)
			+ PKT_BUF_SZ*(RX_RING_SIZE + TX_RING_SIZE),
			&mem_start);

    printk("%s: LANCE at %#3x,", dev->name, ioaddr);

    /* There is a 16 byte station address PROM at the base address.
       The first six bytes are the station address. */
    for (i = 0; i < 6; i++)
	printk(" %2.2x", dev->dev_addr[i] = inb(ioaddr + i));

    dev->base_addr = ioaddr;
    snarf_region(ioaddr, LANCE_TOTAL_SIZE);

    /* Make certain the data structures used by the LANCE are aligned. */
    dev->priv = (void *)(((int)dev->priv + 7) & ~7);
    lp = (struct lance_private *)dev->priv;
    lp->rx_buffs = (long)dev->priv + sizeof(struct lance_private);
    lp->tx_bounce_buffs = (char (*)[PKT_BUF_SZ])
			   (lp->rx_buffs + PKT_BUF_SZ*RX_RING_SIZE);

#ifndef final_version
    /* This should never happen. */
    if ((int)(lp->rx_ring) & 0x07) {
	printk(" **ERROR** LANCE Rx and Tx rings not on even boundary.\n");
	return mem_start;
    }
#endif

    outw(88, ioaddr+LANCE_ADDR);
    lp->old_lance = (inw(ioaddr+LANCE_DATA) != 0x3003);

#if defined(notdef)
    printk(lp->old_lance ? " original LANCE (%04x)" : " PCnet-ISA LANCE (%04x)",
	   inw(ioaddr+LANCE_DATA));
#endif

    lp->init_block.mode = 0x0003;	/* Disable Rx and Tx. */
    for (i = 0; i < 6; i++)
	lp->init_block.phys_addr[i] = dev->dev_addr[i];
    lp->init_block.filter[0] = 0x00000000;
    lp->init_block.filter[1] = 0x00000000;
    lp->init_block.rx_ring = (int)lp->rx_ring | RX_RING_LEN_BITS;
    lp->init_block.tx_ring = (int)lp->tx_ring | TX_RING_LEN_BITS;

    outw(0x0001, ioaddr+LANCE_ADDR);
    outw((short) (int) &lp->init_block, ioaddr+LANCE_DATA);
    outw(0x0002, ioaddr+LANCE_ADDR);
    outw(((int)&lp->init_block) >> 16, ioaddr+LANCE_DATA);
    outw(0x0000, ioaddr+LANCE_ADDR);

    if (hpJ2405A) {
	char dma_tbl[4] = {3, 5, 6, 7};
	char irq_tbl[8] = {3, 4, 5, 9, 10, 11, 12, 15};
	short reset_val = inw(ioaddr+LANCE_RESET);
	dev->dma = dma_tbl[(reset_val >> 2) & 3];
	dev->irq = irq_tbl[(reset_val >> 4) & 7];
	printk(" HP J2405A IRQ %d DMA %d.\n", dev->irq, dev->dma);
    } else {
	/* The DMA channel may be passed in on this parameter. */
	if (dev->mem_start & 0x07)
	    dev->dma = dev->mem_start & 0x07;
	else if (dev->dma == 0)
	    dev->dma = LANCE_DMA;

	/* To auto-IRQ we enable the initialization-done and DMA err,
	   interrupts. For now we will always get a DMA error. */
	if (dev->irq < 2) {

	    autoirq_setup(0);

	    /* Trigger an initialization just for the interrupt. */
	    outw(0x0041, ioaddr+LANCE_DATA);

	    dev->irq = autoirq_report(1);
	    if (dev->irq)
		printk(", probed IRQ %d, fixed at DMA %d.\n",
		       dev->irq, dev->dma);
	    else {
		printk(", failed to detect IRQ line.\n");
		return mem_start;
	    }
	} else
	    printk(" assigned IRQ %d DMA %d.\n", dev->irq, dev->dma);
    }

    if (! lp->old_lance) {
	/* Turn on auto-select of media (10baseT or BNC) so that the user
	   can watch the LEDs even if the board isn't opened. */
	outw(0x0002, ioaddr+LANCE_ADDR);
	outw(0x0002, ioaddr+LANCE_BUS_IF);
    }

    if (lance_debug > 0)
	printk(version);

    /* The LANCE-specific entries in the device structure. */
    dev->open = &lance_open;
    dev->hard_start_xmit = &lance_start_xmit;
    dev->stop = &lance_close;
    dev->get_stats = &lance_get_stats;
#ifdef HAVE_MULTICAST
    dev->set_multicast_list = &set_multicast_list;
#endif

    return mem_start;
}


static int
lance_open(struct device *dev)
{
    struct lance_private *lp = (struct lance_private *)dev->priv;
    int ioaddr = dev->base_addr;
    int i;

    if (request_irq(dev->irq, &lance_interrupt)) {
	return -EAGAIN;
    }

    if (request_dma(dev->dma)) {
	free_irq(dev->irq);
	return -EAGAIN;
    }
    irq2dev_map[dev->irq] = dev;

    /* Reset the LANCE */
    inw(ioaddr+LANCE_RESET);

    /* The DMA controller is used as a no-operation slave, "cascade mode". */
    enable_dma(dev->dma);
    set_dma_mode(dev->dma, DMA_MODE_CASCADE);

    /* Un-Reset the LANCE, needed only for the NE2100. */
    if (lp->old_lance)
	outw(0, ioaddr+LANCE_RESET);

    if (! lp->old_lance) {
	/* This is 79C960-specific: Turn on auto-select of media (AUI, BNC). */
	outw(0x0002, ioaddr+LANCE_ADDR);
	outw(0x0002, ioaddr+LANCE_BUS_IF);
    }

    if (lance_debug > 1)
	printk("%s: lance_open() irq %d dma %d tx/rx rings %#x/%#x init %#x.\n",
	       dev->name, dev->irq, dev->dma, (int) lp->tx_ring, (int) lp->rx_ring,
	       (int) &lp->init_block);

    lance_init_ring(dev);
    /* Re-initialize the LANCE, and start it when done. */
    outw(0x0001, ioaddr+LANCE_ADDR);
    outw((short) (int) &lp->init_block, ioaddr+LANCE_DATA);
    outw(0x0002, ioaddr+LANCE_ADDR);
    outw(((int)&lp->init_block) >> 16, ioaddr+LANCE_DATA);

    outw(0x0004, ioaddr+LANCE_ADDR);
    outw(0x0d15, ioaddr+LANCE_DATA);

    outw(0x0000, ioaddr+LANCE_ADDR);
    outw(0x0001, ioaddr+LANCE_DATA);

    dev->tbusy = 0;
    dev->interrupt = 0;
    dev->start = 1;
    i = 0;
    while (i++ < 100)
	if (inw(ioaddr+LANCE_DATA) & 0x0100)
	    break;
    outw(0x0142, ioaddr+LANCE_DATA);

    if (lance_debug > 2)
	printk("%s: LANCE open after %d ticks, init block %#x csr0 %4.4x.\n",
	       dev->name, i, (int) &lp->init_block, inw(ioaddr+LANCE_DATA));

    return 0;			/* Always succeed */
}

/* Initialize the LANCE Rx and Tx rings. */
static void
lance_init_ring(struct device *dev)
{
    struct lance_private *lp = (struct lance_private *)dev->priv;
    int i;

    lp->cur_rx = lp->cur_tx = 0;
    lp->dirty_rx = lp->dirty_tx = 0;

    for (i = 0; i < RX_RING_SIZE; i++) {
	lp->rx_ring[i].base = (lp->rx_buffs + i*PKT_BUF_SZ) | 0x80000000;
	lp->rx_ring[i].buf_length = -PKT_BUF_SZ;
    }
    /* The Tx buffer address is filled in as needed, but we do need to clear
       the upper ownership bit. */
    for (i = 0; i < TX_RING_SIZE; i++) {
	lp->tx_ring[i].base = 0;
    }

    lp->init_block.mode = 0x0000;
    for (i = 0; i < 6; i++)
	lp->init_block.phys_addr[i] = dev->dev_addr[i];
    lp->init_block.filter[0] = 0x00000000;
    lp->init_block.filter[1] = 0x00000000;
    lp->init_block.rx_ring = (int)lp->rx_ring | RX_RING_LEN_BITS;
    lp->init_block.tx_ring = (int)lp->tx_ring | TX_RING_LEN_BITS;
}

static int
lance_start_xmit(struct sk_buff *skb, struct device *dev)
{
    struct lance_private *lp = (struct lance_private *)dev->priv;
    int ioaddr = dev->base_addr;
    int entry;

    /* Transmitter timeout, serious problems. */
    if (dev->tbusy) {
	int tickssofar = jiffies - dev->trans_start;
	if (tickssofar < 10)
	    return 1;
	outw(0, ioaddr+LANCE_ADDR);
	printk("%s: transmit timed out, status %4.4x, resetting.\n",
	       dev->name, inw(ioaddr+LANCE_DATA));
	outw(0x0001, ioaddr+LANCE_DATA);
	lp->stats.tx_errors++;
#ifndef final_version
	{
	    int i;
	    printk(" Ring data dump: dirty_tx %d cur_tx %d cur_rx %d.",
		   lp->dirty_tx, lp->cur_tx, lp->cur_rx);
	    for (i = 0 ; i < RX_RING_SIZE; i++)
		printk("%s %08x %04x %04x", i & 0x3 ? "" : "\n ",
		       lp->rx_ring[i].base, -lp->rx_ring[i].buf_length,
		       lp->rx_ring[i].msg_length);
	    for (i = 0 ; i < TX_RING_SIZE; i++)
		printk(" %s%08x %04x %04x", i & 0x3 ? "" : "\n ",
		       lp->tx_ring[i].base, -lp->tx_ring[i].length,
		       lp->tx_ring[i].misc);
	    printk("\n");
	}
#endif
	lance_init_ring(dev);
	outw(0x0043, ioaddr+LANCE_DATA);

	dev->tbusy=0;
	dev->trans_start = jiffies;

	return 0;
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

    if (lance_debug > 3) {
	outw(0x0000, ioaddr+LANCE_ADDR);
	printk("%s: lance_start_xmit() called, csr0 %4.4x.\n", dev->name,
	       inw(ioaddr+LANCE_DATA));
	outw(0x0000, ioaddr+LANCE_DATA);
    }

    /* Block a timer-based transmit from overlapping.  This could better be
       done with atomic_swap(1, dev->tbusy), but set_bit() works as well. */
    if (set_bit(0, (void*)&dev->tbusy) != 0)
	printk("%s: Transmitter access conflict.\n", dev->name);

    /* Fill in a Tx ring entry */

    /* Mask to ring buffer boundary. */
    entry = lp->cur_tx & TX_RING_MOD_MASK;

    /* Caution: the write order is important here, set the base address
       with the "ownership" bits last. */

    /* The old LANCE chips doesn't automatically pad buffers to min. size. */
    if (lp->old_lance) {
	lp->tx_ring[entry].length =
	    -(ETH_ZLEN < skb->len ? skb->len : ETH_ZLEN);
    } else
	lp->tx_ring[entry].length = -skb->len;

    lp->tx_ring[entry].misc = 0x0000;

    /* If any part of this buffer is >16M we must copy it to a low-memory
       buffer. */
    if ((int)(skb->data) + skb->len > 0x01000000) {
	if (lance_debug > 5)
	    printk("%s: bouncing a high-memory packet (%#x).\n",
		   dev->name, (int)(skb->data));
	memcpy(&lp->tx_bounce_buffs[entry], skb->data, skb->len);
	lp->tx_ring[entry].base =
	    (int)(lp->tx_bounce_buffs + entry) | 0x83000000;
	if (skb->free)
	    kfree_skb (skb, FREE_WRITE);
    } else {
    	/* We can't free the packet yet, so we inform the memory management
	   code that we are still using it. */
    	if(skb->free==0)
    		skb_kept_by_device(skb);
	lp->tx_ring[entry].base = (int)(skb->data) | 0x83000000;
    }
    lp->cur_tx++;

    /* Trigger an immediate send poll. */
    outw(0x0000, ioaddr+LANCE_ADDR);
    outw(0x0048, ioaddr+LANCE_DATA);

    dev->trans_start = jiffies;

    if (lp->tx_ring[(entry+1) & TX_RING_MOD_MASK].base == 0)
	dev->tbusy=0;

    return 0;
}

/* The LANCE interrupt handler. */
static void
lance_interrupt(int reg_ptr)
{
    int irq = -(((struct pt_regs *)reg_ptr)->orig_eax+2);
    struct device *dev = (struct device *)(irq2dev_map[irq]);
    struct lance_private *lp;
    int csr0, ioaddr;

    if (dev == NULL) {
	printk ("lance_interrupt(): irq %d for unknown device.\n", irq);
	return;
    }

    ioaddr = dev->base_addr;
    lp = (struct lance_private *)dev->priv;
    if (dev->interrupt)
	printk("%s: Re-entering the interrupt handler.\n", dev->name);

    dev->interrupt = 1;

    outw(0x00, dev->base_addr + LANCE_ADDR);
    csr0 = inw(dev->base_addr + LANCE_DATA);

    /* Acknowledge all of the current interrupt sources ASAP. */
    outw(csr0 & ~0x004f, dev->base_addr + LANCE_DATA);

    if (lance_debug > 5)
	printk("%s: interrupt  csr0=%#2.2x new csr=%#2.2x.\n",
	       dev->name, csr0, inw(dev->base_addr + LANCE_DATA));

    if (csr0 & 0x0400)		/* Rx interrupt */
	lance_rx(dev);

    if (csr0 & 0x0200) {	/* Tx-done interrupt */
	int dirty_tx = lp->dirty_tx;

	while (dirty_tx < lp->cur_tx) {
	    int entry = dirty_tx & TX_RING_MOD_MASK;
	    int status = lp->tx_ring[entry].base;
	    void *databuff;
	    
	    if (status < 0)
		break;		/* It still hasn't been Txed */

	    lp->tx_ring[entry].base = 0;
	    databuff = (void*)(status & 0x00ffffff);

	    if (status & 0x40000000) { /* There was an major error, log it. */
		int err_status = lp->tx_ring[entry].misc;
		lp->stats.tx_errors++;
		if (err_status & 0x0400) lp->stats.tx_aborted_errors++;
		if (err_status & 0x0800) lp->stats.tx_carrier_errors++;
		if (err_status & 0x1000) lp->stats.tx_window_errors++;
		if (err_status & 0x4000) lp->stats.tx_fifo_errors++;
		/* Perhaps we should re-init() after the FIFO error. */
	    } else {
		if (status & 0x18000000)
		    lp->stats.collisions++;
		lp->stats.tx_packets++;
	    }

	    /* We don't free the skb if it's a data-only copy in the bounce
	       buffer.  The address checks here are sorted -- the first test
	       should always work.  */
	    if (databuff >= (void*)(&lp->tx_bounce_buffs[TX_RING_SIZE])
		|| databuff < (void*)(lp->tx_bounce_buffs)) {
		struct sk_buff *skb = ((struct sk_buff *)databuff) - 1;
		if (skb->free)
		    kfree_skb(skb, FREE_WRITE);
		else
		    skb_device_release(skb,FREE_WRITE);
		/* Warning: skb may well vanish at the point you call
		   device_release! */
	    }
	    dirty_tx++;
	}

#ifndef final_version
	if (lp->cur_tx - dirty_tx >= TX_RING_SIZE) {
	    printk("out-of-sync dirty pointer, %d vs. %d.\n",
		   dirty_tx, lp->cur_tx);
	    dirty_tx += TX_RING_SIZE;
	}
#endif

	if (dev->tbusy  &&  dirty_tx > lp->cur_tx - TX_RING_SIZE + 2) {
	    /* The ring is no longer full, clear tbusy. */
	    dev->tbusy = 0;
	    mark_bh(INET_BH);
	}

	lp->dirty_tx = dirty_tx;
    }

    if (csr0 & 0x8000) {
	if (csr0 & 0x4000) lp->stats.tx_errors++;
	if (csr0 & 0x1000) lp->stats.rx_errors++;
    }

    /* Clear the interrupts we've handled. */
    outw(0x0000, dev->base_addr + LANCE_ADDR);
    outw(0x7f40, dev->base_addr + LANCE_DATA);

    if (lance_debug > 4)
	printk("%s: exiting interrupt, csr%d=%#4.4x.\n",
	       dev->name, inw(ioaddr + LANCE_ADDR),
	       inw(dev->base_addr + LANCE_DATA));

    dev->interrupt = 0;
    return;
}

static int
lance_rx(struct device *dev)
{
    struct lance_private *lp = (struct lance_private *)dev->priv;
    int entry = lp->cur_rx & RX_RING_MOD_MASK;
	
    /* If we own the next entry, it's a new packet. Send it up. */
    while (lp->rx_ring[entry].base >= 0) {
	int status = lp->rx_ring[entry].base >> 24;

	if (status != 0x03) {		/* There was an error. */
	    /* There is an tricky error noted by John Murphy,
	       <murf@perftech.com> to Russ Nelson: Even with full-sized
	       buffers it's possible for a jabber packet to use two
	       buffers, with only the last correctly noting the error. */
	    if (status & 0x01)	/* Only count a general error at the */
		lp->stats.rx_errors++; /* end of a packet.*/
	    if (status & 0x20) lp->stats.rx_frame_errors++;
	    if (status & 0x10) lp->stats.rx_over_errors++;
	    if (status & 0x08) lp->stats.rx_crc_errors++;
	    if (status & 0x04) lp->stats.rx_fifo_errors++;
	} else {
	    /* Malloc up new buffer, compatible with net-2e. */
	    short pkt_len = lp->rx_ring[entry].msg_length;
	    int sksize = sizeof(struct sk_buff) + pkt_len;
	    struct sk_buff *skb;

	    skb = alloc_skb(sksize, GFP_ATOMIC);
	    if (skb == NULL) {
		printk("%s: Memory squeeze, deferring packet.\n", dev->name);
		lp->stats.rx_dropped++;	/* Really, deferred. */
		break;
	    }
	    skb->mem_len = sksize;
	    skb->mem_addr = skb;
	    skb->len = pkt_len;
	    skb->dev = dev;
	    memcpy(skb->data,
		   (unsigned char *)(lp->rx_ring[entry].base & 0x00ffffff),
		   pkt_len);
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

	lp->rx_ring[entry].base |= 0x80000000;
	entry = (++lp->cur_rx) & RX_RING_MOD_MASK;
    }

    /* We should check that at least two ring entries are free.  If not,
       we should free one and mark stats->rx_dropped++. */

    return 0;
}

static int
lance_close(struct device *dev)
{
    int ioaddr = dev->base_addr;
    struct lance_private *lp = (struct lance_private *)dev->priv;

    dev->start = 0;
    dev->tbusy = 1;

    outw(112, ioaddr+LANCE_ADDR);
    lp->stats.rx_missed_errors = inw(ioaddr+LANCE_DATA);

    outw(0, ioaddr+LANCE_ADDR);

    if (lance_debug > 1)
	printk("%s: Shutting down ethercard, status was %2.2x.\n",
	       dev->name, inw(ioaddr+LANCE_DATA));

    /* We stop the LANCE here -- it occasionally polls
       memory if we don't. */
    outw(0x0004, ioaddr+LANCE_DATA);

    disable_dma(dev->dma);

    free_irq(dev->irq);
    free_dma(dev->dma);

    irq2dev_map[dev->irq] = 0;

    return 0;
}

static struct enet_statistics *
lance_get_stats(struct device *dev)
{
    struct lance_private *lp = (struct lance_private *)dev->priv;
    short ioaddr = dev->base_addr;
    short saved_addr;

    cli();
    saved_addr = inw(ioaddr+LANCE_ADDR);
    outw(112, ioaddr+LANCE_ADDR);
    lp->stats.rx_missed_errors = inw(ioaddr+LANCE_DATA);
    outw(saved_addr, ioaddr+LANCE_ADDR);
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

    /* We take the simple way out and always enable promiscuous mode. */
    outw(0, ioaddr+LANCE_ADDR);
    outw(0x0004, ioaddr+LANCE_DATA); /* Temporarily stop the lance.  */

    outw(15, ioaddr+LANCE_ADDR);
    if (num_addrs >= 0) {
	short multicast_table[4];
	int i;
	/* We don't use the multicast table, but rely on upper-layer filtering. */
	memset(multicast_table, (num_addrs == 0) ? 0 : -1, sizeof(multicast_table));
	for (i = 0; i < 4; i++) {
	    outw(8 + i, ioaddr+LANCE_ADDR);
	    outw(multicast_table[i], ioaddr+LANCE_DATA);
	}
	outw(0x0000, ioaddr+LANCE_DATA); /* Unset promiscuous mode */
    } else {
	outw(0x8000, ioaddr+LANCE_DATA); /* Set promiscuous mode */
    }

    outw(0, ioaddr+LANCE_ADDR);
    outw(0x0142, ioaddr+LANCE_DATA); /* Resume normal operation. */
}
#endif

#ifdef HAVE_DEVLIST
static unsigned int lance_portlist[] = {0x300, 0x320, 0x340, 0x360, 0};
struct netdev_entry lance_drv =
{"lance", lance_probe1, LANCE_TOTAL_SIZE, lance_portlist};
#endif

/*
 * Local variables:
 *  compile-command: "gcc -D__KERNEL__ -I/usr/src/linux/net/inet -Wall -Wstrict-prototypes -O6 -m486 -c lance.c"
 * End:
 */
