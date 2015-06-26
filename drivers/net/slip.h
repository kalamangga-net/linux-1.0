/*
 * slip.h	Define the SLIP device driver interface and constants.
 *
 * NOTE:	THIS FILE WILL BE MOVED TO THE LINUX INCLUDE DIRECTORY
 *		AS SOON AS POSSIBLE!
 *
 * Version:	@(#)slip.h	1.2.0	03/28/93
 *
 * Fixes:
 *		Alan Cox	: 	Added slip mtu field.
 *		Matt Dillon	:	Printable slip (borrowed from net2e)
 *
 * Author:	Fred N. van Kempen, <waltje@uwalt.nl.mugnet.org>
 */
#ifndef _LINUX_SLIP_H
#define _LINUX_SLIP_H

/* SLIP configuration. */
#define SL_NRUNIT	4		/* number of SLIP channels	*/
#define SL_MTU		296		/* 296; I am used to 600- FvK	*/

/* SLIP protocol characters. */
#define END             0300		/* indicates end of frame	*/
#define ESC             0333		/* indicates byte stuffing	*/
#define ESC_END         0334		/* ESC ESC_END means END 'data'	*/
#define ESC_ESC         0335		/* ESC ESC_ESC means ESC 'data'	*/


struct slip {
  /* Bitmapped flag fields. */
  char			inuse;		/* are we allocated?		*/
  char			sending;	/* "channel busy" indicator	*/
  char			escape;		/* SLIP state machine		*/
  char			unused;		/* fillers			*/

  /* Various fields. */
  int			line;		/* SLIP channel number		*/
  struct tty_struct	*tty;		/* ptr to TTY structure		*/
  struct device		*dev;		/* easy for intr handling	*/
  struct slcompress	*slcomp;	/* for header compression 	*/

  /* These are pointers to the malloc()ed frame buffers. */
  unsigned char		*rbuff;		/* receiver buffer		*/
  unsigned char		*xbuff;		/* transmitter buffer		*/
  unsigned char		*cbuff;		/* compression buffer		*/

  /* These are the various pointers into the buffers. */
  unsigned char		*rhead;		/* RECV buffer pointer (head)	*/
  unsigned char		*rend;		/* RECV buffer pointer (end)	*/
  int			rcount;		/* SLIP receive counter		*/

  /* SLIP interface statistics. */
  unsigned long		rpacket;	/* inbound frame counter	*/
  unsigned long		roverrun;	/* "buffer overrun" counter	*/
  unsigned long		spacket;	/* outbound frames counter	*/
  unsigned long		sbusy;		/* "transmitter busy" counter	*/
  unsigned long		errors;		/* error count			*/
  
  int			mtu;		/* Our mtu (to spot changes!)   */
  unsigned char		flags;		/* Flag values/ mode etc	*/
#define SLF_ESCAPE	2
#define SLF_ERROR	4
#define SLF_COMP	16
#define SLF_EXPN	32
  unsigned char		mode;		/* SLIP mode			*/
#define SL_MODE_SLIP	0
#define SL_MODE_CSLIP	1
#define SL_MODE_SLIP6	2		/* Matt Dillon's printable slip */
#define SL_MODE_CSLIP6	(SL_MODE_SLIP6|SL_MODE_CSLIP)
#define SL_MODE_AX25	4
#define SL_MODE_ADAPTIVE 8
  int			xdata,xbits;	/* 6 bit slip controls 		*/
};


extern int	slip_init(struct device *dev);
extern int	slip_esc(unsigned char *s, unsigned char *d, int len);
extern int	slip_esc6(unsigned char *s, unsigned char *d, int len);
extern void	slip_unesc(struct slip *sl, unsigned char *s, int count, int error);
extern void 	slip_unesc6(struct slip *sl, unsigned char *s, int count, int error);


#endif	/* _LINUX_SLIP.H */
