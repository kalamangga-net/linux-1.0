/*
 *	hosts.h Copyright (C) 1992 Drew Eckhardt 
 *	mid to low-level SCSI driver interface header by	
 *		Drew Eckhardt 
 *
 *	<drew@colorado.edu>
 *
 *       Modified by Eric Youngdale eric@tantalus.nrl.navy.mil to
 *       add scatter-gather, multiple outstanding request, and other
 *       enhancements.
 * 
 *	Further modified by Eric Youngdale to support multiple host adapters
 *	of the same type.
 */

#ifndef _HOSTS_H
	#define _HOSTS_H

/*
	$Header: /usr/src/linux/kernel/blk_drv/scsi/RCS/hosts.h,v 1.3 1993/09/24 12:21:00 drew Exp drew $
*/


/* A jumpstart is often required when the reset() function is called -
   many host adapters cannot do this cleanly, so they do nothing at all.
   To get the command going again, these routines set this bit in the flags
   so that a scsi_request_sense() is executed, and the command starts running
   again */

#define NEEDS_JUMPSTART 0x20

#define SG_NONE 0
#define SG_ALL 0xff

/* The various choices mean:
   NONE: Self evident.  Host adapter is not capable of scatter-gather.
   ALL:  Means that the host adapter module can do scatter-gather,
         and that there is no limit to the size of the table to which
	 we scatter/gather data.
  Anything else:  Indicates the maximum number of chains that can be
        used in one scatter-gather request.
*/

/*
	The Scsi_Host_Template type has all that is needed to interface with a SCSI
	host in a device independant matter.  There is one entry for each different
	type of host adapter that is supported on the system.
*/

typedef struct     
	{
	/*
		The name pointer is a pointer to the name of the SCSI
		device detected.
	*/

	char *name;

	/*
		The detect function shall return non zero on detection,
		indicating the number of host adapters of this particular
		type were found.  It should also
		initialize all data necessary for this particular
		SCSI driver.  It is passed the host number, so this host
		knows where the first entry is in the scsi_hosts[] array.

		Note that the detect routine MUST not call any of the mid level
		functions to queue commands because things are not guaranteed
		to be set up yet.  The detect routine can send commands to
		the host adapter as long as the program control will not be
		passed to scsi.c in the processesing of the command.  Note
		especially that scsi_malloc/scsi_free must not be called.
	*/

	int (* detect)(int); 

	/*
		The info function will return whatever useful
		information the developer sees fit.              
	*/

        const char *(* info)(void);

	/*
		The command function takes a target, a command (this is a SCSI 
		command formatted as per the SCSI spec, nothing strange), a 
		data buffer pointer, and data buffer length pointer.  The return
		is a status int, bit fielded as follows : 
		Byte	What
		0	SCSI status code
		1	SCSI 1 byte message
		2 	host error return.
		3	mid level error return
	*/

	int (* command)(Scsi_Cmnd *);

        /*
                The QueueCommand function works in a similar manner
                to the command function.  It takes an additional parameter,
                void (* done)(int host, int code) which is passed the host 
		# and exit result when the command is complete.  
		Host number is the POSITION IN THE hosts array of THIS
		host adapter.
        */

        int (* queuecommand)(Scsi_Cmnd *, void (*done)(Scsi_Cmnd *));

	/*
		Since the mid level driver handles time outs, etc, we want to 
		be able to abort the current command.  Abort returns 0 if the 
		abortion was successful.  If non-zero, the code passed to it 
		will be used as the return code, otherwise 
		DID_ABORT  should be returned.

		Note that the scsi driver should "clean up" after itself, 
		resetting the bus, etc.  if necessary. 
	*/

	int (* abort)(Scsi_Cmnd *, int);

	/*
		The reset function will reset the SCSI bus.  Any executing 
		commands should fail with a DID_RESET in the host byte.
		The Scsi_Cmnd  is passed so that the reset routine can figure
		out which host adapter should be reset, and also which command
		within the command block was responsible for the reset in
		the first place.  Some hosts do not implement a reset function,
		and these hosts must call scsi_request_sense(SCpnt) to keep
		the command alive.
	*/ 

	int (* reset)(Scsi_Cmnd *);
	/*
		This function is used to select synchronous communications,
		which will result in a higher data throughput.  Not implemented
		yet.
	*/ 

	int (* slave_attach)(int, int);
	/*
		This function determines the bios parameters for a given
		harddisk.  These tend to be numbers that are made up by
		the host adapter.  Parameters:
		size, device number, list (heads, sectors, cylinders)
	*/ 

	int (* bios_param)(int, int, int []);
	
	/*
		This determines if we will use a non-interrupt driven
		or an interrupt driven scheme,  It is set to the maximum number
		of simulataneous commands a given host adapter will accept.
	*/
	int can_queue;

	/*
		In many instances, especially where disconnect / reconnect are 
		supported, our host also has an ID on the SCSI bus.  If this is 
		the case, then it must be reserved.  Please set this_id to -1 if
 		your settup is in single initiator mode, and the host lacks an 
		ID.
	*/
	
	int this_id;

	/*
	        This determines the degree to which the host adapter is capable
		of scatter-gather.
	*/

	short unsigned int sg_tablesize;

	/*
	  True if this host adapter can make good use of linked commands.
	  This will allow more than one command to be queued to a given
	  unit on a given host.  Set this to the maximum number of command
	  blocks to be provided for each device.  Set this to 1 for one
	  command block per lun, 2 for two, etc.  Do not set this to 0.
	  You should make sure that the host adapter will do the right thing
	  before you try setting this above 1.
	 */

	short cmd_per_lun;
	/*
		present contains counter indicating how many boards of this
		type were found when we did the scan.
	*/

	unsigned char present;	
	/*
	  true if this host adapter uses unchecked DMA onto an ISA bus.
	*/
	unsigned unchecked_isa_dma:1;
	} Scsi_Host_Template;

/*
	The scsi_hosts array is	the array containing the data for all 
	possible <supported> scsi hosts.   This is similar to the
	Scsi_Host_Template, except that we have one entry for each
	actual physical host adapter on the system, stored as a linked
	list.  Note that if there are 2 aha1542 boards, then there will
	be two Scsi_Host entries, but only 1 Scsi_Host_Template entries.
*/

struct Scsi_Host
	{
		struct Scsi_Host * next;
		volatile unsigned char host_busy;
		char host_no;  /* Used for IOCTL_GET_IDLUN */
		int last_reset;
		struct wait_queue *host_wait;
		Scsi_Cmnd *host_queue; 
		Scsi_Host_Template * hostt;

		/* These parameters should be set by the detect routine */
		unsigned char *base;
		short unsigned int io_port;
		unsigned char irq;
		unsigned char dma_channel;
		/*
		  The rest can be copied from the template, or specifically
		  initialized, as required.
		  */
		
		int this_id;
		short unsigned int sg_tablesize;
		unsigned unchecked_isa_dma:1;
		int hostdata[0];  /* Used for storage of host specific stuff */
	};

extern struct Scsi_Host * scsi_hostlist;

extern Scsi_Host_Template scsi_hosts[];

/*
	scsi_init initializes the scsi hosts.
*/


unsigned int scsi_init(unsigned long memory_start,unsigned long memory_end);
extern struct Scsi_Host * scsi_register(int i, int j);
extern void scsi_unregister(struct Scsi_Host * i, int j);

#define BLANK_HOST {"", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
#endif
