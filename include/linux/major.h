#ifndef _LINUX_MAJOR_H
#define _LINUX_MAJOR_H

/*
 * This file has definitions for major device numbers
 */

/* limits */

#define MAX_CHRDEV 32
#define MAX_BLKDEV 32

/*
 * assignments
 *
 * devices are as follows (same as minix, so we can use the minix fs):
 *
 *      character              block                  comments
 *      --------------------   --------------------   --------------------
 *  0 - unnamed                unnamed                minor 0 = true nodev
 *  1 - /dev/mem               ramdisk
 *  2 -                        floppy
 *  3 -                        hd
 *  4 - /dev/tty*
 *  5 - /dev/tty; /dev/cua*
 *  6 - lp
 *  7 -                                               UNUSED
 *  8 -                        scsi disk
 *  9 - scsi tape
 * 10 - mice
 * 11 -                        scsi cdrom
 * 12 - qic02 tape
 * 13 -                        xt disk
 * 14 - sound card
 * 15 -                        cdu31a cdrom
 * 16 - sockets
 * 17 - af_unix
 * 18 - af_inet
 * 19 -                                               UNUSED
 * 20 -                                               UNUSED
 * 21 - scsi generic
 * 22 -                        (at2disk)
 * 23 -                        mitsumi cdrom
 * 24 -	                       sony535 cdrom
 * 25 -                        matsushita cdrom       minors 0..3
 * 26 -
 * 27 - qic117 tape
 */

#define UNNAMED_MAJOR	0
#define MEM_MAJOR	1
#define FLOPPY_MAJOR	2
#define HD_MAJOR	3
#define TTY_MAJOR	4
#define TTYAUX_MAJOR	5
#define LP_MAJOR	6
/* unused: 7 */
#define SCSI_DISK_MAJOR	8
#define SCSI_TAPE_MAJOR	9
#define MOUSE_MAJOR	10
#define SCSI_CDROM_MAJOR 11
#define QIC02_TAPE_MAJOR 12
#define XT_DISK_MAJOR	13
#define SOUND_MAJOR	14
#define CDU31A_CDROM_MAJOR 15
#define SOCKET_MAJOR	16
#define AF_UNIX_MAJOR	17
#define AF_INET_MAJOR	18
/* unused: 19, 20 */
#define SCSI_GENERIC_MAJOR 21
/* unused: 22 */
#define MITSUMI_CDROM_MAJOR 23
#define CDU535_CDROM_MAJOR 24
#define MATSUSHITA_CDROM_MAJOR 25
#define QIC117_TAPE_MAJOR 27

/*
 * Tests for SCSI devices.
 */

#define SCSI_MAJOR(M) \
  ((M) == SCSI_DISK_MAJOR	\
   || (M) == SCSI_TAPE_MAJOR	\
   || (M) == SCSI_CDROM_MAJOR	\
   || (M) == SCSI_GENERIC_MAJOR)

static inline int scsi_major(int m) {
	return SCSI_MAJOR(m);
}

#endif
