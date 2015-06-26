/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Holds initial configuration information for devices.
 *
 * NOTE:	This file is a nice idea, but its current format does not work
 *		well for drivers that support multiple units, like the SLIP
 *		driver.  We should actually have only one pointer to a driver
 *		here, with the driver knowing how many units it supports.
 *		Currently, the SLIP driver abuses the "base_addr" integer
 *		field of the 'device' structure to store the unit number...
 *		-FvK
 *
 * Version:	@(#)Space.c	1.0.7	08/12/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Donald J. Becker, <becker@super.org>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#include <linux/config.h>
#include <linux/ddi.h>
#include "dev.h"

#define LOOPBACK			/* always present, right?	*/

#define	NEXT_DEV	NULL


/* A unifed ethernet device probe.  This is the easiest way to have every
   ethernet adaptor have the name "eth[0123...]".
   */

extern int ultra_probe(struct device *dev);
extern int wd_probe(struct device *dev);
extern int el2_probe(struct device *dev);
extern int ne_probe(struct device *dev);
extern int hp_probe(struct device *dev);
extern int znet_probe(struct device *);
extern int express_probe(struct device *);
extern int el3_probe(struct device *);
extern int at1500_probe(struct device *);
extern int at1700_probe(struct device *);
extern int depca_probe(struct device *);
extern int el1_probe(struct device *);
extern int el16_probe(struct device *);
extern int elplus_probe(struct device *);
extern int ac3200_probe(struct device *);
extern int e2100_probe(struct device *);

/* Detachable devices ("pocket adaptors" and special PCMCIA drivers). */
extern int atp_init(struct device *);
extern int d_link_init(struct device *);

static int
ethif_probe(struct device *dev)
{
    short base_addr = dev->base_addr;

    if (base_addr < 0  ||  base_addr == 1)
	return 1;		/* ENXIO */

    if (1
#if defined(CONFIG_ULTRA)
	&& ultra_probe(dev)
#endif
#if defined(CONFIG_WD80x3) || defined(WD80x3)
	&& wd_probe(dev)
#endif
#if defined(CONFIG_EL2) || defined(EL2)	/* 3c503 */
	&& el2_probe(dev)
#endif
#if defined(CONFIG_NE2000) || defined(NE2000)
	&& ne_probe(dev)
#endif
#if defined(CONFIG_HPLAN) || defined(HPLAN)
	&& hp_probe(dev)
#endif
#ifdef CONFIG_AT1500
	&& at1500_probe(dev)
#endif
#ifdef CONFIG_AT1700
	&& at1700_probe(dev)
#endif
#ifdef CONFIG_EL3		/* 3c509 */
	&& el3_probe(dev)
#endif
#ifdef CONFIG_ZNET		/* Zenith Z-Note and some IBM Thinkpads. */
	&& znet_probe(dev)
#endif
#ifdef CONFIG_EEXPRESS		/* Intel EtherExpress */
	&& express_probe(dev)
#endif
#ifdef CONFIG_DEPCA		/* DEC DEPCA */
	&& depca_probe(dev)
#endif
#ifdef CONFIG_EL1		/* 3c501 */
	&& el1_probe(dev)
#endif
#ifdef CONFIG_EL16		/* 3c507 */
	&& el16_probe(dev)
#endif
#ifdef CONFIG_ELPLUS		/* 3c505 */
	&& elplus_probe(dev)
#endif
#ifdef CONFIG_AC3200		/* Ansel Communications EISA 3200. */
	&& ac3200_probe(dev)
#endif
#ifdef CONFIG_E2100		/* Cabletron E21xx series. */
	&& e2100_probe(dev)
#endif
	&& 1 ) {
	return 1;	/* -ENODEV or -EAGAIN would be more accurate. */
    }
    return 0;
}


/* This remains seperate because it requires the addr and IRQ to be set. */
#if defined(D_LINK) || defined(CONFIG_DE600)
static struct device d_link_dev = {
    "dl0", 0, 0, 0, 0, D_LINK_IO, D_LINK_IRQ, 0, 0, 0, NEXT_DEV, d_link_init };
#   undef NEXT_DEV
#   define NEXT_DEV	(&d_link_dev)
#endif

/* Run-time ATtachable (Pocket) devices have a different (not "eth#") name. */
#ifdef CONFIG_ATP		/* AT-LAN-TEC (RealTek) pocket adaptor. */
static struct device atp_dev = {
    "atp0", 0, 0, 0, 0, 0, 0, 0, 0, 0, NEXT_DEV, atp_init, /* ... */ };
#   undef NEXT_DEV
#   define NEXT_DEV	(&atp_dev)
#endif

/* The first device defaults to I/O base '0', which means autoprobe. */
#ifndef ETH0_ADDR
# define ETH0_ADDR 0
#endif
#ifndef ETH0_IRQ
# define ETH0_IRQ 0
#endif
/* "eth0" defaults to autoprobe (== 0), other use a base of 0xffe0 (== -0x20),
   which means "don't probe".  These entries exist to only to provide empty
   slots which may be enabled at boot-time. */

static struct device eth3_dev = {
    "eth3", 0,0,0,0,0xffe0 /* I/O base*/, 0,0,0,0, NEXT_DEV, ethif_probe };
static struct device eth2_dev = {
    "eth2", 0,0,0,0,0xffe0 /* I/O base*/, 0,0,0,0, &eth3_dev, ethif_probe };
static struct device eth1_dev = {
    "eth1", 0,0,0,0,0xffe0 /* I/O base*/, 0,0,0,0, &eth2_dev, ethif_probe };

static struct device eth0_dev = {
    "eth0", 0, 0, 0, 0, ETH0_ADDR, ETH0_IRQ, 0, 0, 0, &eth1_dev, ethif_probe };

#   undef NEXT_DEV
#   define NEXT_DEV	(&eth0_dev)

#if defined(PLIP) || defined(CONFIG_PLIP)
    extern int plip_init(struct device *);
    static struct device plip2_dev = {
	"plip2", 0, 0, 0, 0, 0x278, 2, 0, 0, 0, NEXT_DEV, plip_init, };
    static struct device plip1_dev = {
	"plip1", 0, 0, 0, 0, 0x378, 7, 0, 0, 0, &plip2_dev, plip_init, };
    static struct device plip0_dev = {
	"plip0", 0, 0, 0, 0, 0x3BC, 5, 0, 0, 0, &plip1_dev, plip_init, };
#   undef NEXT_DEV
#   define NEXT_DEV	(&plip0_dev)
#endif  /* PLIP */

#if defined(SLIP) || defined(CONFIG_SLIP)
    extern int slip_init(struct device *);
    static struct device slip3_dev = {
	"sl3",			/* Internal SLIP driver, channel 3	*/
	0x0,			/* recv memory end			*/
	0x0,			/* recv memory start			*/
	0x0,			/* memory end				*/
	0x0,			/* memory start				*/
	0x3,			/* base I/O address			*/
	0,			/* IRQ					*/
	0, 0, 0,		/* flags				*/
	NEXT_DEV,		/* next device				*/
	slip_init		/* slip_init should set up the rest	*/
    };
    static struct device slip2_dev = {
	"sl2",			/* Internal SLIP driver, channel 2	*/
	0x0,			/* recv memory end			*/
	0x0,			/* recv memory start			*/
	0x0,			/* memory end				*/
	0x0,			/* memory start				*/
	0x2,			/* base I/O address			*/
	0,			/* IRQ					*/
	0, 0, 0,		/* flags				*/
	&slip3_dev,		/* next device				*/
	slip_init		/* slip_init should set up the rest	*/
    };
    static struct device slip1_dev = {
	"sl1",			/* Internal SLIP driver, channel 1	*/
	0x0,			/* recv memory end			*/
	0x0,			/* recv memory start			*/
	0x0,			/* memory end				*/
	0x0,			/* memory start				*/
	0x1,			/* base I/O address			*/
	0,			/* IRQ					*/
	0, 0, 0,		/* flags				*/
	&slip2_dev,		/* next device				*/
	slip_init		/* slip_init should set up the rest	*/
    };
    static struct device slip0_dev = {
	"sl0",			/* Internal SLIP driver, channel 0	*/
	0x0,			/* recv memory end			*/
	0x0,			/* recv memory start			*/
	0x0,			/* memory end				*/
	0x0,			/* memory start				*/
	0x0,			/* base I/O address			*/
	0,			/* IRQ					*/
	0, 0, 0,		/* flags				*/
	&slip1_dev,		/* next device				*/
	slip_init		/* slip_init should set up the rest	*/
    };
#   undef	NEXT_DEV
#   define	NEXT_DEV	(&slip0_dev)
#endif	/* SLIP */
  
#if defined(CONFIG_PPP)
extern int ppp_init(struct device *);
static struct device ppp3_dev = {
    "ppp3", 0x0, 0x0, 0x0, 0x0, 3, 0, 0, 0, 0, NEXT_DEV,  ppp_init, };
static struct device ppp2_dev = {
    "ppp2", 0x0, 0x0, 0x0, 0x0, 2, 0, 0, 0, 0, &ppp3_dev, ppp_init, };
static struct device ppp1_dev = {
    "ppp1", 0x0, 0x0, 0x0, 0x0, 1, 0, 0, 0, 0, &ppp2_dev, ppp_init, };
static struct device ppp0_dev = {
    "ppp0", 0x0, 0x0, 0x0, 0x0, 0, 0, 0, 0, 0, &ppp1_dev, ppp_init, };
#undef NEXT_DEV
#define NEXT_DEV (&ppp0_dev)
#endif   /* PPP */

#ifdef LOOPBACK
    extern int loopback_init(struct device *dev);
    static struct device loopback_dev = {
	"lo",			/* Software Loopback interface		*/
	0x0,			/* recv memory end			*/
	0x0,			/* recv memory start			*/
	0x0,			/* memory end				*/
	0x0,			/* memory start				*/
	0,			/* base I/O address			*/
	0,			/* IRQ					*/
	0, 0, 0,		/* flags				*/
	NEXT_DEV,		/* next device				*/
	loopback_init		/* loopback_init should set up the rest	*/
    };
#   undef	NEXT_DEV
#   define	NEXT_DEV	(&loopback_dev)
#endif


struct device *dev_base = NEXT_DEV;
