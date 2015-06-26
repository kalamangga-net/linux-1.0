/*
 *  sbpcd.c   CD-ROM device driver for the whole family of IDE-style
 *            Kotobuki/Matsushita/Panasonic CR-5xx drives for
 *            SoundBlaster ("Pro" or "16 ASP" or compatible) cards
 *            and for "no-sound" interfaces like Lasermate and the
 *            Panasonic CI-101P.
 *
 *  NOTE:     This is release 1.3.
 *            It works with my SbPro & drive CR-521 V2.11 from 2/92
 *            and with the new CR-562-B V0.75 on a "naked" Panasonic
 *            CI-101P interface. And vice versa. 
 *  
 *
 *  VERSION HISTORY
 *
 *  0.1  initial release, April/May 93, after mcd.c (Martin Harriss)
 *
 *  0.2  the "repeat:"-loop in do_sbpcd_request did not check for
 *       end-of-request_queue (resulting in kernel panic).
 *       Flow control seems stable, but throughput is not better.  
 *
 *  0.3  interrupt locking totally eliminated (maybe "inb" and "outb"
 *       are still locking) - 0.2 made keyboard-type-ahead losses.
 *       check_sbpcd_media_change added (to use by isofs/inode.c)
 *       - but it detects almost nothing.
 *
 *  0.4  use MAJOR 25 definitely.
 *       Almost total re-design to support double-speed drives and
 *       "naked" (no sound) interface cards.
 *       Flow control should be exact now (tell me if not).
 *       Don't occupy the SbPro IRQ line (not needed either); will
 *       live together with Hannu Savolainen's sndkit now.
 *	 Speeded up data transfer to 150 kB/sec, with help from Kai
 *       Makisara, the "provider" of the "mt" tape utility.
 *       Give "SpinUp" command if necessary.
 *       First steps to support up to 4 drives (but currently only one).
 *       Implemented audio capabilities - workman should work, xcdplayer
 *       gives some problems.
 *       This version is still consuming too much CPU time, and
 *       sleeping still has to be worked on.
 *       During "long" implied seeks, it seems possible that a 
 *       ReadStatus command gets ignored. That gives the message
 *       "ResponseStatus timed out" (happens about 6 times here during
 *       a "ls -alR" of the YGGDRASIL LGX-Beta CD). Such a case is
 *       handled without data error, but it should get done better.
 *
 *  0.5  Free CPU during waits (again with help from Kai Makisara).
 *       Made it work together with the LILO/kernel setup standard.
 *       Included auto-probing code, as suggested by YGGDRASIL.
 *       Formal redesign to add DDI debugging.
 *       There are still flaws in IOCTL (workman with double speed drive).
 *
 *  1.0  Added support for all drive ids (0...3, no longer only 0)
 *       and up to 4 drives on one controller.
 *       Added "#define MANY_SESSION" for "old" multi session CDs.
 *
 *  1.1  Do SpinUp for new drives, too.
 *       Revised for clean compile under "old" kernels (pl9).
 *
 *  1.2  Found the "workman with double-speed drive" bug: use the driver's
 *       audio_state, not what the drive is reporting with ReadSubQ.
 *
 *  1.3  Minor cleanups.
 *       Refinements regarding Workman.
 *
 *     special thanks to Kai Makisara (kai.makisara@vtt.fi) for his fine
 *     elaborated speed-up experiments (and the fabulous results!), for
 *     the "push" towards load-free wait loops, and for the extensive mail
 *     thread which brought additional hints and bug fixes.
 * 
 *
 *   Copyright (C) 1993, 1994  Eberhard Moenkeberg <emoenke@gwdg.de>
 *                         or <eberhard_moenkeberg@rollo.central.de>
 *
 *                  The FTP-home of this driver is 
 *                  ftp.gwdg.de:/pub/linux/cdrom/drivers/sbpcd/.
 *
 *                  If you change this software, you should mail a .diff
 *                  file with some description lines to emoenke@gwdg.de.
 *                  I want to know about it.
 *
 *                  If you are the editor of a Linux CD, you should
 *                  enable sbpcd.c within your boot floppy kernel and
 *                  send me one of your CDs for free.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2, or (at your option)
 *   any later version.
 *
 *   You should have received a copy of the GNU General Public License
 *   (for example /usr/src/linux/COPYING); if not, write to the Free
 *   Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <linux/config.h>
#include <linux/errno.h>

#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/cdrom.h>
#include <linux/ioport.h>
#include <linux/sbpcd.h>

#if SBPCD_USE_IRQ
#include <linux/signal.h>
#endif SBPCD_USE_IRQ

#include <linux/ddi.h>
#include <linux/major.h> 

#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>
#include <stdarg.h>

#define MAJOR_NR MATSUSHITA_CDROM_MAJOR
#include "blk.h"

#define VERSION "1.3 Eberhard Moenkeberg <emoenke@gwdg.de>"

#define SBPCD_DEBUG

#ifndef CONFIG_ISO9660_FS
#error "SBPCD: \"make config\" again. File system iso9660 is necessary."
#endif

/*
 * still testing around...
 */
#define MANY_SESSION 0
#define CDMKE
#undef  FUTURE
#define WORKMAN 1 /* some testing stuff to make it better */

/*==========================================================================*/
/*==========================================================================*/
/*
 * auto-probing address list
 * inspired by Adam J. Richter from Yggdrasil
 *
 * still not good enough - can cause a hang.
 *   example: a NE 2000 ethernet card at 300 will cause a hang probing 310.
 * if that happens, reboot and use the LILO (kernel) command line.
 * The possibly conflicting ethernet card addresses get NOT probed 
 * by default - to minimize the hang possibilities. 
 *
 * The SB Pro addresses get "mirrored" at 0x6xx - to avoid a type error,
 * the 0x2xx-addresses must get checked before 0x6xx.
 *
 * send mail to emoenke@gwdg.de if your interface card is not FULLY
 * represented here.
 */
static int autoprobe[] = 
{
  CDROM_PORT, SBPRO, /* probe with user's setup first */
  0x230, 1, /* Soundblaster Pro and 16 (default) */
  0x300, 0, /* CI-101P (default), Galaxy (default), Reveal (one default) */
  0x250, 1, /* OmniCD default, Soundblaster Pro and 16 */
  0x260, 1, /* OmniCD */
  0x320, 0, /* Lasermate, CI-101P, Galaxy, Reveal (other default) */
  0x340, 0, /* Lasermate, CI-101P */
  0x360, 0, /* Lasermate, CI-101P */
  0x270, 1, /* Soundblaster 16 */
  0x630, 0, /* "sound card #9" (default) */
  0x650, 0, /* "sound card #9" */
  0x670, 0, /* "sound card #9" */
  0x690, 0, /* "sound card #9" */
#if 0
/* some "hazardous" locations (ethernet cards) */
  0x330, 0, /* Lasermate, CI-101P */
  0x350, 0, /* Lasermate, CI-101P */
  0x370, 0, /* Lasermate, CI-101P */
  0x290, 1, /* Soundblaster 16 */
  0x310, 0, /* Lasermate, CI-101P */
#endif
};

#define NUM_AUTOPROBE  (sizeof(autoprobe) / sizeof(int))


/*==========================================================================*/
/*
 * the forward references:
 */
static void sbp_read_cmd(void);
static int  sbp_data(void);

/*==========================================================================*/

/*
 * pattern for printk selection:
 *
 * (1<<DBG_INF)  necessary information
 * (1<<DBG_IRQ)  interrupt trace
 * (1<<DBG_REA)  "read" status trace
 * (1<<DBG_CHK)  "media check" trace
 * (1<<DBG_TIM)  datarate timer test
 * (1<<DBG_INI)  initialization trace
 * (1<<DBG_TOC)  tell TocEntry values
 * (1<<DBG_IOC)  ioctl trace
 * (1<<DBG_STA)  "ResponseStatus" trace
 * (1<<DBG_ERR)  "xx_ReadError" trace
 * (1<<DBG_CMD)  "cmd_out" trace
 * (1<<DBG_WRN)  give explanation before auto-probing
 * (1<<DBG_MUL)  multi session code test
 * (1<<DBG_ID)   "drive_id != 0" test code
 * (1<<DBG_IOX)  some special information
 * (1<<DBG_DID)  drive ID test
 * (1<<DBG_RES)  drive reset info
 * (1<<DBG_SPI)  SpinUp test info
 * (1<<DBG_IOS)  ioctl trace: "subchannel"
 * (1<<DBG_IO2)  ioctl trace: general
 * (1<<DBG_000)  unnecessary information
 */
#if 1
static int sbpcd_debug =  (1<<DBG_INF) | (1<<DBG_WRN);
#else
static int sbpcd_debug =  (1<<DBG_INF) |
                          (1<<DBG_TOC) |
                          (1<<DBG_IOC) |
                          (1<<DBG_IOX);
#endif
static int sbpcd_ioaddr = CDROM_PORT;	/* default I/O base address */
static int sbpro_type = SBPRO;
static int CDo_command, CDo_reset;
static int CDo_sel_d_i, CDo_enable;
static int CDi_info, CDi_status, CDi_data;
static int MIXER_addr, MIXER_data;
static struct cdrom_msf msf;
static struct cdrom_ti ti;
static struct cdrom_tochdr tochdr;
static struct cdrom_tocentry tocentry;
static struct cdrom_subchnl SC;
static struct cdrom_volctrl volctrl;
char *str_sb = "SoundBlaster";
char *str_lm = "LaserMate";
char *type;

/*==========================================================================*/

#if FUTURE
static struct wait_queue *sbp_waitq = NULL;
#endif FUTURE

/*==========================================================================*/

#define SBP_BUFFER_FRAMES 4 /* driver's own read_ahead */

/*==========================================================================*/

static u_char drive_family[]="CR-5";
static u_char drive_vendor[]="MATSHITA";

static u_int response_count=0;
static u_int flags_cmd_out;
static u_char cmd_type=0;
static u_char drvcmd[7];
static u_char infobuf[20];

static u_char timed_out=0;
static u_int datarate= 1000000;
static u_int maxtim16=16000000;
static u_int maxtim04= 4000000;
static u_int maxtim02= 2000000;
static u_int maxtim_8=   30000;
#if MANY_SESSION
static u_int maxtim_data= 9000;
#else
static u_int maxtim_data= 3000;
#endif MANY_SESSION

/*==========================================================================*/

static int ndrives=0;
static u_char drv_pattern[4]={ 0x80, 0x80, 0x80, 0x80 }; /* auto speed */
/*  /X:... drv_pattern[0] |= (sax_n1|sax_n2);         */
/*  /A:... for (i=0;i<4;i++) drv_pattern[i] |= sax_a; */
/*  /N:... ndrives=i-'0';                             */

/*==========================================================================*/
/*
 * drive space begins here (needed separate for each unit) 
 */
static int d=0; /* DS index: drive number */

static struct {
  char drv_minor; /* minor number or -1 */

  char drive_model[4];
  char firmware_version[4];
  u_char *sbp_buf; /* Pointer to internal data buffer,
                           space allocated during sbpcd_init() */
  int sbp_first_frame;  /* First frame in buffer */
  int sbp_last_frame;   /* Last frame in buffer  */
  int sbp_read_frames;   /* Number of frames being read to buffer */
  int sbp_current;       /* Frame being currently read */

  u_char drv_type;
  u_char drv_options;
  u_char status_byte;
  u_char diskstate_flags;
  u_char sense_byte;
  
  u_char CD_changed;
  
  u_char error_byte;
  
  u_char f_multisession;
  u_int lba_multi;
  
  u_char audio_state;
  u_int pos_audio_start;
  u_int pos_audio_end;
  char vol_chan0;
  u_char vol_ctrl0;
  char vol_chan1;
  u_char vol_ctrl1;
#if 000
  char vol_chan2;
  u_char vol_ctrl2;
  char vol_chan3;
  u_char vol_ctrl3;
#endif 000
  
  u_char SubQ_audio;
  u_char SubQ_ctl_adr;
  u_char SubQ_trk;
  u_char SubQ_pnt_idx;
  u_int SubQ_run_tot;
  u_int SubQ_run_trk;
  u_char SubQ_whatisthis;
  
  u_char UPC_ctl_adr;
  u_char UPC_buf[7];
  
  int CDsize_blk;
  int frame_size;
  int CDsize_frm;
  
  u_char xa_byte; /* 0x20: XA capabilities */
  u_char n_first_track; /* binary */
  u_char n_last_track; /* binary (not bcd), 0x01...0x63 */
  u_int size_msf; /* time of whole CD, position of LeadOut track */
  u_int size_blk;
  
  u_char TocEnt_nixbyte; /* em */
  u_char TocEnt_ctl_adr;
  u_char TocEnt_number;
  u_char TocEnt_format; /* em */
  u_int TocEnt_address;
  u_char ored_ctl_adr; /* to detect if CDROM contains data tracks */
  
  struct {
    u_char nixbyte; /* em */
    u_char ctl_adr; /* 0x4x: data, 0x0x: audio */
    u_char number;
    u_char format; /* em */ /* 0x00: lba, 0x01: msf */
    u_int address;
  } TocBuffer[MAX_TRACKS+1]; /* last entry faked */ 
  
  int in_SpinUp;
  
} DS[4];

/*
 * drive space ends here (needed separate for each unit)
 */

/*==========================================================================*/
/*==========================================================================*/
/*
 * DDI interface definitions
 */
#ifdef SBPCD_DEBUG
# define DPRINTF(x)	sbpcd_dprintf x

void sbpcd_dprintf(int level, char *fmt, ...)
{
  char buff[256];
  va_list args;
  extern int vsprintf(char *buf, const char *fmt, va_list args);

  if (! (sbpcd_debug & (1 << level))) return;

  va_start(args, fmt);
  vsprintf(buff, fmt, args);
  va_end(args);
  printk(buff);
}

#else
# define DPRINTF(x)	/* nothing */

#endif SBPCD_DEBUG

/*
 * maintain trace bit pattern
 */
static int sbpcd_dbg_ioctl(unsigned long arg, int level)
{
  int val;

  val = get_fs_long((int *) arg);
  switch(val)
    {
    case 0:	/* OFF */
      sbpcd_debug = 0;
      break;

    default:
      if (val >= 128) sbpcd_debug &= ~(1 << (val - 128));
      else sbpcd_debug |= (1 << val);
    }
  return(0);
}


/*==========================================================================*/
/*==========================================================================*/
/*
 * Wait a little while (used for polling the drive).  If in initialization,
 * setting a timeout doesn't work, so just loop for a while.
 */
static inline void sbp_sleep(u_int jifs)
{
   current->state = TASK_INTERRUPTIBLE;
   current->timeout = jiffies + jifs;
   schedule();
}

/*==========================================================================*/
/*==========================================================================*/
/*
 *  convert logical_block_address to m-s-f_number (3 bytes only)
 */
static void lba2msf(int lba, u_char *msf)
{
  lba += CD_BLOCK_OFFSET;
  msf[0] = lba / (CD_SECS*CD_FRAMES);
  lba %= CD_SECS*CD_FRAMES;
  msf[1] = lba / CD_FRAMES;
  msf[2] = lba % CD_FRAMES;
}
/*==========================================================================*/
/*==========================================================================*/
/*
 *  convert msf-bin to msf-bcd
 */
static void bin2bcdx(u_char *p)  /* must work only up to 75 or 99 */
{
  *p=((*p/10)<<4)|(*p%10);
}
/*==========================================================================*/
static u_int blk2msf(u_int blk)
{
  MSF msf;
  u_int mm;

  msf.c[3] = 0;
  msf.c[2] = (blk + CD_BLOCK_OFFSET) / (CD_SECS * CD_FRAMES);
  mm = (blk + CD_BLOCK_OFFSET) % (CD_SECS * CD_FRAMES);
  msf.c[1] = mm / CD_FRAMES;
  msf.c[0] = mm % CD_FRAMES;
  return (msf.n);
}
/*==========================================================================*/
static u_int make16(u_char rh, u_char rl)
{
  return ((rh<<8)|rl);
}
/*==========================================================================*/
static u_int make32(u_int rh, u_int rl)
{
  return ((rh<<16)|rl);
}
/*==========================================================================*/
static u_char swap_nibbles(u_char i)
{
  return ((i<<4)|(i>>4));
}
/*==========================================================================*/
static u_char byt2bcd(u_char i)
{
  return (((i/10)<<4)+i%10);
}
/*==========================================================================*/
static u_char bcd2bin(u_char bcd)
{
  return ((bcd>>4)*10+(bcd&0x0F));
}
/*==========================================================================*/
static int msf2blk(int msfx)
{
  MSF msf;
  int i;

  msf.n=msfx;
  i=(msf.c[2] * CD_SECS + msf.c[1]) * CD_FRAMES + msf.c[0] - CD_BLOCK_OFFSET;
  if (i<0) return (0);
  return (i);
}
/*==========================================================================*/
/* evaluate xx_ReadError code (still mysterious) */ 
static int sta2err(int sta)
{
  if (sta<=2) return (sta);
  if (sta==0x05) return (-4);
  if (sta==0x06) return (-6);
  if (sta==0x0d) return (-6);
  if (sta==0x0e) return (-3);
  if (sta==0x14) return (-3);
  if (sta==0x0c) return (-11);
  if (sta==0x0f) return (-11);
  if (sta==0x10) return (-11);
  if (sta>=0x16) return (-12);
  DS[d].CD_changed=0xFF;
  if (sta==0x11) return (-15);
  return (-2);
}
/*==========================================================================*/
static void clr_cmdbuf(void)
{
  int i;

  for (i=0;i<7;i++) drvcmd[i]=0;
  cmd_type=0;
}
/*==========================================================================*/
static void mark_timeout(void)
{
  timed_out=1;
  DPRINTF((DBG_TIM,"SBPCD: timer stopped.\n"));
}
/*==========================================================================*/
static void flush_status(void)
{
#ifdef CDMKE
  int i;

  if (current == task[0])
    for (i=maxtim02;i!=0;i--) inb(CDi_status);
  else 
    {
      sbp_sleep(150);
      for (i=maxtim_data;i!=0;i--) inb(CDi_status);
    }
#else
  timed_out=0;
  SET_TIMER(mark_timeout,150);
  do { }
  while (!timed_out);
  CLEAR_TIMER;
  inb(CDi_status);
#endif CDMKE
}
/*==========================================================================*/
static int CDi_stat_loop(void)
{
  int i,j;
  u_long timeout;
  
  if (current == task[0])
    for(i=maxtim16;i!=0;i--)
      {
	j=inb(CDi_status);
        if (!(j&s_not_data_ready)) return (j);
        if (!(j&s_not_result_ready)) return (j);
        if (!new_drive) if (j&s_attention) return (j);
      }
  else
    for(timeout = jiffies + 1000, i=maxtim_data; timeout > jiffies; )
      {
	for ( ;i!=0;i--)
	  {
	    j=inb(CDi_status);
	    if (!(j&s_not_data_ready)) return (j);
	    if (!(j&s_not_result_ready)) return (j);
	    if (!new_drive) if (j&s_attention) return (j);
	  }
	sbp_sleep(1);
	i = 1;
      }
  return (-1);
}
/*==========================================================================*/
static int ResponseInfo(void)
{
  int i,j, st=0;
  u_long timeout;

  if (current == task[0])
    for (i=0;i<response_count;i++)
      {
	for (j=maxtim_8;j!=0;j--)
	  {
	    st=inb(CDi_status);
	    if (!(st&s_not_result_ready)) break;
	  }
	if (j==0) return (-1);
	infobuf[i]=inb(CDi_info);
      }
  else 
    {
      for (i=0, timeout = jiffies + 100; i < response_count; i++) 
	{
	  for (j=maxtim_data; ; )
	    {
	      for ( ;j!=0;j-- )
		{
		  st=inb(CDi_status);
		  if (!(st&s_not_result_ready)) break;
		}
	      if (j != 0 || timeout <= jiffies) break;
	      sbp_sleep(0);
	      j = 1;
	    }
	  if (timeout <= jiffies) return (-1);
	  infobuf[i]=inb(CDi_info);
	}
    }
  return (0);
}
/*==========================================================================*/
static int EvaluateStatus(int st)
{
  if (!new_drive)
    {
      DS[d].status_byte=0;
      if (st&p_caddin_old) DS[d].status_byte |= p_door_closed|p_caddy_in;
      if (st&p_spinning) DS[d].status_byte |= p_spinning;
      if (st&p_check) DS[d].status_byte |= p_check;
      if (st&p_busy_old) DS[d].status_byte |= p_busy_new;
      if (st&p_disk_ok) DS[d].status_byte |= p_disk_ok;
    }
  else { DS[d].status_byte=st;
	 st=p_success_old; /* for new drives: fake "successful" bit of old drives */
       }
  return (st);
}
/*==========================================================================*/
static int ResponseStatus(void)
{
  int i,j;
  u_long timeout;

  DPRINTF((DBG_STA,"SBPCD: doing ResponseStatus...\n"));

  if (current == task[0])
    {
      if (flags_cmd_out & f_respo3) j = maxtim_8;
      else if (flags_cmd_out&f_respo2) j=maxtim16;
      else j=maxtim04;
      for (;j!=0;j--)
	{
	  i=inb(CDi_status);
	  if (!(i&s_not_result_ready)) break;
	}
    }
  else
    {
      if (flags_cmd_out & f_respo3) timeout = jiffies;
      else if (flags_cmd_out & f_respo2) timeout = jiffies + 1600;
      else timeout = jiffies + 400;
      j=maxtim_8;
      do
	{
	  for ( ;j!=0;j--)
	    { 
	      i=inb(CDi_status);
	      if (!(i&s_not_result_ready)) break;
	    }
	  if (j != 0 || timeout <= jiffies) break;
	  sbp_sleep(0);
	  j = 1;
	}
      while (1);
    }
  if (j==0) 
    { if ((flags_cmd_out & f_respo3) == 0)
	DPRINTF((DBG_STA,"SBPCD: ResponseStatus: timeout.\n"));
      EvaluateStatus(0);
      return (-1);
    }
  i=inb(CDi_info);
  i=EvaluateStatus(i);
  return (i);
}
/*==========================================================================*/
static void xx_ReadStatus(void)
{
  int i;

  DPRINTF((DBG_STA,"SBPCD: giving xx_ReadStatus command\n"));

  if (!new_drive) OUT(CDo_command,0x81);
  else
    {
#if SBPCD_DIS_IRQ
      cli();
#endif SBPCD_DIS_IRQ
      OUT(CDo_command,0x05);
      for (i=0;i<6;i++) OUT(CDo_command,0);
#if SBPCD_DIS_IRQ
      sti();
#endif SBPCD_DIS_IRQ
    }
}
/*==========================================================================*/
int xx_ReadError(void)
{
  int cmd_out(void);
  int i;

  clr_cmdbuf();
  DPRINTF((DBG_ERR,"SBPCD: giving xx_ReadError command.\n"));
  if (new_drive)
    {
      drvcmd[0]=0x82;
      response_count=8;
      flags_cmd_out=f_putcmd|f_ResponseStatus;
    }
  else
    {
      drvcmd[0]=0x82;
      response_count=6;
      flags_cmd_out=f_putcmd|f_getsta|f_ResponseStatus;
    }
  i=cmd_out();
  DS[d].error_byte=0;
  DPRINTF((DBG_ERR,"SBPCD: xx_ReadError: cmd_out(82) returns %d (%02X)\n",i,i));
  if (i<0) return (i);
  if (new_drive) i=2;
  else i=1;
  DS[d].error_byte=infobuf[i];
  DPRINTF((DBG_ERR,"SBPCD: xx_ReadError: infobuf[%d] is %d (%02X)\n",i,DS[d].error_byte,DS[d].error_byte));
  i=sta2err(infobuf[i]);
  return (i);
}
/*==========================================================================*/
int cmd_out(void)
{
  int i=0;

  if (flags_cmd_out&f_putcmd)
    { 
      DPRINTF((DBG_CMD,"SBPCD: cmd_out: put"));
      for (i=0;i<7;i++) DPRINTF((DBG_CMD," %02X",drvcmd[i]));
      DPRINTF((DBG_CMD,"\n"));

#if SBPCD_DIS_IRQ
      cli();
#endif SBPCD_DIS_IRQ
      for (i=0;i<7;i++) OUT(CDo_command,drvcmd[i]);
#if SBPCD_DIS_IRQ
      sti();
#endif SBPCD_DIS_IRQ
    }
  if (response_count!=0)
    {
      if (cmd_type!=0)
	{
	  if (sbpro_type) OUT(CDo_sel_d_i,0x01);
	  DPRINTF((DBG_INF,"SBPCD: misleaded to try ResponseData.\n"));
	  if (sbpro_type) OUT(CDo_sel_d_i,0x00);
	}
      else i=ResponseInfo();
      if (i<0) return (-9);
    }
  if (DS[d].in_SpinUp != 0) DPRINTF((DBG_SPI,"SBPCD: to CDi_stat_loop.\n"));
  if (flags_cmd_out&f_lopsta)
    {
      i=CDi_stat_loop();
      if ((i<0)||!(i&s_attention)) return (-9);
    }
  if (!(flags_cmd_out&f_getsta)) goto LOC_229;
  
LOC_228:
  if (DS[d].in_SpinUp != 0) DPRINTF((DBG_SPI,"SBPCD: to xx_ReadStatus.\n"));
  xx_ReadStatus();

LOC_229:
  if (flags_cmd_out&f_ResponseStatus) 
    {
      if (DS[d].in_SpinUp != 0) DPRINTF((DBG_SPI,"SBPCD: to ResponseStatus.\n"));
      i=ResponseStatus();
                   /* builds status_byte, returns orig. status or p_busy_new */
      if (i<0) return (-9);
      if (flags_cmd_out&(f_bit1|f_wait_if_busy))
	{
	  if (!st_check)
	    {
	      if (flags_cmd_out&f_bit1) if (i&p_success_old) goto LOC_232;
	      if (!(flags_cmd_out&f_wait_if_busy)) goto LOC_228;
	      if (!st_busy) goto LOC_228;
	    }
	}
    }
LOC_232:
  if (!(flags_cmd_out&f_obey_p_check)) return (0);
  if (!st_check) return (0);
  if (DS[d].in_SpinUp != 0) DPRINTF((DBG_SPI,"SBPCD: to xx_ReadError.\n"));
  i=xx_ReadError();
  if (DS[d].in_SpinUp != 0) DPRINTF((DBG_SPI,"SBPCD: to cmd_out OK.\n"));
  return (i);
}
/*==========================================================================*/
static int xx_Seek(u_int pos, char f_blk_msf)
{
  int i;

  clr_cmdbuf();
  if (f_blk_msf>1) return (-3);
  if (!new_drive)
    {
      if (f_blk_msf==1) pos=msf2blk(pos);
      drvcmd[2]=(pos>>16)&0x00FF;
      drvcmd[3]=(pos>>8)&0x00FF;
      drvcmd[4]=pos&0x00FF;
      flags_cmd_out = f_putcmd | f_respo2 | f_lopsta | f_getsta |
	               f_ResponseStatus | f_obey_p_check | f_bit1;
    }
  else
    {
      if (f_blk_msf==0) pos=blk2msf(pos);
      drvcmd[1]=(pos>>16)&0x00FF;
      drvcmd[2]=(pos>>8)&0x00FF;
      drvcmd[3]=pos&0x00FF;
      flags_cmd_out=f_putcmd|f_respo2|f_ResponseStatus|f_obey_p_check;
    }
  drvcmd[0]=0x01;
  response_count=0;
  i=cmd_out();
  return (i);
}
/*==========================================================================*/
static int xx_SpinUp(void)
{
  int i;

  DPRINTF((DBG_SPI,"SBPCD: SpinUp.\n"));
  DS[d].in_SpinUp = 1;
  clr_cmdbuf();
  if (!new_drive)
    {
      drvcmd[0]=0x05;
      flags_cmd_out=f_putcmd|f_respo2|f_lopsta|f_getsta|f_ResponseStatus|f_obey_p_check|f_bit1;
    }
  else
    {
      drvcmd[0]=0x02;
      flags_cmd_out=f_putcmd|f_respo2|f_ResponseStatus|f_obey_p_check;
    }
  response_count=0;
  i=cmd_out();
  DS[d].in_SpinUp = 0;
  return (i);
}
/*==========================================================================*/
static int yy_SpinDown(void)
{
  int i;

  if (!new_drive) return (-3);
  clr_cmdbuf();
  drvcmd[0]=0x06;
  flags_cmd_out=f_putcmd|f_respo2|f_ResponseStatus|f_obey_p_check;
  response_count=0;
  i=cmd_out();
  return (i);
}
/*==========================================================================*/
static int yy_SetSpeed(u_char speed, u_char x1, u_char x2)
{
  int i;

  if (!new_drive) return (-3);
  clr_cmdbuf();
  drvcmd[0]=0x09;
  drvcmd[1]=0x03;
  drvcmd[2]=speed;
  drvcmd[3]=x1;
  drvcmd[4]=x2;
  flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
  response_count=0;
  i=cmd_out();
  return (i);
}
/*==========================================================================*/
static int xx_SetVolume(void)
{
  int i;
  u_char channel0,channel1,volume0,volume1;
  u_char control0,value0,control1,value1;

  DS[d].diskstate_flags &= ~volume_bit;
  clr_cmdbuf();
  channel0=DS[d].vol_chan0;
  volume0=DS[d].vol_ctrl0;
  channel1=control1=DS[d].vol_chan1;
  volume1=value1=DS[d].vol_ctrl1;
  control0=value0=0;

  if (((DS[d].drv_options&sax_a)!=0)&&(DS[d].drv_type>=drv_211))
    {
      if ((volume0!=0)&&(volume1==0))
	{
	  volume1=volume0;
	  channel1=channel0;
	}
      else if ((volume0==0)&&(volume1!=0))
	{
	  volume0=volume1;
	  channel0=channel1;
	}
    }
  if (channel0>1)
    {
      channel0=0;
      volume0=0;
    }
  if (channel1>1)
    {
      channel1=1;
      volume1=0;
    }
  
  if (new_drive)
    {
      control0=channel0+1;
      control1=channel1+1;
      value0=(volume0>volume1)?volume0:volume1;
      value1=value0;
      if (volume0==0) control0=0;
      if (volume1==0) control1=0;
      drvcmd[0]=0x09;
      drvcmd[1]=0x05;
      drvcmd[3]=control0;
      drvcmd[4]=value0;
      drvcmd[5]=control1;
      drvcmd[6]=value1;
      flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
    }
  else 
    {
      if (DS[d].drv_type>=drv_300)
	{
	  control0=volume0&0xFC;
	  value0=volume1&0xFC;
	  if ((volume0!=0)&&(volume0<4)) control0 |= 0x04;
	  if ((volume1!=0)&&(volume1<4)) value0 |= 0x04;
	  if (channel0!=0) control0 |= 0x01;
	  if (channel1==1) value0 |= 0x01;
	}
      else
	{
	  value0=(volume0>volume1)?volume0:volume1;
	  if (DS[d].drv_type<drv_211)
	    {
	      if (channel0!=0)
		{
		  i=channel1;
		  channel1=channel0;
		  channel0=i;
		  i=volume1;
		  volume1=volume0;
		  volume0=i;
		}
	      if (channel0==channel1)
		{
		  if (channel0==0)
		    {
		      channel1=1;
		      volume1=0;
		      volume0=value0;
		    }
		  else
		    {
		      channel0=0;
		      volume0=0;
		      volume1=value0;
		    }
		}
	    }

	  if ((volume0!=0)&&(volume1!=0))
	    {
	      if (volume0==0xFF) volume1=0xFF;
	      else if (volume1==0xFF) volume0=0xFF;
	    }
	  else if (DS[d].drv_type<drv_201) volume0=volume1=value0;

	  if (DS[d].drv_type>=drv_201)
	    {
	      if (volume0==0) control0 |= 0x80;
	      if (volume1==0) control0 |= 0x40;
	    }
	  if (DS[d].drv_type>=drv_211)
	    {
	      if (channel0!=0) control0 |= 0x20;
	      if (channel1!=1) control0 |= 0x10;
	    }
	}
      drvcmd[0]=0x84;
      drvcmd[1]=0x83;
      drvcmd[4]=control0;
      drvcmd[5]=value0;
      flags_cmd_out=f_putcmd|f_getsta|f_ResponseStatus|f_obey_p_check;
    }

  response_count=0;
  i=cmd_out();
  if (i>0) return (i);
  DS[d].diskstate_flags |= volume_bit;
  return (0);
}
/*==========================================================================*/
static int GetStatus(void)
{
  int i;

  flags_cmd_out=f_getsta|f_ResponseStatus|f_obey_p_check;
  response_count=0;
  cmd_type=0;
  i=cmd_out();
  return (i);
}
/*==========================================================================*/
static int xy_DriveReset(void)
{
  int i;

  DPRINTF((DBG_RES,"SBPCD: xy_DriveReset called.\n"));
  if (!new_drive) OUT(CDo_reset,0x00);
  else
    {
      clr_cmdbuf();
      drvcmd[0]=0x0A;
      flags_cmd_out=f_putcmd;
      response_count=0;
      i=cmd_out();
    }
  flush_status();
  i=GetStatus();
  if (i>=0) return -1;
  if (DS[d].error_byte!=aud_12) return -1;
  return (0);
}
/*==========================================================================*/
static int SetSpeed(void)
{
  int i, speed;

  if (!(DS[d].drv_options&(speed_auto|speed_300|speed_150))) return (0);
  speed=0x80;
  if (!(DS[d].drv_options&speed_auto))
    {
      speed |= 0x40;
      if (!(DS[d].drv_options&speed_300)) speed=0;
    }
  i=yy_SetSpeed(speed,0,0);
  return (i);
}
/*==========================================================================*/
static int DriveReset(void)
{
  int i;

  i=xy_DriveReset();
  if (i<0) return (-2);
  do
    {
      i=GetStatus();
      if ((i<0)&&(i!=-15)) return (-2); /* i!=-15 is from sta2err */
      if (!st_caddy_in) break;
    }
  while (!st_diskok);
  DS[d].CD_changed=1;
  i=SetSpeed();
  if (i<0) return (-2);
  return (0);
}
/*==========================================================================*/
static int xx_Pause_Resume(int pau_res)
{
  int i;

  clr_cmdbuf();
  if (new_drive)
    {
      drvcmd[0]=0x0D;
      flags_cmd_out=f_putcmd|f_respo2|f_ResponseStatus|f_obey_p_check;
    }
  else
    {
      drvcmd[0]=0x8D;
      flags_cmd_out=f_putcmd|f_respo2|f_getsta|f_ResponseStatus|f_obey_p_check;
    }
  if (pau_res!=1) drvcmd[1]=0x80;
  response_count=0;
  i=cmd_out();
  return (i);
}
/*==========================================================================*/
#if 000
static int yy_LockDoor(char lock)
{
  int i;

  if (!new_drive) return (-3);
  clr_cmdbuf();
  drvcmd[0]=0x0C;
  if (lock==1) drvcmd[1]=0x01;
  flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
  response_count=0;
  i=cmd_out();
  return (i);
}
#endif 000
/*==========================================================================*/
static int xx_ReadSubQ(void)
{
  int i,j;

  DS[d].diskstate_flags &= ~subq_bit;
  clr_cmdbuf();
  if (new_drive)
    {
      drvcmd[0]=0x87;
      flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
      response_count=11;
    }
  else
    {
      drvcmd[0]=0x89;
      drvcmd[1]=0x02;
      flags_cmd_out=f_putcmd|f_getsta|f_ResponseStatus|f_obey_p_check;
      response_count=13;
    }
  for (j=0;j<255;j++)
    {
      i=cmd_out();
      if (i<0) return (i);
      if (infobuf[0]!=0) break;
      if (!st_spinning)
	{
	  DS[d].SubQ_ctl_adr=DS[d].SubQ_trk=DS[d].SubQ_pnt_idx=DS[d].SubQ_whatisthis=0;
	  DS[d].SubQ_run_tot=DS[d].SubQ_run_trk=0;
	  return (0);
	}
    }
  DS[d].SubQ_audio=infobuf[0];
  DS[d].SubQ_ctl_adr=swap_nibbles(infobuf[1]);
  DS[d].SubQ_trk=byt2bcd(infobuf[2]);
  DS[d].SubQ_pnt_idx=byt2bcd(infobuf[3]);
  i=4;
  if (!new_drive) i=5;
  DS[d].SubQ_run_tot=make32(make16(0,infobuf[i]),make16(infobuf[i+1],infobuf[i+2])); /* msf-bin */
  i=7;
  if (!new_drive) i=9;
  DS[d].SubQ_run_trk=make32(make16(0,infobuf[i]),make16(infobuf[i+1],infobuf[i+2])); /* msf-bin */
  DS[d].SubQ_whatisthis=infobuf[i+3];
  DS[d].diskstate_flags |= subq_bit;
  return (0);
}
/*==========================================================================*/
static int xx_ModeSense(void)
{
  int i;

  DS[d].diskstate_flags &= ~frame_size_bit;
  clr_cmdbuf();
  if (new_drive)
    {
      drvcmd[0]=0x84;
      drvcmd[1]=0x00;
      flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
      response_count=5;
    }
  else
    {
      drvcmd[0]=0x85;
      drvcmd[1]=0x00;
      flags_cmd_out=f_putcmd|f_getsta|f_ResponseStatus|f_obey_p_check;
      response_count=2;
    }
  i=cmd_out();
  if (i<0) return (i);
  i=0;
  if (new_drive) DS[d].sense_byte=infobuf[i++];
  DS[d].frame_size=make16(infobuf[i],infobuf[i+1]);
  DS[d].diskstate_flags |= frame_size_bit;
  return (0);
}
/*==========================================================================*/
#if 0000
static int xx_TellVolume(void)
{
  int i;
  u_char switches;
  u_char chan0,vol0,chan1,vol1;

  DS[d].diskstate_flags &= ~volume_bit;
  clr_cmdbuf();
  if (new_drive)
    {
      drvcmd[0]=0x84;
      drvcmd[1]=0x05;
      response_count=5;
      flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
    }
  else
    {
      drvcmd[0]=0x85;
      drvcmd[1]=0x03;
      response_count=2;
      flags_cmd_out=f_putcmd|f_getsta|f_ResponseStatus|f_obey_p_check;
    }
  i=cmd_out();
  if (i<0) return (i);
  if (new_drive)
    {
      chan0=infobuf[1]&0x0F;
      vol0=infobuf[2];
      chan1=infobuf[3]&0x0F;
      vol1=infobuf[4];
      if (chan0==0)
	{
	  chan0=1;
	  vol0=0;
	}
      if (chan1==0)
	{
	  chan1=2;
	  vol1=0;
	}
      chan0 >>= 1;
      chan1 >>= 1;
    }
  else
    {
      chan0=0;
      chan1=1;
      vol0=vol1=infobuf[1];
      if (DS[d].drv_type>=drv_201)
	{
	  if (DS[d].drv_type<drv_300)
	    {
	      switches=infobuf[0];
	      if ((switches&0x80)!=0) vol0=0;
	      if ((switches&0x40)!=0) vol1=0;
	      if (DS[d].drv_type>=drv_211)
		{
		  if ((switches&0x20)!=0) chan0=1;
		  if ((switches&0x10)!=0) chan1=0;
		}
	    }
	  else
	    {
	      vol0=infobuf[0];
	      if ((vol0&0x01)!=0) chan0=1;
	      if ((vol1&0x01)==0) chan1=0;
	      vol0 &= 0xFC;
	      vol1 &= 0xFC;
	      if (vol0!=0) vol0 += 3;
	      if (vol1!=0) vol1 += 3;
	    }
	}
    }
  DS[d].vol_chan0=chan0;
  DS[d].vol_ctrl0=vol0;
  DS[d].vol_chan1=chan1;
  DS[d].vol_ctrl1=vol1;
  DS[d].vol_chan2=2;
  DS[d].vol_ctrl2=0xFF;
  DS[d].vol_chan3=3;
  DS[d].vol_ctrl3=0xFF;
  DS[d].diskstate_flags |= volume_bit;
  return (0);
}
#endif
/*==========================================================================*/
static int xx_ReadCapacity(void)
{
  int i;

  DS[d].diskstate_flags &= ~cd_size_bit;
  clr_cmdbuf();
  if (new_drive)
    {
      drvcmd[0]=0x85;
      flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
    }
  else
    {
      drvcmd[0]=0x88;
      flags_cmd_out=f_putcmd|f_getsta|f_ResponseStatus|f_obey_p_check;
    }
  response_count=5;
  i=cmd_out();
  if (i<0) return (i);
  DS[d].CDsize_blk=make32(make16(0,infobuf[0]),make16(infobuf[1],infobuf[2]));
  if (new_drive) DS[d].CDsize_blk=msf2blk(DS[d].CDsize_blk);
  DS[d].CDsize_frm = (DS[d].CDsize_blk * make16(infobuf[3],infobuf[4])) / CD_FRAMESIZE;
  DS[d].CDsize_blk += 151;
  DS[d].diskstate_flags |= cd_size_bit;
  return (0);
}
/*==========================================================================*/
static int xx_ReadTocDescr(void)
{
  int i;

  DS[d].diskstate_flags &= ~toc_bit;
  clr_cmdbuf();
  if (new_drive)
    {
      drvcmd[0]=0x8B;
      flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
    }
  else
    {
      drvcmd[0]=0x8B;
      flags_cmd_out=f_putcmd|f_getsta|f_ResponseStatus|f_obey_p_check;
    }
  response_count=6;
  i=cmd_out();
  if (i<0) return (i);
  DS[d].xa_byte=infobuf[0];
  DS[d].n_first_track=infobuf[1];
  DS[d].n_last_track=infobuf[2];
  DS[d].size_msf=make32(make16(0,infobuf[3]),make16(infobuf[4],infobuf[5]));
  DS[d].size_blk=msf2blk(DS[d].size_msf);
  DS[d].diskstate_flags |= toc_bit;
  DPRINTF((DBG_TOC,"SBPCD: TocDesc: %02X %02X %02X %08X\n",
	 DS[d].xa_byte,DS[d].n_first_track,DS[d].n_last_track,DS[d].size_msf));
  return (0);
}
/*==========================================================================*/
static int xx_ReadTocEntry(int num)
{
  int i;

  clr_cmdbuf();
  if (new_drive)
    {
      drvcmd[0]=0x8C;
      flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
    }
  else
    {
      drvcmd[0]=0x8C;
      drvcmd[1]=0x02;
      flags_cmd_out=f_putcmd|f_getsta|f_ResponseStatus|f_obey_p_check;
    }
  drvcmd[2]=num;
  response_count=8;
  i=cmd_out();
  if (i<0) return (i);
  DS[d].TocEnt_nixbyte=infobuf[0];
  DS[d].TocEnt_ctl_adr=swap_nibbles(infobuf[1]);
  DS[d].TocEnt_number=infobuf[2];
  DS[d].TocEnt_format=infobuf[3];
  if (new_drive) i=4;
  else i=5;
  DS[d].TocEnt_address=make32(make16(0,infobuf[i]),make16(infobuf[i+1],infobuf[i+2]));
  DPRINTF((DBG_TOC,"SBPCD: TocEntry: %02X %02X %02X %02X %08X\n",
	   DS[d].TocEnt_nixbyte,DS[d].TocEnt_ctl_adr,DS[d].TocEnt_number,
	   DS[d].TocEnt_format,DS[d].TocEnt_address));
  return (0);
}
/*==========================================================================*/
static int xx_ReadPacket(void)
{
  int i;

  clr_cmdbuf();
  drvcmd[0]=0x8E;
  drvcmd[1]=response_count;
  flags_cmd_out=f_putcmd|f_getsta|f_ResponseStatus|f_obey_p_check;
  i=cmd_out();
  return (i);
}
/*==========================================================================*/
static int convert_UPC(u_char *p)
{
  int i;

  p++;
  if (!new_drive) p[13]=0;
  for (i=0;i<7;i++)
    {
      if (new_drive) DS[d].UPC_buf[i]=swap_nibbles(*p++);
      else
	{
	  DS[d].UPC_buf[i]=((*p++)<<4)&0xFF;
	  DS[d].UPC_buf[i] |= *p++;
	}
    }
  DS[d].UPC_buf[6] &= 0xF0;
  return (0);
}
/*==========================================================================*/
static int xx_ReadUPC(void)
{
  int i;

  DS[d].diskstate_flags &= ~upc_bit;
  clr_cmdbuf();
  if (new_drive)
    {
      drvcmd[0]=0x88;
      response_count=8;
      flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
    }
  else
    {
      drvcmd[0]=0x08;
      response_count=0;
      flags_cmd_out=f_putcmd|f_lopsta|f_getsta|f_ResponseStatus|f_obey_p_check|f_bit1;
    }
  i=cmd_out();
  if (i<0) return (i);
  if (!new_drive)
    {
      response_count=16;
      i=xx_ReadPacket();
      if (i<0) return (i);
    }
  DS[d].UPC_ctl_adr=0;
  if (new_drive) i=0;
  else i=2;
  if ((infobuf[i]&0x80)!=0)
    {
      convert_UPC(&infobuf[i]);
      DS[d].UPC_ctl_adr &= 0xF0;
      DS[d].UPC_ctl_adr |= 0x02;
    }
  DS[d].diskstate_flags |= upc_bit;
  return (0);
}
/*==========================================================================*/
static int yy_CheckMultiSession(void)
{
  int i;

  DS[d].diskstate_flags &= ~multisession_bit;
  DS[d].f_multisession=0;
  clr_cmdbuf();
  if (new_drive)
    {
      drvcmd[0]=0x8D;
      response_count=6;
      flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
      i=cmd_out();
      if (i<0) return (i);
      if ((infobuf[0]&0x80)!=0)
	{
	  DPRINTF((DBG_MUL,"SBPCD: MultiSession CD detected: %02X %02X %02X %02X %02X %02X\n",
                         infobuf[0], infobuf[1], infobuf[2],
		         infobuf[3], infobuf[4], infobuf[5]));
	  DS[d].f_multisession=1;
	  DS[d].lba_multi=msf2blk(make32(make16(0,infobuf[1]),
                                   make16(infobuf[2],infobuf[3])));
	}
    }
  DS[d].diskstate_flags |= multisession_bit;
  return (0);
}
/*==========================================================================*/
static void check_datarate(void)
{
#ifdef CDMKE
  int i=0;

  timed_out=0;
  datarate=0;

  /* set a timer to make (timed_out!=0) after 1.1 seconds */

  DPRINTF((DBG_TIM,"SBPCD: timer started (110).\n"));
  sti(); /* to avoid possible "printf" bug */

  SET_TIMER(mark_timeout,110);
  do
    {
      i=inb(CDi_status);
      datarate++;

#if 00000
      if (datarate>0x0FFFFFFF) break;
#endif 00000

    }
  while (!timed_out); /* originally looping for 1.1 seconds */
  CLEAR_TIMER;
  DPRINTF((DBG_TIM,"SBPCD: datarate: %d\n", datarate));
  if (datarate<65536) datarate=65536;

  maxtim16=datarate*16;
  maxtim04=datarate*4;
  maxtim02=datarate*2;
  maxtim_8=datarate/32;
#if MANY_SESSION
  maxtim_data=datarate/100;
#else
  maxtim_data=datarate/300;
#endif MANY_SESSION
  DPRINTF((DBG_TIM,"SBPCD: maxtim_8 %d, maxtim_data %d.\n",
	   maxtim_8, maxtim_data));
#endif CDMKE
}
/*==========================================================================*/
static int check_version(void)
{
  int i, j;

  /* clear any pending error state */
  clr_cmdbuf();
  drvcmd[0]=0x82;
  response_count=9;
  flags_cmd_out=f_putcmd;
  cmd_out();

  /* read drive version */
  clr_cmdbuf();
  for (i=0;i<12;i++) infobuf[i]=0;
  drvcmd[0]=0x83;
  response_count=12;
  flags_cmd_out=f_putcmd;
  i=cmd_out();
  if (i<0) DPRINTF((DBG_INI,"SBPCD: cmd_83 returns %d\n",i));

  DPRINTF((DBG_INI,"SBPCD: infobuf = \""));
  for (i=0;i<12;i++) DPRINTF((DBG_INI,"%c",infobuf[i]));
  DPRINTF((DBG_INI,"\"\n"));

  for (i=0;i<4;i++) if (infobuf[i]!=drive_family[i]) break;
  if (i==4)
    {
      DS[d].drive_model[0]=infobuf[i++];
      DS[d].drive_model[1]=infobuf[i++];
      DS[d].drive_model[2]='-';
      DS[d].drive_model[3]='x';
      DS[d].drv_type=drv_new;
    }
  else
    {
      for (i=0;i<8;i++) if (infobuf[i]!=drive_vendor[i]) break;
      if (i!=8) return (-1);
      DS[d].drive_model[0]='2';
      DS[d].drive_model[1]='x';
      DS[d].drive_model[2]='-';
      DS[d].drive_model[3]='x';
      DS[d].drv_type=drv_old;
    }
  for (j=0;j<4;j++) DS[d].firmware_version[j]=infobuf[i+j];
  j = (DS[d].firmware_version[0] & 0x0F) * 100 +
      (DS[d].firmware_version[2] & 0x0F) *10 +
      (DS[d].firmware_version[3] & 0x0F);
  if (new_drive)
    {
      if (j<100) DS[d].drv_type=drv_099;
      else DS[d].drv_type=drv_100;
    }
  else if (j<200) DS[d].drv_type=drv_199;
  else if (j<201) DS[d].drv_type=drv_200;
  else if (j<210) DS[d].drv_type=drv_201;
  else if (j<211) DS[d].drv_type=drv_210;
  else if (j<300) DS[d].drv_type=drv_211;
  else DS[d].drv_type=drv_300;
  return (0);
}
/*==========================================================================*/
static int switch_drive(int num)
{
  int i;

  d=num;

  i=num;
  if (sbpro_type) i=(i&0x01)<<1|(i&0x02)>>1;
  OUT(CDo_enable,i);
  DPRINTF((DBG_DID,"SBPCD: switch_drive: drive %d activated.\n",DS[d].drv_minor));
  return (0);
}
/*==========================================================================*/
/*
 * probe for the presence of drives on the selected controller
 */
static int check_drives(void)
{
  int i, j;
  char *printk_header="";

  DPRINTF((DBG_INI,"SBPCD: check_drives entered.\n"));

  ndrives=0;
  for (j=0;j<NR_SBPCD;j++)
    {
      DS[j].drv_minor=j;
      switch_drive(j);
      DPRINTF((DBG_ID,"SBPCD: check_drives: drive %d activated.\n",j));
      i=check_version();
      DPRINTF((DBG_ID,"SBPCD: check_version returns %d.\n",i));
      if (i>=0)
	{
	  ndrives++;
	  DS[d].drv_options=drv_pattern[j];
	  if (!new_drive) DS[d].drv_options&=~(speed_auto|speed_300|speed_150);
	  printk("%sDrive %d: %s%.4s (%.4s)\n", printk_header,
		 DS[d].drv_minor,
                 drive_family,
                 DS[d].drive_model,
                 DS[d].firmware_version);
	  printk_header="       - ";
	}
      else DS[d].drv_minor=-1;
    }
  if (ndrives==0) return (-1);
  return (0);
}
/*==========================================================================*/
#if 000
static void timewait(void)
{
  int i;
  for (i=0; i<65500; i++);
}
#endif 000
/*==========================================================================*/
#if FUTURE
/*
 *  obtain if requested service disturbs current audio state
 */            
static int obey_audio_state(u_char audio_state, u_char func,u_char subfunc)
{
  switch (audio_state)                   /* audio status from controller  */
    {
    case aud_11: /* "audio play in progress" */
    case audx11:
      switch (func)                      /* DOS command code */
	{
	case cmd_07: /* input flush  */
	case cmd_0d: /* open device  */
	case cmd_0e: /* close device */
	case cmd_0c: /* ioctl output */
	  return (1);
	case cmd_03: /* ioctl input  */
	  switch (subfunc)
	    /* DOS ioctl input subfunction */
	    {
	    case cxi_00:
	    case cxi_06:
	    case cxi_09:
	      return (1);
	    default:
	      return (ERROR15);
	    }
	  return (1);
	default:
	  return (ERROR15);
	}
      return (1);
    case aud_12:                  /* "audio play paused"      */
    case audx12:
      return (1);
    default:
      return (2);
    }
}
#endif FUTURE
/*==========================================================================*/
/* allowed is only
 * ioctl_o, flush_input, open_device, close_device, 
 * tell_address, tell_volume, tell_capabiliti,
 * tell_framesize, tell_CD_changed, tell_audio_posi
 */
static int check_allowed1(u_char func1, u_char func2)
{
#if 000
  if (func1==ioctl_o) return (0);
  if (func1==read_long) return (-1);
  if (func1==read_long_prefetch) return (-1);
  if (func1==seek) return (-1);
  if (func1==audio_play) return (-1);
  if (func1==audio_pause) return (-1);
  if (func1==audio_resume) return (-1);
  if (func1!=ioctl_i) return (0);
  if (func2==tell_SubQ_run_tot) return (-1);
  if (func2==tell_cdsize) return (-1);
  if (func2==tell_TocDescrip) return (-1);
  if (func2==tell_TocEntry) return (-1);
  if (func2==tell_subQ_info) return (-1);
  if (new_drive) if (func2==tell_SubChanInfo) return (-1);
  if (func2==tell_UPC) return (-1);
#else
  return (0);
#endif 000
}
/*==========================================================================*/
static int check_allowed2(u_char func1, u_char func2)
{
#if 000
  if (func1==read_long) return (-1);
  if (func1==read_long_prefetch) return (-1);
  if (func1==seek) return (-1);
  if (func1==audio_play) return (-1);
  if (func1!=ioctl_o) return (0);
  if (new_drive)
    {
      if (func2==EjectDisk) return (-1);
      if (func2==CloseTray) return (-1);
    }
#else
  return (0);
#endif 000
}
/*==========================================================================*/
static int check_allowed3(u_char func1, u_char func2)
{
#if 000
  if (func1==ioctl_i)
    {
      if (func2==tell_address) return (0);
      if (func2==tell_capabiliti) return (0);
      if (func2==tell_CD_changed) return (0);
      if (!new_drive) if (func2==tell_SubChanInfo) return (0);
      return (-1);
    }
  if (func1==ioctl_o)
    {
      if (func2==DriveReset) return (0);
      if (!new_drive)
	{
	  if (func2==EjectDisk) return (0);
	  if (func2==LockDoor) return (0);
	  if (func2==CloseTray) return (0);
	}
      return (-1);
    }
  if (func1==flush_input) return (-1);
  if (func1==read_long) return (-1);
  if (func1==read_long_prefetch) return (-1);
  if (func1==seek) return (-1);
  if (func1==audio_play) return (-1);
  if (func1==audio_pause) return (-1);
  if (func1==audio_resume) return (-1);
#else
  return (0);
#endif 000
}
/*==========================================================================*/
static int seek_pos_audio_end(void)
{
  int i;

  i=msf2blk(DS[d].pos_audio_end)-1;
  if (i<0) return (-1);
  i=xx_Seek(i,0);
  return (i);
}
/*==========================================================================*/
static int ReadToC(void)
{
  int i, j;
  DS[d].diskstate_flags &= ~toc_bit;
  DS[d].ored_ctl_adr=0;
  for (j=DS[d].n_first_track;j<=DS[d].n_last_track;j++)
    {
      i=xx_ReadTocEntry(j);
      if (i<0) return (i);
      DS[d].TocBuffer[j].nixbyte=DS[d].TocEnt_nixbyte;
      DS[d].TocBuffer[j].ctl_adr=DS[d].TocEnt_ctl_adr;
      DS[d].TocBuffer[j].number=DS[d].TocEnt_number;
      DS[d].TocBuffer[j].format=DS[d].TocEnt_format;
      DS[d].TocBuffer[j].address=DS[d].TocEnt_address;
      DS[d].ored_ctl_adr |= DS[d].TocEnt_ctl_adr;
    }
/* fake entry for LeadOut Track */
  DS[d].TocBuffer[j].nixbyte=0;
  DS[d].TocBuffer[j].ctl_adr=0;
  DS[d].TocBuffer[j].number=0;
  DS[d].TocBuffer[j].format=0;
  DS[d].TocBuffer[j].address=DS[d].size_msf;

  DS[d].diskstate_flags |= toc_bit;
  return (0);
}
/*==========================================================================*/
static int DiskInfo(void)
{
  int i;

  i=SetSpeed();
  if (i<0)
    {
      DPRINTF((DBG_INF,"SBPCD: DiskInfo: first SetSpeed returns %d\n", i));
      i=SetSpeed();
      if (i<0)
	{
	  DPRINTF((DBG_INF,"SBPCD: DiskInfo: second SetSpeed returns %d\n", i));
	  return (i);
	}
    }
  i=xx_ModeSense();
  if (i<0)
    {
      DPRINTF((DBG_INF,"SBPCD: DiskInfo: first xx_ModeSense returns %d\n", i));
      i=xx_ModeSense();
      if (i<0)
	{
	  DPRINTF((DBG_INF,"SBPCD: DiskInfo: second xx_ModeSense returns %d\n", i));
	  return (i);
	}
      return (i);
    }
  i=xx_ReadCapacity();
  if (i<0)
    {
      DPRINTF((DBG_INF,"SBPCD: DiskInfo: first ReadCapacity returns %d\n", i));
      i=xx_ReadCapacity();
      if (i<0)
	{
	  DPRINTF((DBG_INF,"SBPCD: DiskInfo: second ReadCapacity returns %d\n", i));
	  return (i);
	}
      return (i);
    }
  i=xx_ReadTocDescr();
  if (i<0)
    {
      DPRINTF((DBG_INF,"SBPCD: DiskInfo: ReadTocDescr returns %d\n", i));
      return (i);
    }
  i=ReadToC();
  if (i<0)
    {
      DPRINTF((DBG_INF,"SBPCD: DiskInfo: ReadToC returns %d\n", i));
      return (i);
    }
  i=yy_CheckMultiSession();
  if (i<0)
    {
      DPRINTF((DBG_INF,"SBPCD: DiskInfo: yy_CheckMultiSession returns %d\n", i));
      return (i);
    }
  i=xx_ReadTocEntry(DS[d].n_first_track);
  if (i<0)
    {
      DPRINTF((DBG_INF,"SBPCD: DiskInfo: xx_ReadTocEntry(1) returns %d\n", i));
      return (i);
    }
  i=xx_ReadUPC();
  if (i<0)
    {
      DPRINTF((DBG_INF,"SBPCD: DiskInfo: xx_ReadUPC returns %d\n", i));
      return (i);
    }
  return (0);
}
/*==========================================================================*/
/*
 *  called always if driver gets entered
 *  returns 0 or ERROR2 or ERROR15
 */
static int prepare(u_char func, u_char subfunc)
{
  int i;

  if (!new_drive)
    {
      i=inb(CDi_status);
      if (i&s_attention) GetStatus();
    }
  else GetStatus();
  if (DS[d].CD_changed==0xFF)
    {
#if MANY_SESSION
#else
      DS[d].diskstate_flags=0;
#endif MANY_SESSION
      DS[d].audio_state=0;
      if (!st_diskok)
	{
	  i=check_allowed1(func,subfunc);
	  if (i<0) return (-2);
	}
      else 
	{
	  i=check_allowed3(func,subfunc);
	  if (i<0)
	    {
	      DS[d].CD_changed=1;
	      return (-15);
	    }
	}
    }
  else
    {
      if (!st_diskok)
	{
#if MANY_SESSION
#else
	  DS[d].diskstate_flags=0;
#endif MANY_SESSION
	  DS[d].audio_state=0;
	  i=check_allowed1(func,subfunc);
	  if (i<0) return (-2);
	}
      else
	{ 
	  if (st_busy)
	    {
	      if (DS[d].audio_state!=audio_pausing)
		{
		  i=check_allowed2(func,subfunc);
		  if (i<0) return (-2);
		}
	    }
	  else
	    {
	      if (DS[d].audio_state==audio_playing) seek_pos_audio_end();
	      DS[d].audio_state=0;
	    }
	  if (!frame_size_valid)
	    {
	      i=DiskInfo();
	      if (i<0)
		{
#if MANY_SESSION
#else
		  DS[d].diskstate_flags=0;
#endif MANY_SESSION
		  DS[d].audio_state=0;
		  i=check_allowed1(func,subfunc);
		  if (i<0) return (-2);
		}
	    }
	}
    }
  return (0);
}
/*==========================================================================*/
static int xx_PlayAudioMSF(int pos_audio_start,int pos_audio_end)
{
  int i;

  if (DS[d].audio_state==audio_playing) return (-EINVAL);
  clr_cmdbuf();
  if (new_drive)
    {
      drvcmd[0]=0x0E;
      flags_cmd_out = f_putcmd | f_respo2 | f_ResponseStatus |
                       f_obey_p_check | f_wait_if_busy;
    }
  else
    {
      drvcmd[0]=0x0B;
      flags_cmd_out = f_putcmd | f_respo2 | f_lopsta | f_getsta |
                       f_ResponseStatus | f_obey_p_check | f_wait_if_busy;
    }
  drvcmd[1]=(pos_audio_start>>16)&0x00FF;
  drvcmd[2]=(pos_audio_start>>8)&0x00FF;
  drvcmd[3]=pos_audio_start&0x00FF;
  drvcmd[4]=(pos_audio_end>>16)&0x00FF;
  drvcmd[5]=(pos_audio_end>>8)&0x00FF;
  drvcmd[6]=pos_audio_end&0x00FF;
  response_count=0;
  i=cmd_out();
  return (i);
}
/*==========================================================================*/
/*==========================================================================*/

/*==========================================================================*/
/*==========================================================================*/
/*
 * ioctl support, adopted from scsi/sr_ioctl.c and mcd.c
 */
static int sbpcd_ioctl(struct inode *inode,struct file *file,
		       u_int cmd, u_long arg)
{
  int i, st;

  DPRINTF((DBG_IO2,"SBPCD: ioctl(%d, 0x%08lX, 0x%08lX)\n",
				MINOR(inode->i_rdev), cmd, arg));
  if (!inode) return (-EINVAL);
  i=MINOR(inode->i_rdev);
  if ( (i<0) || (i>=NR_SBPCD) )
    {
      printk("SBPCD: ioctl: bad device: %d\n", i);
      return (-ENODEV);             /* no such drive */
    }
  switch_drive(i);

  st=GetStatus();
  if (st<0) return (-EIO);
  
  if (!toc_valid)
    {
      i=DiskInfo();
      if (i<0) return (-EIO);	/* error reading TOC */
    }
  
  DPRINTF((DBG_IO2,"SBPCD: ioctl: device %d, request %04X\n",i,cmd));
  switch (cmd) 		/* Sun-compatible */
    {
    case DDIOCSDBG:		/* DDI Debug */
      if (! suser()) return (-EPERM);
      i = verify_area(VERIFY_READ, (int *) arg, sizeof(int));
      if (i>=0) i=sbpcd_dbg_ioctl(arg,1);
      return (i);

    case CDROMPAUSE:     /* Pause the drive */
      DPRINTF((DBG_IOC,"SBPCD: ioctl: CDROMPAUSE entered.\n"));
      /* pause the drive unit when it is currently in PLAY mode,         */
      /* or reset the starting and ending locations when in PAUSED mode. */
      /* If applicable, at the next stopping point it reaches            */
      /* the drive will discontinue playing.                             */
      switch (DS[d].audio_state)
	{
	case audio_playing:
	  i=xx_Pause_Resume(1);
	  if (i<0) return (-EIO);
	  DS[d].audio_state=audio_pausing;
	  i=xx_ReadSubQ();
	  if (i<0) return (-EIO);
	  DS[d].pos_audio_start=DS[d].SubQ_run_tot;
	  return (0);
	case audio_pausing:
	  i=xx_Seek(DS[d].pos_audio_start,1);
	  if (i<0) return (-EIO);
	  return (0);
	default:
	  return (-EINVAL);
	}
      
    case CDROMRESUME: /* resume paused audio play */
      DPRINTF((DBG_IOC,"SBPCD: ioctl: CDROMRESUME entered.\n"));
      /* resume playing audio tracks when a previous PLAY AUDIO call has  */
      /* been paused with a PAUSE command.                                */
      /* It will resume playing from the location saved in SubQ_run_tot.  */
      if (DS[d].audio_state!=audio_pausing) return -EINVAL;
      i=xx_Pause_Resume(3);
      if (i<0) return (-EIO);
      DS[d].audio_state=audio_playing;
      return (0);

    case CDROMPLAYMSF:
      DPRINTF((DBG_IOC,"SBPCD: ioctl: CDROMPLAYMSF entered.\n"));
      if (DS[d].audio_state==audio_playing)
	{
	  i=xx_Pause_Resume(1);
	  if (i<0) return (-EIO);
	  i=xx_ReadSubQ();
	  if (i<0) return (-EIO);
	  DS[d].pos_audio_start=DS[d].SubQ_run_tot;
	  i=xx_Seek(DS[d].pos_audio_start,1);
	}
      st=verify_area(VERIFY_READ, (void *) arg, sizeof(struct cdrom_msf));
      if (st) return (st);
      memcpy_fromfs(&msf, (void *) arg, sizeof(struct cdrom_msf));
      /* values come as msf-bin */
      DS[d].pos_audio_start = (msf.cdmsf_min0<<16) |
                        (msf.cdmsf_sec0<<8) |
                        msf.cdmsf_frame0;
      DS[d].pos_audio_end = (msf.cdmsf_min1<<16) |
                      (msf.cdmsf_sec1<<8) |
                      msf.cdmsf_frame1;
      DPRINTF((DBG_IOX,"SBPCD: ioctl: CDROMPLAYMSF %08X %08X\n",
	                       DS[d].pos_audio_start,DS[d].pos_audio_end));
      i=xx_PlayAudioMSF(DS[d].pos_audio_start,DS[d].pos_audio_end);
      DPRINTF((DBG_IOC,"SBPCD: ioctl: xx_PlayAudioMSF returns %d\n",i));
#if 0
      if (i<0) return (-EIO);
#endif 0
      DS[d].audio_state=audio_playing;
      return (0);

    case CDROMPLAYTRKIND: /* Play a track.  This currently ignores index. */
      DPRINTF((DBG_IOC,"SBPCD: ioctl: CDROMPLAYTRKIND entered.\n"));
      if (DS[d].audio_state==audio_playing)
	{
	  DPRINTF((DBG_IOX,"SBPCD: CDROMPLAYTRKIND: already audio_playing.\n"));
	  return (0);
	  return (-EINVAL);
	}
      st=verify_area(VERIFY_READ,(void *) arg,sizeof(struct cdrom_ti));
      if (st<0)
	{
	  DPRINTF((DBG_IOX,"SBPCD: CDROMPLAYTRKIND: verify_area error.\n"));
	  return (st);
	}
      memcpy_fromfs(&ti,(void *) arg,sizeof(struct cdrom_ti));
      DPRINTF((DBG_IOX,"SBPCD: ioctl: trk0: %d, ind0: %d, trk1:%d, ind1:%d\n",
	     ti.cdti_trk0,ti.cdti_ind0,ti.cdti_trk1,ti.cdti_ind1));
      if (ti.cdti_trk0<DS[d].n_first_track) return (-EINVAL);
      if (ti.cdti_trk0>DS[d].n_last_track) return (-EINVAL);
      if (ti.cdti_trk1<ti.cdti_trk0) ti.cdti_trk1=ti.cdti_trk0;
      if (ti.cdti_trk1>DS[d].n_last_track) ti.cdti_trk1=DS[d].n_last_track;
      DS[d].pos_audio_start=DS[d].TocBuffer[ti.cdti_trk0].address;
      DS[d].pos_audio_end=DS[d].TocBuffer[ti.cdti_trk1+1].address;
      i=xx_PlayAudioMSF(DS[d].pos_audio_start,DS[d].pos_audio_end);
#if 0
      if (i<0) return (-EIO);
#endif 0
      DS[d].audio_state=audio_playing;
      return (0);
	    
    case CDROMREADTOCHDR:        /* Read the table of contents header */
      DPRINTF((DBG_IOC,"SBPCD: ioctl: CDROMREADTOCHDR entered.\n"));
      tochdr.cdth_trk0=DS[d].n_first_track;
      tochdr.cdth_trk1=DS[d].n_last_track;
      st=verify_area(VERIFY_WRITE, (void *) arg, sizeof(struct cdrom_tochdr));
      if (st) return (st);
      memcpy_tofs((void *) arg, &tochdr, sizeof(struct cdrom_tochdr));
      return (0);

    case CDROMREADTOCENTRY:      /* Read an entry in the table of contents */
      DPRINTF((DBG_IOC,"SBPCD: ioctl: CDROMREADTOCENTRY entered.\n"));
      st=verify_area(VERIFY_READ, (void *) arg, sizeof(struct cdrom_tocentry));
      if (st) return (st);
      memcpy_fromfs(&tocentry, (void *) arg, sizeof(struct cdrom_tocentry));
      i=tocentry.cdte_track;
      if (i==CDROM_LEADOUT) i=DS[d].n_last_track+1;
      else if (i<DS[d].n_first_track||i>DS[d].n_last_track) return (-EINVAL);
      tocentry.cdte_adr=DS[d].TocBuffer[i].ctl_adr&0x0F;
      tocentry.cdte_ctrl=(DS[d].TocBuffer[i].ctl_adr>>4)&0x0F;
      tocentry.cdte_datamode=DS[d].TocBuffer[i].format;
      if (tocentry.cdte_format==CDROM_MSF) /* MSF-bin required */
	{ tocentry.cdte_addr.msf.minute=(DS[d].TocBuffer[i].address>>16)&0x00FF;
	  tocentry.cdte_addr.msf.second=(DS[d].TocBuffer[i].address>>8)&0x00FF;
	  tocentry.cdte_addr.msf.frame=DS[d].TocBuffer[i].address&0x00FF;
	}
      else if (tocentry.cdte_format==CDROM_LBA) /* blk required */
	tocentry.cdte_addr.lba=msf2blk(DS[d].TocBuffer[i].address);
      else return (-EINVAL);
      st=verify_area(VERIFY_WRITE,(void *) arg, sizeof(struct cdrom_tocentry));
      if (st) return (st);
      memcpy_tofs((void *) arg, &tocentry, sizeof(struct cdrom_tocentry));
      return (0);

    case CDROMSTOP:      /* Spin down the drive */
      DPRINTF((DBG_IOC,"SBPCD: ioctl: CDROMSTOP entered.\n"));
      i=DriveReset();
#if WORKMAN
      DS[d].CD_changed=0xFF;
      DS[d].diskstate_flags=0;
#endif WORKMAN
      DPRINTF((DBG_IOC,"SBPCD: ioctl: DriveReset returns %d\n",i));
      DS[d].audio_state=0;
      i=DiskInfo();
      return (0);

    case CDROMSTART:  /* Spin up the drive */
      DPRINTF((DBG_IOC,"SBPCD: ioctl: CDROMSTART entered.\n"));
      i=xx_SpinUp();
      DS[d].audio_state=0;
      return (0);
      
    case CDROMEJECT:
      DPRINTF((DBG_IOC,"SBPCD: ioctl: CDROMEJECT entered.\n"));
      if (!new_drive) return (0);
#if WORKMAN
      DS[d].CD_changed=0xFF;
      DS[d].diskstate_flags=0;
#endif WORKMAN
      i=yy_SpinDown();
      if (i<0) return (-EIO);
      DS[d].audio_state=0;
      return (0);
      
    case CDROMVOLCTRL:   /* Volume control */
      DPRINTF((DBG_IOC,"SBPCD: ioctl: CDROMVOLCTRL entered.\n"));
      st=verify_area(VERIFY_READ,(void *) arg,sizeof(volctrl));
      if (st) return (st);
      memcpy_fromfs(&volctrl,(char *) arg,sizeof(volctrl));
      DS[d].vol_chan0=0;
      DS[d].vol_ctrl0=volctrl.channel0;
      DS[d].vol_chan1=1;
      DS[d].vol_ctrl1=volctrl.channel1;
      i=xx_SetVolume();
      return (0);

    case CDROMSUBCHNL:   /* Get subchannel info */
      DPRINTF((DBG_IOS,"SBPCD: ioctl: CDROMSUBCHNL entered.\n"));
      if ((st_spinning)||(!subq_valid)) { i=xx_ReadSubQ();
					  if (i<0) return (-EIO);
					}
      st=verify_area(VERIFY_WRITE, (void *) arg, sizeof(struct cdrom_subchnl));
      if (st)	return (st);
      memcpy_fromfs(&SC, (void *) arg, sizeof(struct cdrom_subchnl));
#if 0
      if (DS[d].SubQ_audio==0x80) DS[d].SubQ_audio=CDROM_AUDIO_NO_STATUS;
#endif
      switch (DS[d].audio_state)
	{
	case audio_playing:
	  SC.cdsc_audiostatus=CDROM_AUDIO_PLAY;
	  break;
	case audio_pausing:
	  SC.cdsc_audiostatus=CDROM_AUDIO_PAUSED;
	  break;
	default:
	  SC.cdsc_audiostatus=CDROM_AUDIO_NO_STATUS;
	  break;
	}
      SC.cdsc_adr=DS[d].SubQ_ctl_adr;
      SC.cdsc_ctrl=DS[d].SubQ_ctl_adr>>4;
      SC.cdsc_trk=bcd2bin(DS[d].SubQ_trk);
      SC.cdsc_ind=bcd2bin(DS[d].SubQ_pnt_idx);
      if (SC.cdsc_format==CDROM_LBA)
	{
	  SC.cdsc_absaddr.lba=msf2blk(DS[d].SubQ_run_tot);
	  SC.cdsc_reladdr.lba=msf2blk(DS[d].SubQ_run_trk);
	}
      else /* not only if (SC.cdsc_format==CDROM_MSF) */
	{
	  SC.cdsc_absaddr.msf.minute=(DS[d].SubQ_run_tot>>16)&0x00FF;
	  SC.cdsc_absaddr.msf.second=(DS[d].SubQ_run_tot>>8)&0x00FF;
	  SC.cdsc_absaddr.msf.frame=DS[d].SubQ_run_tot&0x00FF;
	  SC.cdsc_reladdr.msf.minute=(DS[d].SubQ_run_trk>>16)&0x00FF;
	  SC.cdsc_reladdr.msf.second=(DS[d].SubQ_run_trk>>8)&0x00FF;
	  SC.cdsc_reladdr.msf.frame=DS[d].SubQ_run_trk&0x00FF;
	}
      memcpy_tofs((void *) arg, &SC, sizeof(struct cdrom_subchnl));
      DPRINTF((DBG_IOS,"SBPCD: CDROMSUBCHNL: %1X %02X %08X %08X %02X %02X %06X %06X\n",
	       SC.cdsc_format,SC.cdsc_audiostatus,
	       SC.cdsc_adr,SC.cdsc_ctrl,
	       SC.cdsc_trk,SC.cdsc_ind,
	       SC.cdsc_absaddr,SC.cdsc_reladdr));
      return (0);

    case CDROMREADMODE2:
      DPRINTF((DBG_IOC,"SBPCD: ioctl: CDROMREADMODE2 requested.\n"));
      return (-EINVAL);

    case CDROMREADMODE1:
      DPRINTF((DBG_IOC,"SBPCD: ioctl: CDROMREADMODE1 requested.\n"));
      return (-EINVAL);
      
    default:
      DPRINTF((DBG_IOC,"SBPCD: ioctl: unknown function request %04X\n", cmd));
      return (-EINVAL);
    } /* end switch(cmd) */
}
/*==========================================================================*/
/*
 *  Take care of the different block sizes between cdrom and Linux.
 *  When Linux gets variable block sizes this will probably go away.
 */
static void sbp_transfer(void)
{
  long offs;
  
  while ( (CURRENT->nr_sectors > 0) &&
	  (CURRENT->sector/4 >= DS[d].sbp_first_frame) &&
	  (CURRENT->sector/4 <= DS[d].sbp_last_frame) )
    {
      offs = (CURRENT->sector - DS[d].sbp_first_frame * 4) * 512;
      memcpy(CURRENT->buffer, DS[d].sbp_buf + offs, 512);
      CURRENT->nr_sectors--;
      CURRENT->sector++;
      CURRENT->buffer += 512;
    }
}
/*==========================================================================*/
/*
 *  We seem to get never an interrupt.
 */
#if SBPCD_USE_IRQ
static void sbpcd_interrupt(int unused)
{
  int st;
  
  st = inb(CDi_status) & 0xFF;
  DPRINTF((DBG_IRQ,"SBPCD: INTERRUPT received - CDi_status=%02X\n", st));
}
#endif SBPCD_USE_IRQ
/*==========================================================================*/
/*
 * Called from the timer to check the results of the get-status cmd.
 */
static int sbp_status(void)
{
  int st;

  st=ResponseStatus();
  if (st<0)
    {
      DPRINTF((DBG_INF,"SBPCD: sbp_status: timeout.\n"));
      return (0);
    }

  if (!st_spinning) DPRINTF((DBG_SPI,"SBPCD: motor got off - ignoring.\n"));

  if (st_check) 
    {
      DPRINTF((DBG_INF,"SBPCD: st_check detected - retrying.\n"));
      return (0);
    }
  if (!st_door_closed)
    {
      DPRINTF((DBG_INF,"SBPCD: door is open - retrying.\n"));
      return (0);
    }
  if (!st_caddy_in)
    {
      DPRINTF((DBG_INF,"SBPCD: disk removed - retrying.\n"));
      return (0);
    }
  if (!st_diskok) 
    {
      DPRINTF((DBG_INF,"SBPCD: !st_diskok detected - retrying.\n"));
      return (0);
    }
  if (st_busy) 
    {
      DPRINTF((DBG_INF,"SBPCD: st_busy detected - retrying.\n"));
      return (0);
    }
  return (1);
}
/*==========================================================================*/
/*
 *  I/O request routine, called from Linux kernel.
 */
static void do_sbpcd_request(void)
{
  u_int block;
  int dev;
  u_int nsect;
  int i, status_tries, data_tries;
  
request_loop:

  sti();

  if ((CURRENT==NULL)||(CURRENT->dev<0)) return;
  if (CURRENT -> sector == -1)	return;

  dev = MINOR(CURRENT->dev);
  if ( (dev<0) || (dev>=NR_SBPCD) )
    {
      printk("SBPCD: do_request: bad device: %d\n", dev);
      return;
    }
  switch_drive(dev);

  INIT_REQUEST;
  block = CURRENT->sector;
  nsect = CURRENT->nr_sectors;

  if (CURRENT->cmd != READ)
    {
      printk("SBPCD: bad cmd %d\n", CURRENT->cmd);
      end_request(0);
      goto request_loop;
    }

  DPRINTF((DBG_MUL,"SBPCD: read LBA %d\n", block/4));
  sbp_transfer();

  /* if we satisfied the request from the buffer, we're done. */

  if (CURRENT->nr_sectors == 0)
    {
      end_request(1);
      goto request_loop;
    }

  i=prepare(0,0); /* at moment not really a hassle check, but ... */
  if (i!=0) DPRINTF((DBG_INF,"SBPCD: \"prepare\" tells error %d -- ignored\n", i));

  if (!st_spinning) xx_SpinUp();

  for (data_tries=3; data_tries > 0; data_tries--)
    {
      for (status_tries=3; status_tries > 0; status_tries--)
	{
	  flags_cmd_out |= f_respo3;
	  xx_ReadStatus();
	  if (sbp_status() != 0) break;
	  sbp_sleep(1);    /* wait a bit, try again */
	}
      if (status_tries == 0)
	{
	  DPRINTF((DBG_INF,"SBPCD: sbp_status: failed after 3 tries\n"));
	  break;
	}

      sbp_read_cmd();
      sbp_sleep(0);
      if (sbp_data() != 0)
	{
	  end_request(1);
	  goto request_loop;
	}
    }
  
  end_request(0);
  sbp_sleep(10);    /* wait a bit, try again */
  goto request_loop;
}
/*==========================================================================*/
/*
 *  build and send the READ command.
 *  Maybe it would be better to "set mode1" before ...
 */
static void sbp_read_cmd(void)
{
  int i;
  int block;

  DS[d].sbp_first_frame=DS[d].sbp_last_frame=-1;      /* purge buffer */
  block=CURRENT->sector/4;

  if (new_drive)
    {
#if MANY_SESSION
      DPRINTF((DBG_MUL,"SBPCD: read MSF %08X\n", blk2msf(block)));
      if ( (DS[d].f_multisession) && (multisession_valid) )
	{
	  DPRINTF((DBG_MUL,"SBPCD: MultiSession: use %08X for %08X (msf)\n",
		         blk2msf(DS[d].lba_multi+block),
                         blk2msf(block)));
	  block=DS[d].lba_multi+block;
	}
#else
      if ( (block==166) && (DS[d].f_multisession) && (multisession_valid) )
	{
	  DPRINTF((DBG_MUL,"SBPCD: MultiSession: use %08X for %08X (msf)\n",
		         blk2msf(DS[d].lba_multi+16),
                         blk2msf(block)));
	  block=DS[d].lba_multi+16;
	}
#endif MANY_SESSION
    }

  if (block+SBP_BUFFER_FRAMES <= DS[d].CDsize_frm)
    DS[d].sbp_read_frames = SBP_BUFFER_FRAMES;
  else
    {
      DS[d].sbp_read_frames=DS[d].CDsize_frm-block;
                                      /* avoid reading past end of data */
      if (DS[d].sbp_read_frames < 1)
	{
	  DPRINTF((DBG_INF,"SBPCD: requested frame %d, CD size %d ???\n",
		        block, DS[d].CDsize_frm));
	  DS[d].sbp_read_frames=1;
	}
    }
  DS[d].sbp_current = 0;

  flags_cmd_out = f_putcmd |
                  f_respo2 |
                  f_ResponseStatus |
                  f_obey_p_check;

  if (!new_drive)
    {
      if (DS[d].drv_type>=drv_201)
	{
	  lba2msf(block,&drvcmd[1]); /* msf-bcd format required */
	  bin2bcdx(&drvcmd[1]);
	  bin2bcdx(&drvcmd[2]);
	  bin2bcdx(&drvcmd[3]);
	}
      else
	{
	  drvcmd[1]=(block>>16)&0x000000ff;
	  drvcmd[2]=(block>>8)&0x000000ff;
	  drvcmd[3]=block&0x000000ff;
	}
      drvcmd[4]=0;
      drvcmd[5]=DS[d].sbp_read_frames;
      drvcmd[6]=(DS[d].drv_type<drv_201)?0:2; /* flag "lba or msf-bcd format" */
      drvcmd[0]=0x02;              /* "read frames" command for old drives */
      flags_cmd_out |= f_lopsta|f_getsta|f_bit1;
    }
  else /* if new_drive */
    {
      lba2msf(block,&drvcmd[1]); /* msf-bin format required */
      drvcmd[4]=0;
      drvcmd[5]=0;
      drvcmd[6]=DS[d].sbp_read_frames;
      drvcmd[0]=0x10;              /* "read frames" command for new drives */
    }
#if SBPCD_DIS_IRQ
  cli();
#endif SBPCD_DIS_IRQ
  for (i=0;i<7;i++) OUT(CDo_command,drvcmd[i]);
#if SBPCD_DIS_IRQ
  sti();
#endif SBPCD_DIS_IRQ

  return;
}
/*==========================================================================*/
/*
 *  Check the completion of the read-data command.  On success, read
 *  the SBP_BUFFER_FRAMES * 2048 bytes of data from the disk into buffer.
 */
static int sbp_data(void)
{
  int i=0, j=0, frame;
  u_int try=0;
  u_long timeout;
  u_char *p;
  u_int data_tries = 0;
  u_int data_waits = 0;
  u_int data_retrying = 0;
  int error_flag;

  error_flag=0;

  for (frame=DS[d].sbp_current;frame<DS[d].sbp_read_frames&&!error_flag; frame++)
    {
#if SBPCD_DIS_IRQ
      cli();
#endif SBPCD_DIS_IRQ
      try=maxtim_data;
      for (timeout=jiffies+100; ; )
	{
	  for ( ; try!=0;try--)
	    {
	      j=inb(CDi_status);
	      if (!(j&s_not_data_ready)) break;
	      if (!(j&s_not_result_ready)) break;
	      if (!new_drive) if (j&s_attention) break;
	    }
	  if (try != 0 || timeout <= jiffies) break;
	  if (data_retrying == 0) data_waits++;
	  data_retrying = 1;
	  sbp_sleep(1);
	  try = 1;
	}
      if (try==0)
	{
	  DPRINTF((DBG_INF,"SBPCD: sbp_data: CDi_status timeout.\n"));
	  error_flag++;
	  break;
	}

      if (j&s_not_data_ready)
	{
	  if ((DS[d].ored_ctl_adr&0x40)==0)
	    printk("SBPCD: CD contains no data tracks.\n");
	  else printk("SBPCD: sbp_data: DATA_READY timeout.\n");
	  error_flag++;
	  break;
	}
      
#if SBPCD_DIS_IRQ
      sti();
#endif SBPCD_DIS_IRQ

      CLEAR_TIMER;
      error_flag=0;
      p = DS[d].sbp_buf + frame *  CD_FRAMESIZE;

      if (sbpro_type) OUT(CDo_sel_d_i,0x01);
      READ_DATA(CDi_data, p, CD_FRAMESIZE);
      if (sbpro_type) OUT(CDo_sel_d_i,0x00);
      DS[d].sbp_current++;

      data_tries++;
      data_retrying = 0;
      if (data_tries >= 1000)
	{
	  DPRINTF((DBG_INF,"SBPCD: info: %d waits in %d frames.\n",
		        data_waits, data_tries));
	  data_waits = data_tries = 0;
	}
    }
#if SBPCD_DIS_IRQ
  sti();
#endif SBPCD_DIS_IRQ
  
  if (error_flag)    /* must have been spurious D_RDY or (ATTN&&!D_RDY) */
    {
      DPRINTF((DBG_INF,"SBPCD: read aborted by drive\n"));
      i=DriveReset();                /* ugly fix to prevent a hang */
      return (0);
    }

  if (!new_drive)
    {
#if SBPCD_DIS_IRQ
      cli();
#endif	SBPCD_DIS_IRQ
      i=maxtim_data;
      for (timeout=jiffies+100; timeout > jiffies; timeout--)
	{
	  for ( ;i!=0;i--)
	    {
	      j=inb(CDi_status);
	      if (!(j&s_not_data_ready)) break;
	      if (!(j&s_not_result_ready)) break;
	      if (j&s_attention) break;
	    }
	  if (i != 0 || timeout <= jiffies) break;
	  sbp_sleep(0);
	  i = 1;
	}
      if (i==0) { DPRINTF((DBG_INF,"SBPCD: STATUS TIMEOUT AFTER READ")); }
      if (!(j&s_attention))
	{
	  DPRINTF((DBG_INF,"SBPCD: sbp_data: timeout waiting DRV_ATTN - retrying\n"));
	  i=DriveReset();  /* ugly fix to prevent a hang */
#if SBPCD_DIS_IRQ
	  sti();
#endif SBPCD_DIS_IRQ
	  return (0);
	}
#if SBPCD_DIS_IRQ
      sti();
#endif SBPCD_DIS_IRQ
    }

  do
    {
      if (!new_drive) xx_ReadStatus();
      i=ResponseStatus();  /* builds status_byte, returns orig. status (old) or faked p_success_old (new) */
      if (i<0) { DPRINTF((DBG_INF,"SBPCD: xx_ReadStatus error after read: %02X\n",
			       DS[d].status_byte));
		 return (0);
	       }
    }
  while ((!new_drive)&&(!st_check)&&(!(i&p_success_old)));
  if (st_check)
    {
      i=xx_ReadError();
      DPRINTF((DBG_INF,"SBPCD: xx_ReadError was necessary after read: %02X\n",i));
      return (0);
    }

  DS[d].sbp_first_frame = CURRENT -> sector / 4;
  DS[d].sbp_last_frame = DS[d].sbp_first_frame + DS[d].sbp_read_frames - 1;
  sbp_transfer();
  return (1);
}
/*==========================================================================*/
/*==========================================================================*/
/*
 *  Open the device special file.  Check that a disk is in. Read TOC.
 */
int sbpcd_open(struct inode *ip, struct file *fp)
{
  int i;

  if (ndrives==0) return (-ENXIO);             /* no hardware */

  i = MINOR(ip->i_rdev);
  if ( (i<0) || (i>=NR_SBPCD) )
    {
      printk("SBPCD: open: bad device: %d\n", i);
      return (-ENODEV);             /* no such drive */
    }
  switch_drive(i);

  if (!st_spinning) xx_SpinUp();

  flags_cmd_out |= f_respo2;
  xx_ReadStatus();                         /* command: give 1-byte status */
  i=ResponseStatus();
  if (i<0)
    {
      DPRINTF((DBG_INF,"SBPCD: sbpcd_open: xx_ReadStatus timed out\n"));
      return (-EIO);                  /* drive doesn't respond */
    }
  DPRINTF((DBG_STA,"SBPCD: sbpcd_open: status %02X\n", DS[d].status_byte));
  if (!st_door_closed||!st_caddy_in)
    {
      printk("SBPCD: sbpcd_open: no disk in drive\n");
      return (-EIO);
    }

/*
 * we could try to keep an "open" counter here and lock the door if 0->1.
 * not done yet.
 */

  
  if (!st_spinning) xx_SpinUp();

  i=DiskInfo();
  if ((DS[d].ored_ctl_adr&0x40)==0)
    DPRINTF((DBG_INF,"SBPCD: CD contains no data tracks.\n"));
  return (0);
}
/*==========================================================================*/
/*
 *  On close, we flush all sbp blocks from the buffer cache.
 */
static void sbpcd_release(struct inode * ip, struct file * file)
{
  int i;
/*
 * we could try to count down an "open" counter here
 * and unlock the door if zero.
 * not done yet.
 */

  i = MINOR(ip->i_rdev);
  if ( (i<0) || (i>=NR_SBPCD) ) 
    {
      printk("SBPCD: release: bad device: %d\n", i);
      return;
    }
  switch_drive(i);

  DS[d].sbp_first_frame=DS[d].sbp_last_frame=-1;
  sync_dev(ip->i_rdev);                    /* nonsense if read only device? */
  invalidate_buffers(ip->i_rdev);
  DS[d].diskstate_flags &= ~cd_size_bit;
}
/*==========================================================================*/
/*
 *
 */
static struct file_operations sbpcd_fops =
{
  NULL,                   /* lseek - default */
  block_read,             /* read - general block-dev read */
  block_write,            /* write - general block-dev write */
  NULL,                   /* readdir - bad */
  NULL,                   /* select */
  sbpcd_ioctl,            /* ioctl */
  NULL,                   /* mmap */
  sbpcd_open,             /* open */
  sbpcd_release           /* release */
};
/*==========================================================================*/
/*
 *  SBP interrupt descriptor
 */
#if SBPCD_USE_IRQ
static struct sigaction sbpcd_sigaction =
{
  sbpcd_interrupt,
  0,
  SA_INTERRUPT,
  NULL
};
#endif SBPCD_USE_IRQ
/*==========================================================================*/
/*
 * accept "kernel command line" parameters 
 * (suggested by Peter MacDonald with SLS 1.03)
 *
 * use: tell LILO:
 *                 sbpcd=0x230,SoundBlaster
 *             or
 *                 sbpcd=0x300,LaserMate
 *
 * (upper/lower case sensitive here!!!).
 *
 * the address value has to be the TRUE CDROM PORT ADDRESS -
 * not the soundcard base address.
 *
 */
void sbpcd_setup(char *s, int *p)
{
  DPRINTF((DBG_INI,"SBPCD: sbpcd_setup called with %04X,%s\n",p[1], s));
  if (!strcmp(s,str_sb)) sbpro_type=1;
  else sbpro_type=0;
  if (p[0]>0) sbpcd_ioaddr=p[1];

  CDo_command=sbpcd_ioaddr;
  CDi_info=sbpcd_ioaddr;
  CDi_status=sbpcd_ioaddr+1;
  CDo_reset=sbpcd_ioaddr+2;
  CDo_enable=sbpcd_ioaddr+3; 
  if (sbpro_type==1)
    {
      MIXER_addr=sbpcd_ioaddr-0x10+0x04;
      MIXER_data=sbpcd_ioaddr-0x10+0x05;
      CDo_sel_d_i=sbpcd_ioaddr+1;
      CDi_data=sbpcd_ioaddr;
    }
  else CDi_data=sbpcd_ioaddr+2;
}
/*==========================================================================*/
/*
 *  Test for presence of drive and initialize it.  Called at boot time.
 */
u_long sbpcd_init(u_long mem_start, u_long mem_end)
{
  int i=0, j=0;
  int addr[2]={1, CDROM_PORT};
  int port_index;
   
  DPRINTF((DBG_INF,"SBPCD version %s\n", VERSION));

  DPRINTF((DBG_INF,"SBPCD: Looking for a SoundBlaster/Matsushita CD-ROM drive\n"));
  DPRINTF((DBG_WRN,"SBPCD: \n"));
  DPRINTF((DBG_WRN,"SBPCD: = = = = = = = = = = W A R N I N G = = = = = = = = = =\n"));
  DPRINTF((DBG_WRN,"SBPCD: Auto-Probing can cause a hang (f.e. touching an ethernet card).\n"));
  DPRINTF((DBG_WRN,"SBPCD: If that happens, you have to reboot and use the\n"));
  DPRINTF((DBG_WRN,"SBPCD: LILO (kernel) command line feature like:\n"));
  DPRINTF((DBG_WRN,"SBPCD: \n"));
  DPRINTF((DBG_WRN,"SBPCD:    LILO boot: linux sbpcd=0x230,SoundBlaster\n"));
  DPRINTF((DBG_WRN,"SBPCD: or like:\n"));
  DPRINTF((DBG_WRN,"SBPCD:    LILO boot: linux sbpcd=0x300,LaserMate\n"));
  DPRINTF((DBG_WRN,"SBPCD: \n"));
  DPRINTF((DBG_WRN,"SBPCD: with your REAL address.\n"));
  DPRINTF((DBG_WRN,"SBPCD: = = = = = = = = = = END of WARNING = = = = = = = = = =\n"));
  DPRINTF((DBG_WRN,"SBPCD: \n"));
  sti(); /* to avoid possible "printk" bug */

  autoprobe[0]=sbpcd_ioaddr; /* possibly changed by kernel command line */
  autoprobe[1]=sbpro_type; /* possibly changed by kernel command line */

  for (port_index=0;port_index<NUM_AUTOPROBE;port_index+=2)
    {
      addr[1]=autoprobe[port_index];
      if (check_region(addr[1],4)) continue;
      DPRINTF((DBG_INI,"SBPCD: check_region done.\n"));
      if (autoprobe[port_index+1]==0) type=str_lm;
      else type=str_sb;
      sbpcd_setup(type, addr);
      DPRINTF((DBG_INF,"SBPCD: Trying to detect a %s CD-ROM drive at 0x%X.\n",
	             type, CDo_command));

      DPRINTF((DBG_INF,"SBPCD: - "));
      sti(); /* to avoid possible "printk" bug */
      i=check_drives();
      DPRINTF((DBG_INI,"SBPCD: check_drives done.\n"));
      sti(); /* to avoid possible "printk" bug */
      if (i>=0) break; /* drive found */
      DPRINTF((DBG_INF,"\n"));
      sti(); /* to avoid possible "printk" bug */
    } /* end of cycling through the set of possible I/O port addresses */

  if (ndrives==0)
    {
      printk("SBPCD: No drive found.\n");
      sti();
      return (mem_start);
    }

  if (port_index>0)
    {
      printk("SBPCD: You should configure sbpcd.h for your hardware.\n");
      sti();
    }

  printk("SBPCD: %d %s CD-ROM drive(s) at 0x%04X.\n",
	   ndrives, type, CDo_command);
  sti(); /* to avoid possible "printk" bug */
  check_datarate();
  DPRINTF((DBG_INI,"SBPCD: check_datarate done.\n"));
  sti(); /* to avoid possible "printk" bug */

  for (j=0;j<NR_SBPCD;j++)
    {
      if (DS[j].drv_minor==-1) continue;
      switch_drive(j);
      xy_DriveReset();
      if (!st_spinning) xx_SpinUp();
      DS[d].sbp_first_frame = -1;  /* First frame in buffer */
      DS[d].sbp_last_frame = -1;   /* Last frame in buffer  */
      DS[d].sbp_read_frames = 0;   /* Number of frames being read to buffer */
      DS[d].sbp_current = 0;       /* Frame being currently read */
      DS[d].CD_changed=1;
      DS[d].frame_size=CD_FRAMESIZE;

      xx_ReadStatus();
      i=ResponseStatus();  /* returns orig. status or p_busy_new */
      if (i<0)
	DPRINTF((DBG_INF,"SBPCD: init: ResponseStatus returns %02X\n",i));
      else
	{
	  if (st_check)
	    {
	      i=xx_ReadError();
	      DPRINTF((DBG_INI,"SBPCD: init: xx_ReadError returns %d\n",i));
	      sti(); /* to avoid possible "printk" bug */
	    }
	}
      DPRINTF((DBG_INI,"SBPCD: init: first GetStatus: %d\n",i));
      sti(); /* to avoid possible "printk" bug */
      if (DS[d].error_byte==aud_12)
	{
	  do { i=GetStatus();
	       DPRINTF((DBG_INI,"SBPCD: init: second GetStatus: %02X\n",i));
	       sti(); /* to avoid possible "printk" bug */
	       if (i<0) break;
	       if (!st_caddy_in) break;
	     }
	  while (!st_diskok);
	}
      i=SetSpeed();
      if (i>=0) DS[d].CD_changed=1;
    }

  if (sbpro_type)
    {
      OUT(MIXER_addr,MIXER_CD_Volume);
      OUT(MIXER_data,0xCC); /* one nibble per channel */
    }
  
  if (register_blkdev(MATSUSHITA_CDROM_MAJOR, "sbpcd", &sbpcd_fops) != 0)
    {
      printk("SBPCD: Can't get MAJOR %d for Matsushita CDROM\n",
	       MATSUSHITA_CDROM_MAJOR);
      sti(); /* to avoid possible "printk" bug */
      return (mem_start);
    }
  blk_dev[MATSUSHITA_CDROM_MAJOR].request_fn = DEVICE_REQUEST;
  read_ahead[MATSUSHITA_CDROM_MAJOR] = 4; /* just one frame */ 
  
  snarf_region(CDo_command,4);

#if SBPCD_USE_IRQ
  if (irqaction(SBPCD_INTR_NR, &sbpcd_sigaction))
    {
      printk("SBPCD: Can't get IRQ%d for sbpcd driver\n", SBPCD_INTR_NR);
      sti(); /* to avoid possible "printk" bug */
    }
#endif SBPCD_USE_IRQ

/*
 * allocate memory for the frame buffers
 */ 
  for (j=0;j<NR_SBPCD;j++)
    {
      if (DS[j].drv_minor==-1) continue;
      DS[j].sbp_buf=(u_char *)mem_start;
      mem_start += SBP_BUFFER_FRAMES*CD_FRAMESIZE;
    }
  DPRINTF((DBG_INF,"SBPCD: init done.\n"));
  sti(); /* to avoid possible "printk" bug */
  return (mem_start);
}
/*==========================================================================*/
/*
 * adopted from sr.c
 *
 * Check if the media has changed in the CD-ROM drive.
 * used externally (isofs/inode.c) - but still does not work.
 *
 */
int check_sbpcd_media_change(int full_dev, int unused_minor)
{
  int st;

  if (MAJOR(full_dev) != MATSUSHITA_CDROM_MAJOR) 
    {
      printk("SBPCD: media_check: invalid device.\n");
      return (-1);
    }
  
  xx_ReadStatus();                         /* command: give 1-byte status */
  st=ResponseStatus();
  DPRINTF((DBG_CHK,"SBPCD: media_check: %02X\n",DS[d].status_byte));
  if (st<0)
    {
      DPRINTF((DBG_INF,"SBPCD: media_check: ResponseStatus error.\n"));
      return (1); /* status not obtainable */
    }
  if (DS[d].CD_changed==0xFF) DPRINTF((DBG_CHK,"SBPCD: media_check: \"changed\" assumed.\n"));
  if (!st_spinning) DPRINTF((DBG_CHK,"SBPCD: media_check: motor off.\n"));
  if (!st_door_closed)
    {
      DPRINTF((DBG_CHK,"SBPCD: media_check: door open.\n"));
      DS[d].CD_changed=0xFF;
    }
  if (!st_caddy_in)
    {
      DPRINTF((DBG_CHK,"SBPCD: media_check: no disk in drive.\n"));
      DS[d].CD_changed=0xFF;
    }
  if (!st_diskok) DPRINTF((DBG_CHK,"SBPCD: media_check: !st_diskok.\n"));
  
#if 0000
  if (DS[d].CD_changed==0xFF)
    {
      DS[d].CD_changed=1;
      return (1); /* driver had a change detected before */
    }
#endif 0000 /* seems to give additional errors at the moment */

  if (!st_diskok) return (1); /* disk not o.k. */
  if (!st_caddy_in) return (1); /* disk removed */
  if (!st_door_closed) return (1); /* door open */
  return (0);
}
/*==========================================================================*/
