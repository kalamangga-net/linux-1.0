/*
 * Space.c	Defines which protocol modules and I/O device drivers get
 *		linked into the LINUX kernel.  Currently, this is only used
 *		by the NET layer of LINUX, but it eventually might move to
 *		an upper directory of the system.
 *
 * Version:	@(#)Space.c	1.0.2	04/22/93
 *
 * Author:	Fred N. van Kempen, <waltje@uwalt.nl.mugnet.org>
 */
#include <linux/config.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/ddi.h>


#define CONFIG_UNIX		YES		/* always present...	*/


/*
 * Section A:	Networking Protocol Handlers.
 *		This section defines which networking protocols get
 *		linked into the SOCKET layer of the Linux kernel.
 *		Currently, these are AF_UNIX (always) and AF_INET.
 */
#ifdef	CONFIG_UNIX
#  include "unix/unix.h"
#endif
#ifdef	CONFIG_INET
#  include "inet/inet.h"
#endif
#ifdef CONFIG_IPX
#include "inet/ipxcall.h"
#endif
#ifdef CONFIG_AX25
#include "inet/ax25call.h"
#endif

struct ddi_proto protocols[] = {
#ifdef	CONFIG_UNIX
  { "UNIX",	unix_proto_init	},
#endif
#ifdef  CONFIG_IPX
  { "IPX",	ipx_proto_init },
#endif
#ifdef CONFIG_AX25  
  { "AX.25",	ax25_proto_init },
#endif  
#ifdef	CONFIG_INET
  { "INET",	inet_proto_init	},
#endif
  { NULL,	NULL		}
};


/*
 * Section B:	Device Driver Modules.
 *		This section defines which network device drivers
 *		get linked into the Linux kernel.  It is currently
 *		only used by the INET protocol.  Any takers for the
 *		other protocols like XNS or Novell?
 *
 * WARNING:	THIS SECTION IS NOT YET USED BY THE DRIVERS !!!!!
 */
/*#include "drv/we8003/we8003.h"	Western Digital WD-80[01]3	*/
/*#include "drv/dp8390/dp8390.h"	Donald Becker's DP8390 kit	*/
/*#inclde "drv/slip/slip.h"		Laurence Culhane's SLIP kit	*/


struct ddi_device devices[] = {
#if CONF_WE8003
  { "WD80x3[EBT]",
	"",	0,	1,	we8003_init,	NULL,
	19,	0,	DDI_FCHRDEV,
    { 0x280,	0,	15,	0,	32768,	0xD0000		}	},
#endif
#if CONF_DP8390
  { "DP8390/WD80x3",
	"",	0,	1,	dpwd8003_init,	NULL,
	20,	0,	DDI_FCHRDEV,
    {	0,	0,	0,	0,	0,	0,		}	},
  { "DP8390/NE-x000",
	"",	0,	1,	dpne2000_init,	NULL,
	20,	8,	DDI_FCHRDEV,
    {	0,	0,	0,	0,	0,	0,		}	},
  { "DP8390/3C50x",
	"",	0,	1,	dpec503_init,	NULL,
	20,	16,	DDI_FCHRDEV,
    {	0,	0,	0,	0,	0,	0,		}	},
#endif
  { NULL,
	"",	0,	0,	NULL,		NULL,
	0,	0,	0,
    {	0,	0,	0,	0,	0,	0		}	}
};
