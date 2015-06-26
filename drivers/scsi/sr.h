/*
 *      sr.h by David Giller
 *      CD-ROM disk driver header file
 *      
 *      adapted from:
 *	sd.h Copyright (C) 1992 Drew Eckhardt 
 *	SCSI disk driver header file by
 *		Drew Eckhardt 
 *
 *	<drew@colorado.edu>
 *
 *       Modified by Eric Youngdale eric@tantalus.nrl.navy.mil to
 *       add scatter-gather, multiple outstanding request, and other
 *       enhancements.
 */

#ifndef _SR_H
#define _SR_H

#include "scsi.h"

typedef struct
	{
	unsigned 	capacity;		/* size in blocks 			*/
	unsigned 	sector_size;		/* size in bytes 			*/
	Scsi_Device  	*device;		
	unsigned char	sector_bit_size;	/* sector size = 2^sector_bit_size	*/
	unsigned char	sector_bit_shift;	/* sectors/FS block = 2^sector_bit_shift*/
	unsigned 	needs_sector_size:1;   	/* needs to get sector size */
	unsigned 	ten:1;			/* support ten byte commands		*/
	unsigned 	remap:1;		/* support remapping			*/
	unsigned 	use:1;			/* is this device still supportable	*/
	} Scsi_CD;
	
extern Scsi_CD * scsi_CDs;

#endif
