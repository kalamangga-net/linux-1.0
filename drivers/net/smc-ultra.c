/* smc-ultra.c: A SMC Ultra ethernet driver for linux. */
/*
    Written 1993 by Donald Becker.  If released, this code will be
    Copyright 1993 United States Government as represented by the
    Director, National Security Agency.  This software may be used and
    distributed according to the terms of the GNU Public License,
    incorporated herein by reference.
    
    This is a driver for the SMC Ultra ethercard.

    The Author may be reached as becker@super.org or
    C/O Supercomputing Research Ctr., 17100 Science Dr., Bowie MD 20715

*/

static char *version =
    "smc-ultra.c:v0.07 3/1/94 Donald Becker (becker@super.org)\n";

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <asm/io.h>
#include <asm/system.h>

#include "dev.h"
#include "8390.h"

/* Compatibility definitions for earlier kernel versions. */
#ifndef HAVE_PORTRESERVE
#define check_region(ioaddr, size)              0
#define snarf_region(ioaddr, size);             do ; while (0)
#endif

int ultraprobe(int ioaddr, struct device *dev);
int ultraprobe1(int ioaddr, struct device *dev);

static int ultra_open(struct device *dev);
static void ultra_reset_8390(struct device *dev);
static int ultra_block_input(struct device *dev, int count,
			  char *buf, int ring_offset);
static void ultra_block_output(struct device *dev, int count,
			    const unsigned char *buf, const start_page);
static int ultra_close_card(struct device *dev);


#define START_PG	0x00	/* First page of TX buffer */

#define ULTRA_CMDREG	0	/* Offset to ASIC command register. */
#define  ULTRA_RESET	0x80	/* Board reset, in ULTRA_CMDREG. */
#define  ULTRA_MEMENB	0x40	/* Enable the shared memory. */
#define ULTRA_NIC_OFFSET  16	/* NIC register offset from the base_addr. */

/*  Probe for the Ultra.  This looks like a 8013 with the station
    address PROM at I/O ports <base>+8 to <base>+13, with a checksum
    following.
*/

int ultra_probe(struct device *dev)
{
    int *port, ports[] = {0x200, 0x220, 0x240, 0x280, 0x300, 0x340, 0x380, 0};
    unsigned short ioaddr = dev->base_addr;

    if (ioaddr > 0x1ff)
	return ultraprobe1(ioaddr, dev);
    else if (ioaddr > 0)
	return ENXIO;		/* Don't probe at all. */

    for (port = &ports[0]; *port; port++) {
	if (check_region(*port, 32))
	    continue;
	if ((inb(*port + 7) & 0xF0) == 0x20	/* Check chip ID nibble. */
	    && ultraprobe1(*port, dev) == 0)
	    return 0;
    }
    dev->base_addr = ioaddr;
    return ENODEV;
}

int ultraprobe1(int ioaddr, struct device *dev)
{
  int i;
  unsigned char *station_addr = dev->dev_addr;
  int checksum = 0;
  char *model_name;
  unsigned char eeprom_irq = 0;
  /* Values from various config regs. */
  unsigned char num_pages, irqreg, addr, reg4 = inb(ioaddr + 4) & 0x7f;


  /* Select the station address register set. */
  outb(reg4, ioaddr + 4);

  for (i = 0; i < 8; i++)
      checksum += inb(ioaddr + 8 + i);
  if ((checksum & 0xff) != 0xFF)
      return ENODEV;
  
  printk("%s: SMC Ultra at %#3x,", dev->name, ioaddr);
  for (i = 0; i < 6; i++)
      printk(" %2.2X", station_addr[i] = inb(ioaddr + 8 + i));

  /* Switch from the station address to the alternate register set and
     read the useful registers there. */
  outb(0x80 | reg4, ioaddr + 4);

  /* Enabled FINE16 mode to avoid BIOS ROM width mismatches during reboot. */
  outb(0x80 | inb(ioaddr + 0x0c), ioaddr + 0x0c);
  irqreg = inb(ioaddr + 0xd);
  addr = inb(ioaddr + 0xb);

  /* Switch back to the station address register set so that the MS-DOS driver
     can find the card after a warm boot. */
  outb(reg4, ioaddr + 4);

  model_name = "SMC Ultra";

  if (dev->irq < 2) {
      unsigned char irqmap[] = {0, 9, 3, 5, 7, 10, 11, 15};
      int irq;

      /* The IRQ bits are split. */
      irq = irqmap[((irqreg & 0x40) >> 4) + ((irqreg & 0x0c) >> 2)];

      if (irq == 0) {
	  printk(", failed to detect IRQ line.\n");
	  return -EAGAIN;
      }
      dev->irq = irq;
      eeprom_irq = 1;
  }


  /* OK, were are certain this is going to work.  Setup the device. */
  snarf_region(ioaddr, 32);

  /* The 8390 isn't at the base address, so fake the offset */
  dev->base_addr = ioaddr+ULTRA_NIC_OFFSET;

  { 
      int addr_tbl[4] = {0x0C0000, 0x0E0000, 0xFC0000, 0xFE0000};
      short num_pages_tbl[4] = {0x20, 0x40, 0x80, 0xff};

      dev->mem_start = ((addr & 0x0f) << 13) + addr_tbl[(addr >> 6) & 3] ;
      num_pages = num_pages_tbl[(addr >> 4) & 3];
  }

  ethdev_init(dev);

  ei_status.name = model_name;
  ei_status.word16 = 1;
  ei_status.tx_start_page = START_PG;
  ei_status.rx_start_page = START_PG + TX_PAGES;
  ei_status.stop_page = num_pages;

  dev->rmem_start = dev->mem_start + TX_PAGES*256;
  dev->mem_end = dev->rmem_end
      = dev->mem_start + (ei_status.stop_page - START_PG)*256;

  printk(",%s IRQ %d memory %#lx-%#lx.\n", eeprom_irq ? "" : "assigned ",
	 dev->irq, dev->mem_start, dev->mem_end-1);
  if (ei_debug > 0)
      printk(version);

  ei_status.reset_8390 = &ultra_reset_8390;
  ei_status.block_input = &ultra_block_input;
  ei_status.block_output = &ultra_block_output;
  dev->open = &ultra_open;
  dev->stop = &ultra_close_card;
  NS8390_init(dev, 0);

  return 0;
}

static int
ultra_open(struct device *dev)
{
  int ioaddr = dev->base_addr - ULTRA_NIC_OFFSET; /* ASIC addr */

  if (irqaction(dev->irq, &ei_sigaction))
      return -EAGAIN;

  outb(ULTRA_MEMENB, ioaddr);	/* Enable memory, 16 bit mode. */
  outb(0x80, ioaddr + 5);
  outb(0x01, ioaddr + 6);	/* Enable interrupts and memory. */
  return ei_open(dev);
}

static void
ultra_reset_8390(struct device *dev)
{
    int cmd_port = dev->base_addr - ULTRA_NIC_OFFSET; /* ASIC base addr */

    outb(ULTRA_RESET, cmd_port);
    if (ei_debug > 1) printk("resetting Ultra, t=%ld...", jiffies);
    ei_status.txing = 0;

    outb(ULTRA_MEMENB, cmd_port);

    if (ei_debug > 1) printk("reset done\n");
    return;
}

/* Block input and output are easy on shared memory ethercards, the only
   complication is when the ring buffer wraps. */

static int
ultra_block_input(struct device *dev, int count, char *buf, int ring_offset)
{
    void *xfer_start = (void *)(dev->mem_start + ring_offset
				- (START_PG<<8));

    if (xfer_start + count > (void*) dev->rmem_end) {
	/* We must wrap the input move. */
	int semi_count = (void*)dev->rmem_end - xfer_start;
	memcpy(buf, xfer_start, semi_count);
	count -= semi_count;
	memcpy(buf + semi_count, (char *)dev->rmem_start, count);
	return dev->rmem_start + count;
    }
    memcpy(buf, xfer_start, count);

    return ring_offset + count;
}

static void
ultra_block_output(struct device *dev, int count, const unsigned char *buf,
		int start_page)
{
    unsigned char *shmem
	= (unsigned char *)dev->mem_start + ((start_page - START_PG)<<8);

    memcpy(shmem, buf, count);

}

static int
ultra_close_card(struct device *dev)
{
    int ioaddr = dev->base_addr - ULTRA_NIC_OFFSET; /* CMDREG */

    dev->start = 0;
    dev->tbusy = 1;

    if (ei_debug > 1)
	printk("%s: Shutting down ethercard.\n", dev->name);

    outb(0x00, ioaddr + 6);	/* Disable interrupts. */
    free_irq(dev->irq);
    irq2dev_map[dev->irq] = 0;

    NS8390_init(dev, 0);

    /* We should someday disable shared memory and change to 8-bit mode
       "just in case"... */

    return 0;
}


/*
 * Local variables:
 *  compile-command: "gcc -D__KERNEL__ -Wall -O6 -I/usr/src/linux/net/inet -c smc-ultra.c"
 *  version-control: t
 *  kept-new-versions: 5
 * End:
 */
