/*
 * portio.h	Definitions of routines for detecting, reserving and
 *		allocating system resources.
 *
 * Version:	0.01	8/30/93
 *
 * Author:	Donald Becker (becker@super.org)
 */

#ifndef _LINUX_PORTIO_H
#define _LINUX_PORTIO_H

#define HAVE_PORTRESERVE
/*
 * Call check_region() before probing for your hardware.
 * Once you have found you hardware, register it with snarf_region().
 */
extern void reserve_setup(char *str, int *ints);
extern int check_region(unsigned int from, unsigned int extent);
extern void snarf_region(unsigned int from, unsigned int extent);


#define HAVE_AUTOIRQ
extern void *irq2dev_map[16];		/* Use only if you own the IRQ. */
extern void autoirq_setup(int waittime);
extern int autoirq_report(int waittime);

#endif	/* _LINUX_PORTIO_H */
