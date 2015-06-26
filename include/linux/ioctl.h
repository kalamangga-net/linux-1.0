/* $Id: ioctl.h,v 1.5 1993/07/19 21:53:50 root Exp root $
 *
 * linux/ioctl.h for Linux by H.H. Bergman.
 */

#ifndef _LINUX_IOCTL_H
#define _LINUX_IOCTL_H


/* ioctl command encoding: 32 bits total, command in lower 16 bits,
 * size of the parameter structure in the lower 14 bits of the
 * upper 16 bits.
 * Encoding the size of the parameter structure in the ioctl request
 * is useful for catching programs compiled with old versions
 * and to avoid overwriting user space outside the user buffer area.
 * The highest 2 bits are reserved for indicating the ``access mode''.
 * NOTE: This limits the max parameter size to 16kB -1 !
 */

#define IOC_VOID	0x00000000	/* param in size field */
#define IOC_IN		0x40000000	/* user --> kernel */
#define IOC_OUT		0x80000000	/* kernel --> user */
#define IOC_INOUT	(IOC_IN | IOC_OUT)	/* both */
#define IOCSIZE_MASK	0x3fff0000	/* size (max 16k-1 bytes) */
#define IOCSIZE_SHIFT	16		/* how to get the size */
#define IOCCMD_MASK	0x0000ffff	/* command code */
#define IOCCMD_SHIFT	0


/* _IO(magic, subcode); size field is zero and the 
 * subcode determines the command.
 */
#define _IO(c,d)	(IOC_VOID | ((c)<<8) | (d)) /* param encoded */

/* _IOXX(magic, subcode, arg_t); where arg_t is the type of the
 * (last) argument field in the ioctl call, if present.
 */
#define _IOW(c,d,t)	(IOC_IN | ((sizeof(t)<<16) & IOCSIZE_MASK) | \
				  ((c)<<8) | (d))
#define _IOR(c,d,t)	(IOC_OUT | ((sizeof(t)<<16) & IOCSIZE_MASK) | \
				   ((c)<<8) | (d))
/* WR rather than RW to avoid conflict with stdio.h */
#define _IOWR(c,d,t)	(IOC_INOUT | ((sizeof(t)<<16) & IOCSIZE_MASK) | \
				     ((c)<<8) | (d))

#endif /* _LINUX_IOCTL_H */

