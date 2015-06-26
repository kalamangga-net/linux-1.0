/* hp.c: A HP LAN ethernet driver for linux. */
/*
	Written 1993 by Donald Becker.
	Copyright 1993 United States Government as represented by the
	Director, National Security Agency.	 This software may be used and
	distributed according to the terms of the GNU Public License,
	incorporated herein by reference.

	This is a driver for the HP LAN adaptors.

	The Author may be reached as becker@super.org or
	C/O Supercomputing Research Ctr., 17100 Science Dr., Bowie MD 20715
*/

static char *version =
	"hp.c:v0.99.15k 3/3/94 Donald Becker (becker@super.org)\n";

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <asm/system.h>
#include <asm/io.h>

#include "dev.h"
#include "8390.h"

#ifndef HAVE_PORTRESERVE
#define check_region(ioaddr, size)				0
#define snarf_region(ioaddr, size);				do ; while (0)
#endif

#define HP_IO_EXTENT	32

#define HP_DATAPORT		0x0c	/* "Remote DMA" data port. */
#define HP_ID			0x07
#define HP_CONFIGURE	0x08	/* Configuration register. */
#define	 HP_RUN			0x01	/* 1 == Run, 0 == reset. */
#define	 HP_IRQ			0x0E	/* Mask for software-configured IRQ line. */
#define	 HP_DATAON		0x10	/* Turn on dataport */
#define NIC_OFFSET		0x10	/* Offset the 8390 registers. */

#define HP_START_PG		0x00	/* First page of TX buffer */
#define HP_8BSTOP_PG	0x80	/* Last page +1 of RX ring */
#define HP_16BSTOP_PG	0xFF	/* Same, for 16 bit cards. */

int hp_probe(struct device *dev);
int hpprobe1(struct device *dev, int ioaddr);

static void hp_reset_8390(struct device *dev);
static int hp_block_input(struct device *dev, int count,
						  char *buf, int ring_offset);
static void hp_block_output(struct device *dev, int count,
							const unsigned char *buf, const start_page);
static void hp_init_card(struct device *dev);

/* The map from IRQ number to HP_CONFIGURE register setting. */
/* My default is IRQ5	   0  1	 2  3  4  5  6	7  8  9 10 11 */
static char irqmap[16] = { 0, 0, 4, 6, 8,10, 0,14, 0, 4, 2,12,0,0,0,0};


/*	Probe for an HP LAN adaptor.
	Also initialize the card and fill in STATION_ADDR with the station
	address. */

int hp_probe(struct device *dev)
{
	int *port, ports[] = {0x300, 0x320, 0x340, 0x280, 0x2C0, 0x200, 0x240, 0};
	short ioaddr = dev->base_addr;

	if (ioaddr > 0x1ff)			/* Check a single specified location. */
		return hpprobe1(dev, ioaddr);
	else if (ioaddr > 0)				/* Don't probe at all. */
		return ENXIO;

	for (port = &ports[0]; *port; port++) {
		if (check_region(*port, HP_IO_EXTENT))
			continue;
		if (hpprobe1(dev, *port) == 0) {
			return 0;
		}
	}
	return ENODEV;
}

int hpprobe1(struct device *dev, int ioaddr)
{
	int i, board_id, wordmode;
	char *name;
	unsigned char *station_addr = dev->dev_addr;

	/* Check for the HP physical address, 08 00 09 xx xx xx. */
	/* This really isn't good enough: we may pick up HP LANCE boards
	   also!  Avoid the lance 0x5757 signature. */
	if (inb(ioaddr) != 0x08
		|| inb(ioaddr+1) != 0x00
		|| inb(ioaddr+2) != 0x09
		|| inb(ioaddr+14) == 0x57)
		return ENODEV;

	/* Set up the parameters based on the board ID.
	   If you have additional mappings, please mail them to becker@super.org. */
	if ((board_id = inb(ioaddr + HP_ID)) & 0x80) {
		name = "HP27247";
		wordmode = 1;
	} else {
		name = "HP27250";
		wordmode = 0;
	}

	/* Grab the region so we can find another board if something fails. */
	snarf_region(ioaddr, HP_IO_EXTENT);

	printk("%s: %s (ID %02x) at %#3x,", dev->name, name, board_id, ioaddr);

	for(i = 0; i < ETHER_ADDR_LEN; i++)
		printk(" %2.2x", station_addr[i] = inb(ioaddr + i));

	/* Snarf the interrupt now.  Someday this could be moved to open(). */
	if (dev->irq < 2) {
		int irq_16list[] = { 11, 10, 5, 3, 4, 7, 9, 0};
		int irq_8list[] = { 7, 5, 3, 4, 9, 0};
		int *irqp = wordmode ? irq_16list : irq_8list;
		do {
			int irq = *irqp;
			if (request_irq (irq, NULL) != -EBUSY) {
				autoirq_setup(0);
				/* Twinkle the interrupt, and check if it's seen. */
				outb_p(irqmap[irq] | HP_RUN, ioaddr + HP_CONFIGURE);
				outb_p( 0x00 | HP_RUN, ioaddr + HP_CONFIGURE);
				if (irq == autoirq_report(0)		 /* It's a good IRQ line! */
					&& request_irq (irq, &ei_interrupt) == 0) {
					printk(" selecting IRQ %d.\n", irq);
					dev->irq = *irqp;
					break;
				}
			}
		} while (*++irqp);
		if (*irqp == 0) {
			printk(" no free IRQ lines.\n");
			return EBUSY;
		}
	} else {
		if (dev->irq == 2)
			dev->irq = 9;
		if (irqaction(dev->irq, &ei_sigaction)) {
			printk (" unable to get IRQ %d.\n", dev->irq);
			return EBUSY;
		}
	}

	if (ei_debug > 1)
		printk(version);

	/* Set the base address to point to the NIC, not the "real" base! */
	dev->base_addr = ioaddr + NIC_OFFSET;

	ethdev_init(dev);

	ei_status.name = name;
	ei_status.word16 = wordmode;
	ei_status.tx_start_page = HP_START_PG;
	ei_status.rx_start_page = HP_START_PG + TX_PAGES;
	ei_status.stop_page = wordmode ? HP_16BSTOP_PG : HP_8BSTOP_PG;

	ei_status.reset_8390 = &hp_reset_8390;
	ei_status.block_input = &hp_block_input;
	ei_status.block_output = &hp_block_output;
	hp_init_card(dev);

	return 0;
}

static void
hp_reset_8390(struct device *dev)
{
	int hp_base = dev->base_addr - NIC_OFFSET;
	int saved_config = inb_p(hp_base + HP_CONFIGURE);

	if (ei_debug > 1) printk("resetting the 8390 time=%ld...", jiffies);
	outb_p(0x00, hp_base + HP_CONFIGURE);
	ei_status.txing = 0;
	/* Pause just a few cycles for the hardware reset to take place. */
	SLOW_DOWN_IO;
	SLOW_DOWN_IO;

	outb_p(saved_config, hp_base + HP_CONFIGURE);
	SLOW_DOWN_IO; SLOW_DOWN_IO;
	
	if ((inb_p(hp_base+NIC_OFFSET+EN0_ISR) & ENISR_RESET) == 0)
		printk("%s: hp_reset_8390() did not complete.\n", dev->name);

	if (ei_debug > 1) printk("8390 reset done (%ld).", jiffies);
	return;
}

/* Block input and output, similar to the Crynwr packet driver.	 If you
   porting to a new ethercard look at the packet driver source for hints.
   The HP LAN doesn't use shared memory -- we put the packet
   out through the "remote DMA" dataport. */

static int
hp_block_input(struct device *dev, int count, char *buf, int ring_offset)
{
	int nic_base = dev->base_addr;
	int saved_config = inb_p(nic_base - NIC_OFFSET + HP_CONFIGURE);
	int xfer_count = count;

	outb_p(saved_config | HP_DATAON, nic_base - NIC_OFFSET + HP_CONFIGURE);
	outb_p(E8390_NODMA+E8390_PAGE0+E8390_START, nic_base);
	outb_p(count & 0xff, nic_base + EN0_RCNTLO);
	outb_p(count >> 8, nic_base + EN0_RCNTHI);
	outb_p(ring_offset & 0xff, nic_base + EN0_RSARLO);
	outb_p(ring_offset >> 8, nic_base + EN0_RSARHI);
	outb_p(E8390_RREAD+E8390_START, nic_base);
	if (ei_status.word16) {
	  insw(nic_base - NIC_OFFSET + HP_DATAPORT,buf,count>>1);
	  if (count & 0x01)
		buf[count-1] = inb(nic_base - NIC_OFFSET + HP_DATAPORT), xfer_count++;
	} else {
		insb(nic_base - NIC_OFFSET + HP_DATAPORT, buf, count);
	}
	/* This is for the ALPHA version only, remove for later releases. */
	if (ei_debug > 0) {			/* DMA termination address check... */
	  int high = inb_p(nic_base + EN0_RSARHI);
	  int low = inb_p(nic_base + EN0_RSARLO);
	  int addr = (high << 8) + low;
	  /* Check only the lower 8 bits so we can ignore ring wrap. */
	  if (((ring_offset + xfer_count) & 0xff) != (addr & 0xff))
		printk("%s: RX transfer address mismatch, %#4.4x vs. %#4.4x (actual).\n",
			   dev->name, ring_offset + xfer_count, addr);
	}
	outb_p(saved_config & (~HP_DATAON), nic_base - NIC_OFFSET + HP_CONFIGURE);
	return ring_offset + count;
}

static void
hp_block_output(struct device *dev, int count,
				const unsigned char *buf, const start_page)
{
	int nic_base = dev->base_addr;
	int saved_config = inb_p(nic_base - NIC_OFFSET + HP_CONFIGURE);

	outb_p(saved_config | HP_DATAON, nic_base - NIC_OFFSET + HP_CONFIGURE);
	/* Round the count up for word writes.	Do we need to do this?
	   What effect will an odd byte count have on the 8390?
	   I should check someday. */
	if (ei_status.word16 && (count & 0x01))
	  count++;
	/* We should already be in page 0, but to be safe... */
	outb_p(E8390_PAGE0+E8390_START+E8390_NODMA, nic_base);

#ifdef ei8390_bug
	/* Handle the read-before-write bug the same way as the
	   Crynwr packet driver -- the NatSemi method doesn't work. */
	outb_p(0x42, nic_base + EN0_RCNTLO);
	outb_p(0,	nic_base + EN0_RCNTHI);
	outb_p(0xff, nic_base + EN0_RSARLO);
	outb_p(0x00, nic_base + EN0_RSARHI);
	outb_p(E8390_RREAD+E8390_START, EN_CMD);
	/* Make certain that the dummy read has occured. */
	inb_p(0x61);
	inb_p(0x61);
#endif

	outb_p(count & 0xff, nic_base + EN0_RCNTLO);
	outb_p(count >> 8,	 nic_base + EN0_RCNTHI);
	outb_p(0x00, nic_base + EN0_RSARLO);
	outb_p(start_page, nic_base + EN0_RSARHI);

	outb_p(E8390_RWRITE+E8390_START, nic_base);
	if (ei_status.word16) {
		/* Use the 'rep' sequence for 16 bit boards. */
		outsw(nic_base - NIC_OFFSET + HP_DATAPORT, buf, count>>1);
	} else {
		outsb(nic_base - NIC_OFFSET + HP_DATAPORT, buf, count);
	}

	/* DON'T check for 'inb_p(EN0_ISR) & ENISR_RDC' here -- it's broken! */

	/* This is for the ALPHA version only, remove for later releases. */
	if (ei_debug > 0) {			/* DMA termination address check... */
	  int high = inb_p(nic_base + EN0_RSARHI);
	  int low  = inb_p(nic_base + EN0_RSARLO);
	  int addr = (high << 8) + low;
	  if ((start_page << 8) + count != addr)
		printk("%s: TX Transfer address mismatch, %#4.4x vs. %#4.4x.\n",
			   dev->name, (start_page << 8) + count, addr);
	}
	outb_p(saved_config & (~HP_DATAON), nic_base - NIC_OFFSET + HP_CONFIGURE);
	return;
}

/* This function resets the ethercard if something screws up. */
static void
hp_init_card(struct device *dev)
{
	int irq = dev->irq;
	NS8390_init(dev, 0);
	outb_p(irqmap[irq&0x0f] | HP_RUN,
		   dev->base_addr - NIC_OFFSET + HP_CONFIGURE);
	return;
}


/*
 * Local variables:
 * compile-command: "gcc -D__KERNEL__ -I/usr/src/linux/net/inet -Wall -Wstrict-prototypes -O6 -m486 -c hp.c"
 * version-control: t
 * kept-new-versions: 5
 * tab-width: 4
 * c-indent-level: 4
 * End:
 */
