/*
 * ddi.c	Implement the Device Driver Interface (DDI) routines.
 *		Currently, this is only used by the NET layer of LINUX,
 *		but it eventually might move to an upper directory of
 *		the system.
 *
 * Version:	@(#)ddi.c	1.0.5	04/22/93
 *
 * Author:	Fred N. van Kempen, <waltje@uwalt.nl.mugnet.org>
 */
#include <asm/segment.h>
#include <asm/system.h>
#include <linux/types.h>
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/socket.h>
#include <linux/ddi.h>


#undef	DDI_DEBUG
#ifdef	DDI_DEBUG
#   define PRINTK(x)	printk x
#else
#   define PRINTK(x)	/**/
#endif


extern struct ddi_device	devices[];	/* device driver map	*/
extern struct ddi_proto		protocols[];	/* network protocols	*/


/*
 * This function gets called with an ASCII string representing the
 * ID of some DDI driver.  We loop through the DDI Devices table
 * and return the address of the control block that has a matching
 * "name" field.  It is used by upper-level layers that want to
 * dynamically bind some UNIX-domain "/dev/XXXX" file name to a
 * DDI device driver.  The "iflink(8)" program is an example of
 * this behaviour.
 */
struct ddi_device *
ddi_map(const char *id)
{
  register struct ddi_device *dev;

  PRINTK (("DDI: MAP: looking for \"%s\": ", id));
  dev = devices;
  while (dev->title != NULL) {
	if (strncmp(dev->name, id, DDI_MAXNAME) == 0) {
		PRINTK (("OK at 0x%X\n", dev));
		return(dev);
	}
	dev++;
  }
  PRINTK (("NOT FOUND\n"));
  return(NULL);
}


/*
 * This is the function that is called by a kernel routine during
 * system startup.  Its purpose is to walk trough the "devices"
 * table (defined above), and to call all moduled defined in it.
 */
void
ddi_init(void)
{
  struct ddi_proto *pro;
  struct ddi_device *dev;

  PRINTK (("DDI: Starting up!\n"));

  /* First off, kick all configured protocols. */
  pro = protocols;
  while (pro->name != NULL) {
	(*pro->init)(pro);
	pro++;
  }
  
  /* Done.  Now kick all configured device drivers. */
  dev = devices;
  while (dev->title != NULL) {
	(*dev->init)(dev);
	dev++;
  }

  /* We're all done... */
}
