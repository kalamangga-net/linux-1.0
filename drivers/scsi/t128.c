#define AUTOSENSE
#define PSEUDO_DMA

/*
 * Trantor T128/T128F/T228 driver
 *	Note : architecturally, the T100 and T130 are different and won't 
 * 	work
 *
 * Copyright 1993, Drew Eckhardt
 *	Visionary Computing
 *	(Unix and Linux consulting and custom programming)
 *	drew@colorado.edu
 *      +1 (303) 440-4894
 *
 * DISTRIBUTION RELEASE 3.
 *
 * For more information, please consult 
 *
 * Trantor Systems, Ltd.
 * T128/T128F/T228 SCSI Host Adapter
 * Hardware Specifications
 * 
 * Trantor Systems, Ltd. 
 * 5415 Randall Place
 * Fremont, CA 94538
 * 1+ (415) 770-1400, FAX 1+ (415) 770-9910
 * 
 * and 
 *
 * NCR 5380 Family
 * SCSI Protocol Controller
 * Databook
 *
 * NCR Microelectronics
 * 1635 Aeroplaza Drive
 * Colorado Springs, CO 80916
 * 1+ (719) 578-3400
 * 1+ (800) 334-5454
 */

/*
 * Options : 
 * AUTOSENSE - if defined, REQUEST SENSE will be performed automatically
 *      for commands that return with a CHECK CONDITION status. 
 *
 * PSEUDO_DMA - enables PSEUDO-DMA hardware, should give a 3-4X performance
 * increase compared to polled I/O.
 *
 * PARITY - enable parity checking.  Not supported.
 * 
 * SCSI2 - enable support for SCSI-II tagged queueing.  Untested.
 *
 *
 * UNSAFE - leave interrupts enabled during pseudo-DMA transfers.  You
 *          only really want to use this if you're having a problem with
 *          dropped characters during high speed communications, and even
 *          then, you're going to be better off twiddling with transfersize.
 *
 * USLEEP - enable support for devices that don't disconnect.  Untested.
 *
 * The card is detected and initialized in one of several ways : 
 * 1.  Autoprobe (default) - since the board is memory mapped, 
 *     a BIOS signature is scanned for to locate the registers.
 *     An interrupt is triggered to autoprobe for the interrupt
 *     line.
 *
 * 2.  With command line overrides - t128=address,irq may be 
 *     used on the LILO command line to override the defaults.
 *
 * 3.  With the T128_OVERRIDE compile time define.  This is 
 *     specified as an array of address, irq tupples.  Ie, for
 *     one board at the default 0xcc000 address, IRQ5, I could say 
 *     -DT128_OVERRIDE={{0xcc000, 5}}
 *	
 *     Note that if the override methods are used, place holders must
 *     be specified for other boards in the system.
 * 
 * T128/T128F jumper/dipswitch settings (note : on my sample, the switches 
 * were epoxy'd shut, meaning I couldn't change the 0xcc000 base address) :
 *
 * T128    Sw7 Sw8 Sw6 = 0ws Sw5 = boot 
 * T128F   Sw6 Sw7 Sw5 = 0ws Sw4 = boot Sw8 = floppy disable
 * cc000   off off      
 * c8000   off on
 * dc000   on  off
 * d8000   on  on
 *
 * 
 * Interrupts 
 * There is a 12 pin jumper block, jp1, numbered as follows : 
 *   T128 (JP1)  	 T128F (J5)
 * 2 4 6 8 10 12	11 9  7 5 3 1
 * 1 3 5 7 9  11	12 10 8 6 4 2
 *
 * 3   2-4
 * 5   1-3
 * 7   3-5
 * T128F only 
 * 10 8-10
 * 12 7-9
 * 14 10-12
 * 15 9-11
 */
 
/*
 * $Log: t128.c,v $
 */

#include <asm/system.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include "../block/blk.h"
#include "scsi.h"
#include "hosts.h"
#include "t128.h"
#define AUTOPROBE_IRQ
#include "NCR5380.h"
#include "constants.h"
#include <linux/string.h>



static struct override {
    unsigned char *address;
    int irq;
} overrides 
#ifdef T128_OVERRIDE
    [] = T128_OVERRIDE;
#else
    [4] = {{NULL,IRQ_AUTO}, {NULL,IRQ_AUTO}, {NULL,IRQ_AUTO},
	{NULL,IRQ_AUTO}};
#endif

#define NO_OVERRIDES (sizeof(overrides) / sizeof(struct override))

static struct base {
    unsigned char *address;
    int noauto;
} bases[] = {{(unsigned char *) 0xcc000, 0}, {(unsigned char *) 0xc8000, 0},
    {(unsigned char *) 0xdc000, 0}, {(unsigned char *) 0xd8000, 0}};

#define NO_BASES (sizeof (bases) / sizeof (struct base))

static const struct signature {
    const char *string;
    int offset;
} signatures[] = {
{"TSROM: SCSI BIOS, Version 1.12", 0x36},
};

#define NO_SIGNATURES (sizeof (signatures) /  sizeof (struct signature))

/*
 * Function : t128_setup(char *str, int *ints)
 *
 * Purpose : LILO command line initialization of the overrides array,
 * 
 * Inputs : str - unused, ints - array of integer paramters with ints[0]
 *	equal to the number of ints.
 *
 */

void t128_setup(char *str, int *ints) {
    static int commandline_current = 0;
    int i;
    if (ints[0] != 2) 
	printk("t128_setup : usage t128=address,irq\n");
    else 
	if (commandline_current < NO_OVERRIDES) {
	    overrides[commandline_current].address = (unsigned char *) ints[1];
	    overrides[commandline_current].irq = ints[2];
	    for (i = 0; i < NO_BASES; ++i)
		if (bases[i].address == (unsigned char *) ints[1]) {
		    bases[i].noauto = 1;
		    break;
		}
	    ++commandline_current;
	}
}

static struct sigaction t128_sigaction =  { t128_intr, 0, SA_INTERRUPT , NULL };

/* 
 * Function : int t128_detect(int hostno)
 *
 * Purpose : detects and initializes T128,T128F, or T228 controllers
 *	that were autoprobed, overriden on the LILO command line, 
 *	or specified at compile time.
 *
 * Inputs : hostno - id of this SCSI adapter.
 * 
 * Returns : 1 if a host adapter was found, 0 if not.
 *
 */

int t128_detect(int hostno) {
    static int current_override = 0, current_base = 0;
    struct Scsi_Host *instance;
    unsigned char *base;
    int sig, count;

    for (count = 0; current_override < NO_OVERRIDES; ++current_override) {
	base = NULL;

	if (overrides[current_override].address)
	    base = overrides[current_override].address;
	else 
	    for (; !base && (current_base < NO_BASES); ++current_base) {
#if (TDEBUG & TDEBUG_INIT)
    printk("scsi%d : probing address %08x\n", hostno, (unsigned int) bases[current_base].address);
#endif
		for (sig = 0; sig < NO_SIGNATURES; ++sig) 
		    if (!bases[current_base].noauto && !memcmp 
			(bases[current_base].address + signatures[sig].offset, 
			signatures[sig].string, strlen(signatures[sig].string))) {
			base = bases[current_base].address;
#if (TDEBUG & TDEBUG_INIT)
			printk("scsi%d : detected board.\n", hostno);
#endif
			break;
		    }
	    }

#if defined(TDEBUG) && (TDEBUG & TDEBUG_INIT)
	printk("scsi%d : base = %08x\n", hostno, (unsigned int) base);
#endif

	if (!base)
	    break;

	instance = scsi_register (hostno, sizeof(struct NCR5380_hostdata));
	instance->base = base;

	NCR5380_init(instance);

	if (overrides[current_override].irq != IRQ_AUTO)
	    instance->irq = overrides[current_override].irq;
	else 
	    instance->irq = NCR5380_probe_irq(instance, T128_IRQS);

	if (instance->irq != IRQ_NONE) 
	    if (irqaction (instance->irq, &t128_sigaction)) {
		printk("scsi%d : IRQ%d not free, interrupts disabled\n", 
		    hostno, instance->irq);
		instance->irq = IRQ_NONE;
	    } 

	if (instance->irq == IRQ_NONE) {
	    printk("scsi%d : interrupts not enabled. for better interactive performance,\n", hostno);
	    printk("scsi%d : please jumper the board for a free IRQ.\n", hostno);
	}

#if defined(TDEBUG) && (TDEBUG & TDEBUG_INIT)
	printk("scsi%d : irq = %d\n", hostno, instance->irq);
#endif

	printk("scsi%d : at 0x%08x", instance->host_no, (int) 
	    instance->base);
	if (instance->irq == IRQ_NONE)
	    printk (" interrupts disabled");
	else 
	    printk (" irq %d", instance->irq);
	printk(" options CAN_QUEUE=%d  CMD_PER_LUN=%d release=%d",
	    CAN_QUEUE, CMD_PER_LUN, T128_PUBLIC_RELEASE);
	NCR5380_print_options(instance);
	printk("\n");

	++current_override;
	++count;
	++hostno;
    }
    return count;
}

/*
 * Function : int t128_biosparam(int size, int dev, int *ip)
 *
 * Purpose : Generates a BIOS / DOS compatable H-C-S mapping for 
 *	the specified device / size.
 * 
 * Inputs : size = size of device in sectors (512 bytes), dev = block device
 *	major / minor, ip[] = {heads, sectors, cylinders}  
 *
 * Returns : allways 0 (success), initializes ip
 *	
 */

/* 
 * XXX Most SCSI boards use this mapping, I could be incorrect.  Some one
 * using hard disks on a trantor should verify that this mapping corresponds
 * to that used by the BIOS / ASPI driver by running the linux fdisk program
 * and matching the H_C_S coordinates to what DOS uses.
 */

int t128_biosparam(int size, int dev, int * ip)
{
  ip[0] = 64;
  ip[1] = 32;
  ip[2] = size >> 11;
  return 0;
}

/*
 * Function : int NCR5380_pread (struct Scsi_Host *instance, 
 *	unsigned char *dst, int len)
 *
 * Purpose : Fast 5380 pseudo-dma read function, transfers len bytes to 
 *	dst
 * 
 * Inputs : dst = destination, len = length in bytes
 *
 * Returns : 0 on success, non zero on a failure such as a watchdog 
 * 	timeout.
 */

static inline int NCR5380_pread (struct Scsi_Host *instance, unsigned char *dst,
    int len) {
    register unsigned char *reg = (unsigned char *) (instance->base + 
	T_DATA_REG_OFFSET), *d = dst;
    register i = len;

    while (!(instance->base[T_STATUS_REG_OFFSET]) & T_ST_RDY);

    for (; i; --i) 
	*d++ = *reg;

    if (*(instance->base + T_STATUS_REG_OFFSET) & T_ST_TIM) {
	unsigned char tmp;
	volatile unsigned char *foo;
	foo = instance->base + T_CONTROL_REG_OFFSET;
	tmp = *foo;
	*foo = tmp | T_CR_CT;
	*foo = tmp;
	printk("scsi%d : watchdog timer fired in NCR5480_pread()\n",
	    instance->host_no);
	return -1;
    } else
	return 0;
}

/*
 * Function : int NCR5380_pwrite (struct Scsi_Host *instance, 
 *	unsigned char *src, int len)
 *
 * Purpose : Fast 5380 pseudo-dma write function, transfers len bytes from
 *	src
 * 
 * Inputs : src = source, len = length in bytes
 *
 * Returns : 0 on success, non zero on a failure such as a watchdog 
 * 	timeout.
 */

static inline int NCR5380_pwrite (struct Scsi_Host *instance, unsigned char *src,
    int len) {
    register unsigned char *reg = (unsigned char *) (instance->base + 
	T_DATA_REG_OFFSET), *s = src;
    register i = len;

    while (!(instance->base[T_STATUS_REG_OFFSET]) & T_ST_RDY);
    for (; i; --i)
	*reg = *s++;

    if (*(instance->base + T_STATUS_REG_OFFSET) & T_ST_TIM) {
	unsigned char tmp;
	volatile unsigned char *foo;
	foo = instance->base + T_CONTROL_REG_OFFSET;
	tmp = *foo;
	*foo = tmp | T_CR_CT;
	*foo = tmp;
	printk("scsi%d : watchdog timer fired in NCR5480_pwrite()\n",
	    instance->host_no);
	return -1;
    } else 
	return 0;
}

/*
 * Function : const char *t128_info(void)
 *
 * Purpose : provide furthur information about this driver.
 *
 * Returns : an empty string.
 */

const char *t128_info (void) {
    static const char string[]="";
    return string;
}

#include "NCR5380.c"
