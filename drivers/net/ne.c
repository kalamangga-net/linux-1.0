/* ne.c: A general non-shared-memory NS8390 ethernet driver for linux. */
/*
    Written 1992,1993 by Donald Becker.

    Copyright 1993 United States Government as represented by the
    Director, National Security Agency.  This software may be used and
    distributed according to the terms of the GNU Public License,
    incorporated herein by reference.

    This driver should work with many 8390-based ethernet boards.  Currently
    it support the NE1000, NE2000, clones, and some Cabletron products.

    The Author may be reached as becker@super.org or
    C/O Supercomputing Research Ctr., 17100 Science Dr., Bowie MD 20715
*/

/* Routines for the NatSemi-based designs (NE[12]000). */

static char *version =
    "ne.c:v0.99-15k 3/3/94 Donald Becker (becker@super.org)\n";

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <asm/system.h>
#include <asm/io.h>

#include "dev.h"
#include "8390.h"

#define NE_BASE	 (dev->base_addr)
#define NE_CMD	 	0x00
#define NE_DATAPORT	0x10	/* NatSemi-defined port window offset. */
#define NE_RESET	0x1f	/* Issue a read to reset, a write to clear. */

#define NE1SM_START_PG	0x20	/* First page of TX buffer */
#define NE1SM_STOP_PG 	0x40	/* Last page +1 of RX ring */
#define NESM_START_PG	0x40	/* First page of TX buffer */
#define NESM_STOP_PG	0x80	/* Last page +1 of RX ring */

int ne_probe(struct device *dev);
static int neprobe1(int ioaddr, struct device *dev, int verbose);

static void ne_reset_8390(struct device *dev);
static int ne_block_input(struct device *dev, int count,
			  char *buf, int ring_offset);
static void ne_block_output(struct device *dev, const int count,
		const unsigned char *buf, const int start_page);


/*  Probe for various non-shared-memory ethercards.

   NEx000-clone boards have a Station Address PROM (SAPROM) in the packet
   buffer memory space.  NE2000 clones have 0x57,0x57 in bytes 0x0e,0x0f of
   the SAPROM, while other supposed NE2000 clones must be detected by their
   SA prefix.

   Reading the SAPROM from a word-wide card with the 8390 set in byte-wide
   mode results in doubled values, which can be detected and compansated for.

   The probe is also responsible for initializing the card and filling
   in the 'dev' and 'ei_status' structures.

   We use the minimum memory size for some ethercard product lines, iff we can't
   distinguish models.  You can increase the packet buffer size by setting
   PACKETBUF_MEMSIZE.  Reported Cabletron packet buffer locations are:
	E1010   starts at 0x100 and ends at 0x2000.
	E1010-x starts at 0x100 and ends at 0x8000. ("-x" means "more memory")
	E2010	 starts at 0x100 and ends at 0x4000.
	E2010-x starts at 0x100 and ends at 0xffff.  */

int ne_probe(struct device *dev)
{
    int *port, ports[] = {0x300, 0x280, 0x320, 0x340, 0x360, 0};
    short ioaddr = dev->base_addr;

    if (ioaddr < 0)
	return ENXIO;		/* Don't probe at all. */
    if (ioaddr > 0x100)
	return ! neprobe1(ioaddr, dev, 1);

    for (port = &ports[0]; *port; port++) {
#ifdef HAVE_PORTRESERVE
	if (check_region(*port, 32))
	    continue;
#endif
	if (inb_p(*port) != 0xff && neprobe1(*port, dev, 0)) {
	    dev->base_addr = *port;
	    return 0;
	}
    }
    dev->base_addr = ioaddr;
    return ENODEV;
}

static int neprobe1(int ioaddr, struct device *dev, int verbose)
{
    int i;
    unsigned char SA_prom[32];
    int wordlength = 2;
    char *name;
    int start_page, stop_page;
    int neX000, ctron, dlink, dfi;
    int reg0 = inb(ioaddr);

    if ( reg0 == 0xFF)
	return 0;

    /* Do a quick preliminary check that we have a 8390. */
    {	int regd;
	outb_p(E8390_NODMA+E8390_PAGE1+E8390_STOP, ioaddr + E8390_CMD);
	regd = inb_p(ioaddr + 0x0d);
	outb_p(0xff, ioaddr + 0x0d);
	outb_p(E8390_NODMA+E8390_PAGE0, ioaddr + E8390_CMD);
	inb_p(ioaddr + EN0_COUNTER0); /* Clear the counter by reading. */
	if (inb_p(ioaddr + EN0_COUNTER0) != 0) {
	    outb_p(reg0, ioaddr);
	    outb(regd, ioaddr + 0x0d);	/* Restore the old values. */
	    return 0;
	}
    }

    printk("NE*000 ethercard probe at %#3x:", ioaddr);

    /* Read the 16 bytes of station address prom, returning 1 for
       an eight-bit interface and 2 for a 16-bit interface.
       We must first initialize registers, similar to NS8390_init(eifdev, 0).
       We can't reliably read the SAPROM address without this.
       (I learned the hard way!). */
    {
	struct {unsigned char value, offset; } program_seq[] = {
	    {E8390_NODMA+E8390_PAGE0+E8390_STOP, E8390_CMD}, /* Select page 0*/
	    {0x48,	EN0_DCFG},	/* Set byte-wide (0x48) access. */
	    {0x00,	EN0_RCNTLO},	/* Clear the count regs. */
	    {0x00,	EN0_RCNTHI},
	    {0x00,	EN0_IMR},	/* Mask completion irq. */
	    {0xFF,	EN0_ISR},
	    {E8390_RXOFF, EN0_RXCR},	/* 0x20  Set to monitor */
	    {E8390_TXOFF, EN0_TXCR},	/* 0x02  and loopback mode. */
	    {32,	EN0_RCNTLO},
	    {0x00,	EN0_RCNTHI},
	    {0x00,	EN0_RSARLO},	/* DMA starting at 0x0000. */
	    {0x00,	EN0_RSARHI},
	    {E8390_RREAD+E8390_START, E8390_CMD},
	};
	for (i = 0; i < sizeof(program_seq)/sizeof(program_seq[0]); i++)
	    outb_p(program_seq[i].value, ioaddr + program_seq[i].offset);
    }
    for(i = 0; i < 32 /*sizeof(SA_prom)*/; i+=2) {
	SA_prom[i] = inb(ioaddr + NE_DATAPORT);
	SA_prom[i+1] = inb(ioaddr + NE_DATAPORT);
	if (SA_prom[i] != SA_prom[i+1])
	    wordlength = 1;
    }

    if (wordlength == 2) {
	/* We must set the 8390 for word mode. */
	outb_p(0x49, ioaddr + EN0_DCFG);
	/* We used to reset the ethercard here, but it doesn't seem
	   to be necessary. */
	/* Un-double the SA_prom values. */
	for (i = 0; i < 16; i++)
	    SA_prom[i] = SA_prom[i+i];
    }

#if defined(show_all_SAPROM)
    /* If your ethercard isn't detected define this to see the SA_PROM. */
    for(i = 0; i < sizeof(SA_prom); i++)
	printk(" %2.2x", SA_prom[i]);
#else
    for(i = 0; i < ETHER_ADDR_LEN; i++) {
	dev->dev_addr[i] = SA_prom[i];
	printk(" %2.2x", SA_prom[i]);
    }
#endif

    neX000 = (SA_prom[14] == 0x57  &&  SA_prom[15] == 0x57);
    ctron =  (SA_prom[0] == 0x00 && SA_prom[1] == 0x00 && SA_prom[2] == 0x1d);
    dlink =  (SA_prom[0] == 0x00 && SA_prom[1] == 0xDE && SA_prom[2] == 0x01);
    dfi   =  (SA_prom[0] == 'D' && SA_prom[1] == 'F' && SA_prom[2] == 'I');

    /* Set up the rest of the parameters. */
    if (neX000 || dlink || dfi) {
	if (wordlength == 2) {
	    name = dlink ? "DE200" : "NE2000";
	    start_page = NESM_START_PG;
	    stop_page = NESM_STOP_PG;
	} else {
	    name = dlink ? "DE100" : "NE1000";
	    start_page = NE1SM_START_PG;
	    stop_page = NE1SM_STOP_PG;
	}
    } else if (ctron) {
	name = "Cabletron";
	start_page = 0x01;
	stop_page = (wordlength == 2) ? 0x40 : 0x20;
    } else {
	printk(" not found.\n");
	return 0;
    }

    if (dev->irq < 2) {
	autoirq_setup(0);
	outb_p(0x50, ioaddr + EN0_IMR);	/* Enable one interrupt. */
	outb_p(0x00, ioaddr + EN0_RCNTLO);
	outb_p(0x00, ioaddr + EN0_RCNTHI);
	outb_p(E8390_RREAD+E8390_START, ioaddr); /* Trigger it... */
	outb_p(0x00, ioaddr + EN0_IMR); 		/* Mask it again. */
	dev->irq = autoirq_report(0);
	if (ei_debug > 2)
	    printk(" autoirq is %d", dev->irq);
    } else if (dev->irq == 2)
	/* Fixup for users that don't know that IRQ 2 is really IRQ 9,
	   or don't know which one to set. */
	dev->irq = 9;
    
    /* Snarf the interrupt now.  There's no point in waiting since we cannot
       share and the board will usually be enabled. */
    {
	int irqval = irqaction (dev->irq, &ei_sigaction);
	if (irqval) {
	    printk (" unable to get IRQ %d (irqval=%d).\n", dev->irq, irqval);
	    return 0;
	}
    }

    dev->base_addr = ioaddr;

#ifdef HAVE_PORTRESERVE
    snarf_region(ioaddr, 32);
#endif

    ethdev_init(dev);
    printk("\n%s: %s found at %#x, using IRQ %d.\n",
	   dev->name, name, ioaddr, dev->irq);

    if (ei_debug > 0)
	printk(version);

    ei_status.name = name;
    ei_status.tx_start_page = start_page;
    ei_status.stop_page = stop_page;
    ei_status.word16 = (wordlength == 2);

    ei_status.rx_start_page = start_page + TX_PAGES;
#ifdef PACKETBUF_MEMSIZE
    /* Allow the packet buffer size to be overridden by know-it-alls. */
    ei_status.stop_page = ei_status.tx_start_page + PACKETBUF_MEMSIZE;
#endif

    ei_status.reset_8390 = &ne_reset_8390;
    ei_status.block_input = &ne_block_input;
    ei_status.block_output = &ne_block_output;
    NS8390_init(dev, 0);
    return dev->base_addr;
}

/* Hard reset the card.  This used to pause for the same period that a
   8390 reset command required, but that shouldn't be necessary. */
static void
ne_reset_8390(struct device *dev)
{
    int tmp = inb_p(NE_BASE + NE_RESET);
    int reset_start_time = jiffies;

    if (ei_debug > 1) printk("resetting the 8390 t=%ld...", jiffies);
    ei_status.txing = 0;

    outb_p(tmp, NE_BASE + NE_RESET);
    /* This check _should_not_ be necessary, omit eventually. */
    while ((inb_p(NE_BASE+EN0_ISR) & ENISR_RESET) == 0)
	if (jiffies - reset_start_time > 2) {
	    printk("%s: ne_reset_8390() did not complete.\n", dev->name);
	    break;
	}
}

/* Block input and output, similar to the Crynwr packet driver.  If you
   porting to a new ethercard look at the packet driver source for hints.
   The NEx000 doesn't share it on-board packet memory -- you have to put
   the packet out through the "remote DMA" dataport using outb. */

static int
ne_block_input(struct device *dev, int count, char *buf, int ring_offset)
{
    int xfer_count = count;
    int nic_base = dev->base_addr;

    if (ei_status.dmaing) {
	if (ei_debug > 0)
	    printk("%s: DMAing conflict in ne_block_input."
		   "[DMAstat:%1x][irqlock:%1x]\n",
		   dev->name, ei_status.dmaing, ei_status.irqlock);
	return 0;
    }
    ei_status.dmaing |= 0x01;
    outb_p(E8390_NODMA+E8390_PAGE0+E8390_START, nic_base+ NE_CMD);
    outb_p(count & 0xff, nic_base + EN0_RCNTLO);
    outb_p(count >> 8, nic_base + EN0_RCNTHI);
    outb_p(ring_offset & 0xff, nic_base + EN0_RSARLO);
    outb_p(ring_offset >> 8, nic_base + EN0_RSARHI);
    outb_p(E8390_RREAD+E8390_START, nic_base + NE_CMD);
    if (ei_status.word16) {
      insw(NE_BASE + NE_DATAPORT,buf,count>>1);
      if (count & 0x01)
	buf[count-1] = inb(NE_BASE + NE_DATAPORT), xfer_count++;
    } else {
	insb(NE_BASE + NE_DATAPORT, buf, count);
    }

    /* This was for the ALPHA version only, but enough people have
       encountering problems that it is still here.  If you see
       this message you either 1) have an slightly imcompatible clone
       or 2) have noise/speed problems with your bus. */
    if (ei_debug > 1) {		/* DMA termination address check... */
	int addr, tries = 20;
	do {
	    /* DON'T check for 'inb_p(EN0_ISR) & ENISR_RDC' here
	       -- it's broken! Check the "DMA" address instead. */
	    int high = inb_p(nic_base + EN0_RSARHI);
	    int low = inb_p(nic_base + EN0_RSARLO);
	    addr = (high << 8) + low;
	    if (((ring_offset + xfer_count) & 0xff) == low)
		break;
	} while (--tries > 0);
	if (tries <= 0)
	    printk("%s: RX transfer address mismatch,"
		   "%#4.4x (expected) vs. %#4.4x (actual).\n",
		   dev->name, ring_offset + xfer_count, addr);
    }
    ei_status.dmaing &= ~0x01;
    return ring_offset + count;
}

static void
ne_block_output(struct device *dev, int count,
		const unsigned char *buf, const int start_page)
{
    int retries = 0;
    int nic_base = NE_BASE;

    /* Round the count up for word writes.  Do we need to do this?
       What effect will an odd byte count have on the 8390?
       I should check someday. */
    if (ei_status.word16 && (count & 0x01))
      count++;
    if (ei_status.dmaing) {
	if (ei_debug > 0)
	    printk("%s: DMAing conflict in ne_block_output."
		   "[DMAstat:%1x][irqlock:%1x]\n",
		   dev->name, ei_status.dmaing, ei_status.irqlock);
	return;
    }
    ei_status.dmaing |= 0x02;
    /* We should already be in page 0, but to be safe... */
    outb_p(E8390_PAGE0+E8390_START+E8390_NODMA, nic_base + NE_CMD);

 retry:
#if defined(rw_bugfix)
    /* Handle the read-before-write bug the same way as the
       Crynwr packet driver -- the NatSemi method doesn't work.
       Actually this doesn't aways work either, but if you have
       problems with your NEx000 this is better than nothing! */
    outb_p(0x42, nic_base + EN0_RCNTLO);
    outb_p(0x00,   nic_base + EN0_RCNTHI);
    outb_p(0x42, nic_base + EN0_RSARLO);
    outb_p(0x00, nic_base + EN0_RSARHI);
    outb_p(E8390_RREAD+E8390_START, nic_base + NE_CMD);
    /* Make certain that the dummy read has occured. */
    SLOW_DOWN_IO;
    SLOW_DOWN_IO;
    SLOW_DOWN_IO;
#endif  /* rw_bugfix */

    /* Now the normal output. */
    outb_p(count & 0xff, nic_base + EN0_RCNTLO);
    outb_p(count >> 8,   nic_base + EN0_RCNTHI);
    outb_p(0x00, nic_base + EN0_RSARLO);
    outb_p(start_page, nic_base + EN0_RSARHI);

    outb_p(E8390_RWRITE+E8390_START, nic_base + NE_CMD);
    if (ei_status.word16) {
	outsw(NE_BASE + NE_DATAPORT, buf, count>>1);
    } else {
	outsb(NE_BASE + NE_DATAPORT, buf, count);
    }

    /* This was for the ALPHA version only, but enough people have
       encountering problems that it is still here. */
    if (ei_debug > 1) {		/* DMA termination address check... */
	int addr, tries = 20;
	do {
	    /* DON'T check for 'inb_p(EN0_ISR) & ENISR_RDC' here
	       -- it's broken! Check the "DMA" address instead. */
	    int high = inb_p(nic_base + EN0_RSARHI);
	    int low = inb_p(nic_base + EN0_RSARLO);
	    addr = (high << 8) + low;
	    if ((start_page << 8) + count == addr)
		break;
	} while (--tries > 0);
	if (tries <= 0) {
	    printk("%s: Tx packet transfer address mismatch,"
		   "%#4.4x (expected) vs. %#4.4x (actual).\n",
		   dev->name, (start_page << 8) + count, addr);
	    if (retries++ == 0)
		goto retry;
	}
    }
    ei_status.dmaing &= ~0x02;
    return;
}


/*
 * Local variables:
 *  compile-command: "gcc -DKERNEL -Wall -O6 -fomit-frame-pointer -I/usr/src/linux/net/tcp -c ne.c"
 *  version-control: t
 *  kept-new-versions: 5
 * End:
 */
