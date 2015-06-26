/*
 *	hosts.c Copyright (C) 1992 Drew Eckhardt 
 *	mid to lowlevel SCSI driver interface by
 *		Drew Eckhardt 
 *
 *	<drew@colorado.edu>
 */


/*
 *	This file contains the medium level SCSI
 *	host interface initialization, as well as the scsi_hosts array of SCSI
 *	hosts currently present in the system. 
 */

#include <linux/config.h>
#include "../block/blk.h"
#include <linux/kernel.h>
#include <linux/string.h>
#include "scsi.h"

#ifndef NULL 
#define NULL 0L
#endif

#define HOSTS_C

#include "hosts.h"

#ifdef CONFIG_SCSI_AHA152X
#include "aha152x.h"
#endif

#ifdef CONFIG_SCSI_AHA1542
#include "aha1542.h"
#endif

#ifdef CONFIG_SCSI_AHA1740
#include "aha1740.h"
#endif

#ifdef CONFIG_SCSI_FUTURE_DOMAIN
#include "fdomain.h"
#endif

#ifdef CONFIG_SCSI_GENERIC_NCR5380
#include "g_NCR5380.h"
#endif

#ifdef CONFIG_SCSI_PAS16
#include "pas16.h"
#endif

#ifdef CONFIG_SCSI_SEAGATE
#include "seagate.h"
#endif

#ifdef CONFIG_SCSI_T128
#include "t128.h"
#endif

#ifdef CONFIG_SCSI_ULTRASTOR
#include "ultrastor.h"
#endif

#ifdef CONFIG_SCSI_7000FASST
#include "wd7000.h"
#endif

#ifdef CONFIG_SCSI_DEBUG
#include "scsi_debug.h"
#endif

/*
static const char RCSid[] = "$Header: /usr/src/linux/kernel/blk_drv/scsi/RCS/hosts.c,v 1.3 1993/09/24 12:21:00 drew Exp drew $";
*/

/*
 *	The scsi host entries should be in the order you wish the 
 *	cards to be detected.  A driver may appear more than once IFF
 *	it can deal with being detected (and therefore initialized) 
 *	with more than one simulatenous host number, can handle being
 *	rentrant, etc.
 *
 *	They may appear in any order, as each SCSI host  is told which host number it is
 *	during detection.
 */

/* This is a placeholder for controllers that are not configured into
   the system - we do this to ensure that the controller numbering is
   always consistent, no matter how the kernel is configured. */

#define NO_CONTROLLER {NULL, NULL, NULL, NULL, NULL, NULL, NULL, \
	        NULL, NULL, 0, 0, 0, 0, 0, 0}

/*
 *	When figure is run, we don't want to link to any object code.  Since 
 *	the macro for each host will contain function pointers, we cannot 
 *	use it and instead must use a "blank" that does no such 
 *	idiocy.
 */

Scsi_Host_Template scsi_hosts[] =
	{
#ifdef CONFIG_SCSI_AHA152X
	AHA152X,
#endif
#ifdef CONFIG_SCSI_AHA1542
	AHA1542,
#endif
#ifdef CONFIG_SCSI_AHA1740
	AHA1740,
#endif
#ifdef CONFIG_SCSI_FUTURE_DOMAIN
	FDOMAIN_16X0,
#endif
#ifdef CONFIG_SCSI_GENERIC_NCR5380
        GENERIC_NCR5380,
#endif
#ifdef CONFIG_SCSI_PAS16
	MV_PAS16,
#endif
#ifdef CONFIG_SCSI_SEAGATE
	SEAGATE_ST0X,
#endif
#ifdef CONFIG_SCSI_T128
        TRANTOR_T128,
#endif
#ifdef CONFIG_SCSI_ULTRASTOR
	ULTRASTOR_14F,
#endif
#ifdef CONFIG_SCSI_7000FASST
	WD7000,
#endif
#ifdef CONFIG_SCSI_DEBUG
	SCSI_DEBUG,
#endif
	};

#define MAX_SCSI_HOSTS (sizeof(scsi_hosts) / sizeof(Scsi_Host_Template))

/*
 *	Our semaphores and timeout counters, where size depends on MAX_SCSI_HOSTS here. 
 */

struct Scsi_Host * scsi_hostlist = NULL;

static int scsi_init_memory_start = 0;

int max_scsi_hosts = 0;
static int next_host = 0;

void
scsi_unregister(struct Scsi_Host * sh, int j){
	struct Scsi_Host * shpnt;

	if(((unsigned int) sh) + sizeof(struct Scsi_Host) + j != scsi_init_memory_start)
		panic("Unable to unregister scsi host");
	if(scsi_hostlist == sh)
		scsi_hostlist = NULL;
	else {
		shpnt = scsi_hostlist;
		while(shpnt->next != sh) shpnt = shpnt->next;
		shpnt->next = shpnt->next->next;

	};
	next_host--;
	scsi_init_memory_start = (unsigned int) sh;
}

/* We call this when we come across a new host adapter. We only do this
   once we are 100% sure that we want to use this host adapter -  it is a
   pain to reverse this, so we try and avoid it */

struct Scsi_Host * scsi_register(int i, int j){
	struct Scsi_Host * retval, *shpnt;
	retval = (struct Scsi_Host*) scsi_init_memory_start;
	scsi_init_memory_start += sizeof(struct Scsi_Host) + j;
	retval->host_busy = 0;
	retval->host_no = next_host++;
	retval->host_queue = NULL;	
	retval->host_wait = NULL;	
	retval->last_reset = 0;	
	retval->hostt = &scsi_hosts[i];	
	retval->next = NULL;
#ifdef DEBUG
	printk("Register %x %x: %d %d\n", retval, retval->hostt, i, j);
#endif

	/* The next three are the default values which can be overridden
	   if need be */
	retval->this_id = scsi_hosts[i].this_id;
	retval->sg_tablesize = scsi_hosts[i].sg_tablesize;
	retval->unchecked_isa_dma = scsi_hosts[i].unchecked_isa_dma;

	if(!scsi_hostlist)
		scsi_hostlist = retval;
	else
	{
		shpnt = scsi_hostlist;
		while(shpnt->next) shpnt = shpnt->next;
		shpnt->next = retval;
	}

	return retval;
}

unsigned int
scsi_init(unsigned long memory_start,unsigned long memory_end)
{
	static int called = 0;
	int i, j, count, pcount;

	count = 0;

	if(called) return memory_start;

	scsi_init_memory_start = memory_start;
	called = 1;	
	for (i = 0; i < MAX_SCSI_HOSTS; ++i)
	{
		/*
		 * Initialize our semaphores.  -1 is interpreted to mean 
		 * "inactive" - where as 0 will indicate a time out condition.
		 */ 
		
		pcount = next_host;
		if ((scsi_hosts[i].detect) && 
		    (scsi_hosts[i].present = 
		     scsi_hosts[i].detect(i)))
		{		
			/* The only time this should come up is when people use
			   some kind of patched driver of some kind or another. */
			if(pcount == next_host) {
				if(scsi_hosts[i].present > 1)
					panic("Failure to register low-level scsi driver");
				/* The low-level driver failed to register a driver.  We
				   can do this now. */
				scsi_register(i,0);
			};
			for(j = 0; j < scsi_hosts[i].present; j++)
				printk ("scsi%d : %s\n",
					count++, scsi_hosts[i].name);
		}
	}
	printk ("scsi : %d hosts.\n", count);
	
	max_scsi_hosts = count;
	return scsi_init_memory_start;
}

#ifndef CONFIG_BLK_DEV_SD
unsigned long sd_init(unsigned long memory_start, unsigned long memory_end){
  return memory_start;
};
unsigned long sd_init1(unsigned long memory_start, unsigned long memory_end){
  return memory_start;
};
void sd_attach(Scsi_Device * SDp){
};
int NR_SD=-1;
int MAX_SD=0;
#endif


#ifndef CONFIG_BLK_DEV_SR
unsigned long sr_init(unsigned long memory_start, unsigned long memory_end){
  return memory_start;
};
unsigned long sr_init1(unsigned long memory_start, unsigned long memory_end){
  return memory_start;
};
void sr_attach(Scsi_Device * SDp){
};
int NR_SR=-1;
int MAX_SR=0;
#endif


#ifndef CONFIG_CHR_DEV_ST
unsigned long st_init(unsigned long memory_start, unsigned long memory_end){
  return memory_start;
};
unsigned long st_init1(unsigned long memory_start, unsigned long memory_end){
  return memory_start;
};
void st_attach(Scsi_Device * SDp){
};
int NR_ST=-1;
int MAX_ST=0;
#endif

#ifndef CONFIG_CHR_DEV_SG
unsigned long sg_init(unsigned long memory_start, unsigned long memory_end){
  return memory_start;
};
unsigned long sg_init1(unsigned long memory_start, unsigned long memory_end){
  return memory_start;
};
void sg_attach(Scsi_Device * SDp){
};
int NR_SG=-1;
int MAX_SG=0;
#endif
