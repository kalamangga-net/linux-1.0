/*
 * ddi.h	Define the structure for linking in I/O drivers into the
 *		operating system kernel.  This method is currently only
 *		used by NET layer drivers, but it will be expanded into
 *		a link methos for ALL kernel-resident device drivers.
 *
 * Version:	@(#)ddi.h	1.0.2	04/22/93
 *
 * Author:	Fred N. van Kempen, <waltje@uwalt.nl.mugnet.org>
 */
#ifndef _LINUX_DDI_H
#define _LINUX_DDI_H


/* DDI control block flags. */
#define DDI_FREADY	0x10000000	/* device is initialized	*/
#define DDI_FPRESENT	0x20000000	/* device hardware is present	*/
#define DDI_FBLKDEV	0x00000001	/* device has a BLK spec. file	*/
#define DDI_FCHRDEV	0x00000002	/* device has a CHR spec. file	*/

/* Various constants. */
#define DDI_MAXNAME	16		/* length of a DDI ID string	*/


/* This structure is used to set up a DDI driver. */
struct ddconf {
  int		ioaddr;			/* main I/O (port) address	*/
  int		ioaux;			/* auxiliary I/O (HD, AST)	*/
  int		irq;			/* IRQ channel			*/
  int		dma;			/* DMA channel to use		*/
  unsigned long	memsize;		/* size of onboard memory	*/
  unsigned long	memaddr;		/* base address of memory	*/
};


/* The DDI device control block. */
struct ddi_device {
  char		*title;			/* title of the driver		*/
  char		name[DDI_MAXNAME];	/* unit name of the I/O driver	*/
  short int	unit;			/* unit number of this driver	*/
  short int	nunits;			/* number of units in driver	*/
  int		(*init)(struct ddi_device *);	/* initialization func		*/
  int		(*handler)(int, ...);	/* command handler		*/
  short	int	major;			/* driver major dev number	*/
  short	int	minor;			/* driver minor dev number	*/
  unsigned long	flags;			/* various flags		*/
  struct ddconf config;			/* driver HW setup		*/
};


/* This structure is used to set up networking protocols. */
struct ddi_proto {
  char		*name;			/* protocol name		*/
  void		(*init)(struct ddi_proto *);	/* initialization func	*/
};


/* This structure is used to link a STREAMS interface. */
struct iflink {
  char		id[DDI_MAXNAME];	/* DDI ID string		*/
  char		stream[DDI_MAXNAME];	/* STREAMS interface name	*/
  int		family;			/* address (protocol) family	*/
  unsigned int	flags;			/* any flags needed (unused)	*/
};


/* DDI control requests. */
#define DDIOCSDBG	0x9000		/* set DDI debug level		*/
#define DDIOCGNAME	0x9001		/* get DDI ID name		*/
#define DDIOCGCONF	0x9002		/* get DDI HW config		*/
#define DDIOCSCONF	0x9003		/* set DDI HW config		*/


/* DDI global functions. */
extern void			ddi_init(void);
extern struct ddi_device	*ddi_map(const char *id);


#endif	/* _LINUX_DDI_H */
