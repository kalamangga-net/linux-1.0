#ifndef _SCSI_DEBUG_H

#include <linux/types.h>

int scsi_debug_detect(int);
int scsi_debug_command(Scsi_Cmnd *);
int scsi_debug_queuecommand(Scsi_Cmnd *, void (*done)(Scsi_Cmnd *));
int scsi_debug_abort(Scsi_Cmnd *, int);
int scsi_debug_biosparam(int, int*);
char *scsi_debug_info(void);
int scsi_debug_reset(Scsi_Cmnd *);

#ifndef NULL
	#define NULL 0
#endif

#define SCSI_DEBUG_MAILBOXES 8

#define SCSI_DEBUG {"SCSI DEBUG", scsi_debug_detect,	\
		scsi_debug_info, scsi_debug_command,		\
		scsi_debug_queuecommand,			\
		scsi_debug_abort,				\
		scsi_debug_reset,				\
		NULL,						\
		scsi_debug_biosparam,				\
		SCSI_DEBUG_MAILBOXES, 7, SG_ALL, 1, 0, 1}
#endif
