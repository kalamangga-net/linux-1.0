/* fdomain.h -- Header for Future Domain TMC-16x0 driver
 * Created: Sun May  3 18:47:33 1992 by faith@cs.unc.edu
 * Revised: Tue Jan  4 20:44:04 1994 by faith@cs.unc.edu
 * Author: Rickard E. Faith, faith@cs.unc.edu
 * Copyright 1992, 1993, 1994 Rickard E. Faith
 *
 * $Id: fdomain.h,v 5.3 1994/01/05 01:44:16 root Exp $

 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.

 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.

 */

#ifndef _FDOMAIN_H
#define _FDOMAIN_H

int        fdomain_16x0_detect( int );
int        fdomain_16x0_command( Scsi_Cmnd * );
int        fdomain_16x0_abort( Scsi_Cmnd *, int );
const char *fdomain_16x0_info( void );
int        fdomain_16x0_reset( Scsi_Cmnd * ); 
int        fdomain_16x0_queue( Scsi_Cmnd *, void (*done)(Scsi_Cmnd *) );
int        fdomain_16x0_biosparam( int, int, int * );

#define FDOMAIN_16X0 { "Future Domain TMC-16x0",          \
			fdomain_16x0_detect,              \
		        fdomain_16x0_info,                \
		        fdomain_16x0_command,             \
		        fdomain_16x0_queue,               \
		        fdomain_16x0_abort,               \
		        fdomain_16x0_reset,               \
		        NULL,                             \
		        fdomain_16x0_biosparam,           \
		        1, 6, 64 /* SG_NONE */, 1 ,0, 0 }
#endif
