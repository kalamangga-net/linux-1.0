#define AUTOSENSE

/*
 * Generic Generic NCR5380 driver
 *	
 * Copyright 1993, Drew Eckhardt
 *	Visionary Computing
 *	(Unix and Linux consulting and custom programming)
 *	drew@colorado.edu
 *      +1 (303) 440-4894
 *
 * ALPHA RELEASE 1. 
 *
 * For more information, please consult 
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
 * TODO : flesh out DMA support, find some one actually using this (I have
 * 	a memory mapped Trantor board that works fine)
 */

/*
 * Options :
 *
 * PARITY - enable parity checking.  Not supported.
 *
 * SCSI2 - enable support for SCSI-II tagged queueing.  Untested.
 *
 * USLEEP - enable support for devices that don't disconnect.  Untested.
 *
 * The card is detected and initialized in one of several ways : 
 * 1.  With command line overrides - NCR5380=port,irq may be 
 *     used on the LILO command line to override the defaults.
 *
 * 2.  With the GENERIC_NCR5380_OVERRIDE compile time define.  This is 
 *     specified as an array of address, irq tupples.  Ie, for
 *     one board at the default 0xcc000 address, IRQ5, no dma, I could 
 *     say  -DGENERIC_NCR5380_OVERRIDE={{0xcc000, 5, DMA_NONE}}
 * 
 * -1 should be specified for no or DMA interrupt, -2 to autoprobe for an 
 * 	IRQ line if overriden on the command line.
 */
 
/*
 * $Log: generic_NCR5380.c,v $
 */

#include <linux/config.h>
#if defined(CONFIG_SCSI_GENERIC_NCR5380)
/* Standard option */
#define AUTOPROBE_IRQ

#include <asm/system.h>
#include <asm/io.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include "../block/blk.h"
#include "scsi.h"
#include "hosts.h"
#include "g_NCR5380.h"
#include "NCR5380.h"
#include "constants.h"

static struct override {
    int port;
    int irq;
    int dma;
} overrides 
#ifdef GENERIC_NCR5380_OVERRIDE 
    [] = GENERIC_NCR5380_OVERRIDE
#else
    [1] = {{0,},};
#endif

#define NO_OVERRIDES (sizeof(overrides) / sizeof(struct override))

/*
 * Function : generic_NCR5380_setup(char *str, int *ints)
 *
 * Purpose : LILO command line initialization of the overrides array,
 * 
 * Inputs : str - unused, ints - array of integer paramters with ints[0]
 *	equal to the number of ints.
 *
 */

void generic_NCR5380_setup(char *str, int *ints) {
    static int commandline_current = 0;
    if (ints[0] != 2) 
	printk("generic_NCR5380_setup : usage ncr5380=port,irq,dma\n");
    else 
	if (commandline_current < NO_OVERRIDES) {
	    overrides[commandline_current].port = ints[1];
	    overrides[commandline_current].irq = ints[2];
	    overrides[commandline_current].dma = ints[3];
	    ++commandline_current;
	}
}

static struct sigaction sa =  { generic_NCR5380_intr, 0, 
    SA_INTERRUPT , NULL };

/* 
 * Function : int generic_NCR5380_detect(int hostno)
 *
 * Purpose : initializes generic NCR5380 driver based on the 
 *	command line / compile time port and irq definitions.
 *
 * Inputs : hostno - id of this SCSI adapter.
 * 
 * Returns : 1 if a host adapter was found, 0 if not.
 *
 */

int generic_NCR5380_detect(int hostno) {
    static int current_override = 0;
    int count;
    struct Scsi_Host *instance;

    for (count = 0; current_override < NO_OVERRIDES; ++current_override) {
	if (!(overrides[current_override].port))
	    continue;

	instance = scsi_register (hostno, sizeof(struct NCR5380_hostdata));
	instance->io_port = overrides[current_override].port;

	NCR5380_init(instance);

	if (overrides[current_override].irq != IRQ_AUTO)
	    instance->irq = overrides[current_override].irq;
	else 
	    instance->irq = NCR5380_probe_irq(instance, 0xffff);

	if (instance->irq != IRQ_NONE) 
	    if (irqaction (instance->irq, &sa)) {
		printk("scsi%d : IRQ%d not free, interrupts disabled\n", 
		    hostno, instance->irq);
		instance->irq = IRQ_NONE;
	    } 

	if (instance->irq == IRQ_NONE) {
	    printk("scsi%d : interrupts not enabled. for better interactive performance,\n", hostno);
	    printk("scsi%d : please jumper the board for a free IRQ.\n", hostno);
	}

	printk("scsi%d : at port %d", instance->host_no, instance->io_port);
	if (instance->irq == IRQ_NONE)
	    printk (" interrupts disabled");
	else 
	    printk (" irq %d", instance->irq);
	printk(" options CAN_QUEUE=%d  CMD_PER_LUN=%d release=%d",
	    CAN_QUEUE, CMD_PER_LUN, GENERIC_NCR5380_PUBLIC_RELEASE);
	NCR5380_print_options(instance);
	printk("\n");

	++current_override;
	++count;
    }
    return count;
}

const char * generic_NCR5380_info (void) {
    static const char string[]="";
    return string;
}

#include "NCR5380.c"

#endif /* defined(CONFIG_SCSI_GENERIC_NCR5380) */
