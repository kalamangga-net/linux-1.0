/*
 * Sony CDU-31A CDROM interface device driver.
 *
 * Corey Minyard (minyard@wf-rch.cirr.com)
 *
 * Colossians 3:17
 *
 * The Sony interface device driver handles Sony interface CDROM
 * drives and provides a complete block-level interface as well as an
 * ioctl() interface compatible with the Sun (as specified in
 * include/linux/cdrom.h).  With this interface, CDROMs can be
 * accessed and standard audio CDs can be played back normally.
 *
 * This interface is (unfortunatly) a polled interface.  This is
 * because most Sony interfaces are set up with DMA and interrupts
 * disables.  Some (like mine) do not even have the capability to
 * handle interrupts or DMA.  For this reason you will see a lot of
 * the following:
 *
 *   retry_count = jiffies+ SONY_JIFFIES_TIMEOUT;
 *   while ((retry_count > jiffies) && (! <some condition to wait for))
 *   {
 *      while (handle_sony_cd_attention())
 *         ;
 *
 *      sony_sleep();
 *   }
 *   if (the condition not met)
 *   {
 *      return an error;
 *   }
 *
 * This ugly hack waits for something to happen, sleeping a little
 * between every try.  it also handles attentions, which are
 * asyncronous events from the drive informing the driver that a disk
 * has been inserted, removed, etc.
 *
 * One thing about these drives: They talk in MSF (Minute Second Frame) format.
 * There are 75 frames a second, 60 seconds a minute, and up to 75 minutes on a
 * disk.  The funny thing is that these are sent to the drive in BCD, but the
 * interface wants to see them in decimal.  A lot of conversion goes on.
 *
 *  Copyright (C) 1993  Corey Minyard
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */



#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/hdreg.h>
#include <linux/genhd.h>
#include <linux/ioport.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#include <linux/cdrom.h>
#include <linux/cdu31a.h>

#define MAJOR_NR CDU31A_CDROM_MAJOR
#include "blk.h"

#define CDU31A_MAX_CONSECUTIVE_ATTENTIONS 10

static unsigned short cdu31a_addresses[] =
{
   0x340,	/* Standard configuration Sony Interface */
   0x1f88,	/* Fusion CD-16 */
   0x230,	/* SoundBlaster 16 card */
   0x360,	/* Secondary standard Sony Interface */
   0x320,	/* Secondary standard Sony Interface */
   0x330,	/* Secondary standard Sony Interface */
   0
};


static int handle_sony_cd_attention(void);
static int read_subcode(void);
static void sony_get_toc(void);
static int scd_open(struct inode *inode, struct file *filp);
static void do_sony_cd_cmd(unsigned char cmd,
                           unsigned char *params,
                           unsigned int num_params,
                           unsigned char *result_buffer,
                           unsigned int *result_size);
static void size_to_buf(unsigned int size,
                        unsigned char *buf);


/* The base I/O address of the Sony Interface.  This is a variable (not a
   #define) so it can be easily changed via some future ioctl() */
static unsigned short sony_cd_base_io = 0;

/*
 * The following are I/O addresses of the various registers for the drive.  The
 * comment for the base address also applies here.
 */
static volatile unsigned short sony_cd_cmd_reg;
static volatile unsigned short sony_cd_param_reg;
static volatile unsigned short sony_cd_write_reg;
static volatile unsigned short sony_cd_control_reg;
static volatile unsigned short sony_cd_status_reg;
static volatile unsigned short sony_cd_result_reg;
static volatile unsigned short sony_cd_read_reg;
static volatile unsigned short sony_cd_fifost_reg;


static int sony_disc_changed = 1;          /* Has the disk been changed
                                              since the last check? */
static int sony_toc_read = 0;              /* Has the table of contents been
                                              read? */
static int sony_spun_up = 0;               /* Has the drive been spun up? */
static unsigned int sony_buffer_size;      /* Size in bytes of the read-ahead
                                              buffer. */
static unsigned int sony_buffer_sectors;   /* Size (in 2048 byte records) of
                                              the read-ahead buffer. */
static unsigned int sony_usage = 0;        /* How many processes have the
                                              drive open. */

static volatile int sony_first_block = -1; /* First OS block (512 byte) in
                                              the read-ahead buffer */
static volatile int sony_last_block = -1;  /* Last OS block (512 byte) in
                                              the read-ahead buffer */

static struct s_sony_toc *sony_toc;              /* Points to the table of
                                                    contents. */
static struct s_sony_subcode * volatile last_sony_subcode; /* Points to the last
                                                    subcode address read */
static unsigned char * volatile sony_buffer;     /* Points to the read-ahead
                                                    buffer */

static volatile int sony_inuse = 0;  /* Is the drive in use?  Only one operation at a time
                                        allowed */

static struct wait_queue * sony_wait = NULL;

static struct task_struct *has_cd_task = NULL;  /* The task that is currently using the
                                                   CDROM drive, or NULL if none. */

/*
 * The audio status uses the values from read subchannel data as specified
 * in include/linux/cdrom.h.
 */
static volatile int sony_audio_status = CDROM_AUDIO_NO_STATUS;

/*
 * The following are a hack for pausing and resuming audio play.  The drive
 * does not work as I would expect it, if you stop it then start it again,
 * the drive seeks back to the beginning and starts over.  This holds the
 * position during a pause so a resume can restart it.  It uses the
 * audio status variable above to tell if it is paused.
 */
unsigned volatile char cur_pos_msf[3] = { 0, 0, 0 };
unsigned volatile char final_pos_msf[3] = { 0, 0, 0 };

/*
 * This routine returns 1 if the disk has been changed since the last
 * check or 0 if it hasn't.  Setting flag to 0 resets the changed flag.
 */
int
check_cdu31a_media_change(int full_dev, int flag)
{
   int retval, target;


   target = MINOR(full_dev);

   if (target > 0) {
      printk("Sony CD-ROM request error: invalid device.\n");
      return 0;
   }

   retval = sony_disc_changed;
   if (!flag)
   {
      sony_disc_changed = 0;
   }

   return retval;
}


/*
 * Wait a little while (used for polling the drive).  If in initialization,
 * setting a timeout doesn't work, so just loop for a while.
 */
static inline void
sony_sleep(void)
{
   current->state = TASK_INTERRUPTIBLE;
   current->timeout = jiffies;
   schedule();
}


/*
 * The following are convenience routine to read various status and set
 * various conditions in the drive.
 */
static inline int
is_attention(void)
{
   return((inb(sony_cd_status_reg) & SONY_ATTN_BIT) != 0);
}

static inline int
is_busy(void)
{
   return((inb(sony_cd_status_reg) & SONY_BUSY_BIT) != 0);
}

static inline int
is_data_ready(void)
{
   return((inb(sony_cd_status_reg) & SONY_DATA_RDY_BIT) != 0);
}

static inline int
is_data_requested(void)
{
   return((inb(sony_cd_status_reg) & SONY_DATA_REQUEST_BIT) != 0);
}

static inline int
is_result_ready(void)
{
   return((inb(sony_cd_status_reg) & SONY_RES_RDY_BIT) != 0);
}

static inline int
is_param_write_rdy(void)
{
   return((inb(sony_cd_fifost_reg) & SONY_PARAM_WRITE_RDY_BIT) != 0);
}

static inline void
reset_drive(void)
{
   outb(SONY_DRIVE_RESET_BIT, sony_cd_control_reg);
}

static inline void
clear_attention(void)
{
   outb(SONY_ATTN_CLR_BIT, sony_cd_control_reg);
}

static inline void
clear_result_ready(void)
{
   outb(SONY_RES_RDY_CLR_BIT, sony_cd_control_reg);
}

static inline void
clear_data_ready(void)
{
   outb(SONY_DATA_RDY_CLR_BIT, sony_cd_control_reg);
}

static inline void
clear_param_reg(void)
{
   outb(SONY_PARAM_CLR_BIT, sony_cd_control_reg);
}

static inline unsigned char
read_status_register(void)
{
   return(inb(sony_cd_status_reg));
}

static inline unsigned char
read_result_register(void)
{
   return(inb(sony_cd_result_reg));
}

static inline unsigned char
read_data_register(void)
{
   return(inb(sony_cd_read_reg));
}

static inline void
write_param(unsigned char param)
{
   outb(param, sony_cd_param_reg);
}

static inline void
write_cmd(unsigned char cmd)
{
   outb(cmd, sony_cd_cmd_reg);
   outb(SONY_RES_RDY_INT_EN_BIT, sony_cd_control_reg);
}

/*
 * Set the drive parameters so the drive will auto-spin-up when a
 * disk is inserted.
 */
static void
set_drive_params(void)
{
   unsigned char res_reg[2];
   unsigned int res_size;
   unsigned char params[3];


   params[0] = SONY_SD_MECH_CONTROL;
   params[1] = 0x03;
   do_sony_cd_cmd(SONY_SET_DRIVE_PARAM_CMD,
                  params,
                  2,
                  res_reg,
                  &res_size);
   if ((res_size < 2) || ((res_reg[0] & 0x20) == 0x20))
   {
      printk("  Unable to set mechanical parameters: 0x%2.2x\n", res_reg[1]);
   }
}

/*
 * This code will reset the drive and attempt to restore sane parameters.
 */
static void
restart_on_error(void)
{
   unsigned char res_reg[2];
   unsigned int res_size;
   unsigned int retry_count;


   printk("cdu31a: Resetting drive on error\n");
   reset_drive();
   retry_count = jiffies + SONY_RESET_TIMEOUT;
   while ((retry_count > jiffies) && (!is_attention()))
   {
      sony_sleep();
   }
   set_drive_params();
   do_sony_cd_cmd(SONY_SPIN_UP_CMD, NULL, 0, res_reg, &res_size);
   if ((res_size < 2) || ((res_reg[0] & 0x20) == 0x20))
   {
      printk("cdu31a: Unable to spin up drive: 0x%2.2x\n", res_reg[1]);
   }

   current->state = TASK_INTERRUPTIBLE;
   current->timeout = jiffies + 200;
   schedule();

   do_sony_cd_cmd(SONY_READ_TOC_CMD, NULL, 0, res_reg, &res_size);
   if ((res_size < 2) || ((res_reg[0] & 0x20) == 0x20))
   {
      printk("cdu31a: Unable to read TOC: 0x%2.2x\n", res_reg[1]);
   }
   sony_get_toc();
   if (!sony_toc_read)
   {
      printk("cdu31a: Unable to get TOC data\n");
   }
}

/*
 * This routine writes data to the parameter register.  Since this should
 * happen fairly fast, it is polled with no OS waits between.
 */
static int
write_params(unsigned char *params,
             int num_params)
{
   unsigned int retry_count;


   retry_count = SONY_READY_RETRIES;
   while ((retry_count > 0) && (!is_param_write_rdy()))
   {
      retry_count--;
   }
   if (!is_param_write_rdy())
   {
      return -EIO;
   }

   while (num_params > 0)
   {
      write_param(*params);
      params++;
      num_params--;
   }

   return 0;
}


/*
 * The following reads data from the command result register.  It is a
 * fairly complex routine, all status info flows back through this
 * interface.  The algorithm is stolen directly from the flowcharts in
 * the drive manual.
 */
static void
get_result(unsigned char *result_buffer,
           unsigned int *result_size)
{
   unsigned char a, b;
   int i;
   unsigned int retry_count;


   while (handle_sony_cd_attention())
      ;
   /* Wait for the result data to be ready */
   retry_count = jiffies + SONY_JIFFIES_TIMEOUT;
   while ((retry_count > jiffies) && (is_busy() || (!(is_result_ready()))))
   {
      sony_sleep();

      while (handle_sony_cd_attention())
         ;
   }
   if (is_busy() || (!(is_result_ready())))
   {
      result_buffer[0] = 0x20;
      result_buffer[1] = SONY_TIMEOUT_OP_ERR;
      *result_size = 2;
      return;
   }

   /*
    * Get the first two bytes.  This determines what else needs
    * to be done.
    */
   clear_result_ready();
   a = read_result_register();
   *result_buffer = a;
   result_buffer++;
   b = read_result_register();
   *result_buffer = b;
   result_buffer++;
   *result_size = 2;

   /*
    * 0x20 means an error occured.  Byte 2 will have the error code.
    * Otherwise, the command succeded, byte 2 will have the count of
    * how many more status bytes are coming.
    *
    * The result register can be read 10 bytes at a time, a wait for
    * result ready to be asserted must be done between every 10 bytes.
    */
   if ((a & 0xf0) != 0x20)
   {
      if (b > 8)
      {
         for (i=0; i<8; i++)
         {
            *result_buffer = read_result_register();
            result_buffer++;
            (*result_size)++;
         }
         b = b - 8;

         while (b > 10)
         {
            retry_count = SONY_READY_RETRIES;
            while ((retry_count > 0) && (!is_result_ready()))
            {
               retry_count--;
            }
            if (!is_result_ready())
            {
               result_buffer[0] = 0x20;
               result_buffer[1] = SONY_TIMEOUT_OP_ERR;
               *result_size = 2;
               return;
            }

            clear_result_ready();
                                
            for (i=0; i<10; i++)
            {
               *result_buffer = read_result_register();
               result_buffer++;
               (*result_size)++;
            }
            b = b - 10;
         }

         if (b > 0)
         {
            retry_count = SONY_READY_RETRIES;
            while ((retry_count > 0) && (!is_result_ready()))
            {
               retry_count--;
            }
            if (!is_result_ready())
            {
               result_buffer[0] = 0x20;
               result_buffer[1] = SONY_TIMEOUT_OP_ERR;
               *result_size = 2;
               return;
            }
         }
      }

      while (b > 0)
      {
         *result_buffer = read_result_register();
         result_buffer++;
         (*result_size)++;
         b--;
      }
   }
}

/*
 * Read in a 2048 byte block of data.
 */
static void
read_data_block(unsigned char *data,
                unsigned char *result_buffer,
                unsigned int  *result_size)
{
   int i;
   unsigned int retry_count;

   for (i=0; i<2048; i++)
   {
      retry_count = jiffies + SONY_JIFFIES_TIMEOUT;
      while ((retry_count > jiffies) && (!is_data_requested()))
      {
         while (handle_sony_cd_attention())
            ;
         
         sony_sleep();
      }
      if (!is_data_requested())
      {
         result_buffer[0] = 0x20;
         result_buffer[1] = SONY_TIMEOUT_OP_ERR;
         *result_size = 2;
         return;
      }
      
      *data = read_data_register();
      data++;
   }
}

/*
 * This routine issues a read data command and gets the data.  I don't
 * really like the way this is done (I would prefer for do_sony_cmd() to
 * handle it automatically) but I found that the drive returns status
 * when it finishes reading (not when the host has read all the data)
 * or after it gets an error.  This means that the status can be
 * received at any time and should be handled immediately (at least
 * between every 2048 byte block) to check for errors, we can't wait
 * until all the data is read.
 *
 * This routine returns the total number of sectors read.  It will
 * not return an error if it reads at least one sector successfully.
 */
static unsigned int
get_data(unsigned char *orig_data,
         unsigned char *params,         /* 6 bytes with the MSF start address
                                           and number of sectors to read. */
         unsigned int orig_data_size,
         unsigned char *result_buffer,
         unsigned int *result_size)
{
   unsigned int cur_offset;
   unsigned int retry_count;
   int result_read;
   int num_retries;
   unsigned int num_sectors_read = 0;
   unsigned char *data = orig_data;
   unsigned int data_size = orig_data_size;


   cli();
   while (sony_inuse)
   {
      interruptible_sleep_on(&sony_wait);
      if (current->signal & ~current->blocked)
      {
         result_buffer[0] = 0x20;
         result_buffer[1] = SONY_SIGNAL_OP_ERR;
         *result_size = 2;
         return 0;
      }
   }
   sony_inuse = 1;
   has_cd_task = current;
   sti();

   num_retries = 0;
retry_data_operation:
   result_buffer[0] = 0;
   result_buffer[1] = 0;

   /*
    * Clear any outstanding attentions and wait for the drive to
    * complete any pending operations.
    */
   while (handle_sony_cd_attention())
      ;

   retry_count = jiffies + SONY_JIFFIES_TIMEOUT;
   while ((retry_count > jiffies) && (is_busy()))
   {
      sony_sleep();
      
      while (handle_sony_cd_attention())
         ;
   }

   if (is_busy())
   {
      result_buffer[0] = 0x20;
      result_buffer[1] = SONY_TIMEOUT_OP_ERR;
      *result_size = 2;
   }
   else
   {
      /* Issue the command */
      clear_result_ready();
      clear_param_reg();

      write_params(params, 6);
      write_cmd(SONY_READ_CMD);

      /*
       * Read the data from the drive one 2048 byte sector at a time.  Handle
       * any results received between sectors, if an error result is returned
       * terminate the operation immediately.
       */
      cur_offset = 0;
      result_read = 0;
      while ((data_size > 0) && (result_buffer[0] == 0))
      {
         /* Wait for the drive to tell us we have something */
         retry_count = jiffies + SONY_JIFFIES_TIMEOUT;
         while ((retry_count > jiffies) && (!(is_result_ready() || is_data_ready())))
         {
            while (handle_sony_cd_attention())
               ;

            sony_sleep();
         }
         if (!(is_result_ready() || is_data_ready()))
         {
            result_buffer[0] = 0x20;
            result_buffer[1] = SONY_TIMEOUT_OP_ERR;
            *result_size = 2;
         }
      
         /* Handle results first */
         else if (is_result_ready())
         {
            result_read = 1;
            get_result(result_buffer, result_size);
         }
         else /* Handle data next */
         {
            /*
             * The drive has to be polled for status on a byte-by-byte basis
             * to know if the data is ready.  Yuck.  I really wish I could use DMA.
             */
            clear_data_ready();
            read_data_block(data, result_buffer, result_size);
            data += 2048;
            data_size -= 2048;
            cur_offset = cur_offset + 2048;
            num_sectors_read++;
         }
      }

      /* Make sure the result has been read */
      if (!result_read)
      {
         get_result(result_buffer, result_size);
      }
   }

   if (   ((result_buffer[0] & 0x20) == 0x20)
       && (result_buffer[1] != SONY_NOT_SPIN_ERR) /* No retry when not spin */
       && (num_retries < MAX_CDU31A_RETRIES))
   {
      /*
       * If an error occurs, go back and only read one sector at the
       * given location.  Hopefully the error occurred on an unused
       * sector after the first one.  It is hard to say which sector
       * the error occurred on because the drive returns status before
       * the data transfer is finished and doesn't say which sector.
       */
      data_size = 2048;
      data = orig_data;
      num_sectors_read = 0;
      size_to_buf(1, &params[3]);

      num_retries++;
      /* Issue a reset on an error (the second time), othersize just delay */
      if (num_retries == 2)
      {
         restart_on_error();
      }
      else
      {
         current->state = TASK_INTERRUPTIBLE;
         current->timeout = jiffies + 10;
         schedule();
      }

      /* Restart the operation. */
      goto retry_data_operation;
   }

   has_cd_task = NULL;
   sony_inuse = 0;
   wake_up_interruptible(&sony_wait);

   return(num_sectors_read);
}


/*
 * Do a command that does not involve data transfer.  This routine must
 * be re-entrant from the same task to support being called from the
 * data operation code when an error occurs.
 */
static void
do_sony_cd_cmd(unsigned char cmd,
               unsigned char *params,
               unsigned int num_params,
               unsigned char *result_buffer,
               unsigned int *result_size)
{
   unsigned int retry_count;
   int num_retries;
   int recursive_call;


   cli();
   if (current != has_cd_task) /* Allow recursive calls to this routine */
   {
      while (sony_inuse)
      {
         interruptible_sleep_on(&sony_wait);
         if (current->signal & ~current->blocked)
         {
            result_buffer[0] = 0x20;
            result_buffer[1] = SONY_SIGNAL_OP_ERR;
            *result_size = 2;
            return;
         }
      }
      sony_inuse = 1;
      has_cd_task = current;
      recursive_call = 0;
   }
   else
   {
      recursive_call = 1;
   }
   sti();

   num_retries = 0;
retry_cd_operation:

   while (handle_sony_cd_attention())
      ;
   
   retry_count = jiffies + SONY_JIFFIES_TIMEOUT;
   while ((retry_count > jiffies) && (is_busy()))
   {
      sony_sleep();
      
      while (handle_sony_cd_attention())
         ;
   }
   if (is_busy())
   {
      result_buffer[0] = 0x20;
      result_buffer[1] = SONY_TIMEOUT_OP_ERR;
      *result_size = 2;
   }
   else
   {
      clear_result_ready();
      clear_param_reg();

      write_params(params, num_params);
      write_cmd(cmd);

      get_result(result_buffer, result_size);
   }

   if (   ((result_buffer[0] & 0x20) == 0x20)
       && (num_retries < MAX_CDU31A_RETRIES))
   {
      num_retries++;
      current->state = TASK_INTERRUPTIBLE;
      current->timeout = jiffies + 10; /* Wait .1 seconds on retries */
      schedule();
      goto retry_cd_operation;
   }

   if (!recursive_call)
   {
      has_cd_task = NULL;
      sony_inuse = 0;
      wake_up_interruptible(&sony_wait);
   }
}


/*
 * Handle an attention from the drive.  This will return 1 if it found one
 * or 0 if not (if one is found, the caller might want to call again).
 *
 * This routine counts the number of consecutive times it is called
 * (since this is always called from a while loop until it returns
 * a 0), and returns a 0 if it happens too many times.  This will help
 * prevent a lockup.
 */
static int
handle_sony_cd_attention(void)
{
   unsigned char atten_code;
   static int num_consecutive_attentions = 0;


   if (is_attention())
   {
      if (num_consecutive_attentions > CDU31A_MAX_CONSECUTIVE_ATTENTIONS)
      {
         printk("cdu31a: Too many consecutive attentions: %d\n",
                num_consecutive_attentions);
         num_consecutive_attentions = 0;
         return(0);
      }

      clear_attention();
      atten_code = read_result_register();

      switch (atten_code)
      {
       /* Someone changed the CD.  Mark it as changed */
      case SONY_MECH_LOADED_ATTN:
         sony_disc_changed = 1;
         sony_toc_read = 0;
         sony_audio_status = CDROM_AUDIO_NO_STATUS;
         sony_first_block = -1;
         sony_last_block = -1;
         break;

      case SONY_AUDIO_PLAY_DONE_ATTN:
         sony_audio_status = CDROM_AUDIO_COMPLETED;
         read_subcode();
         break;

      case SONY_EJECT_PUSHED_ATTN:
         sony_audio_status = CDROM_AUDIO_INVALID;
         break;

      case SONY_LEAD_IN_ERR_ATTN:
      case SONY_LEAD_OUT_ERR_ATTN:
      case SONY_DATA_TRACK_ERR_ATTN:
      case SONY_AUDIO_PLAYBACK_ERR_ATTN:
         sony_audio_status = CDROM_AUDIO_ERROR;
         break;
      }

      num_consecutive_attentions++;
      return(1);
   }

   num_consecutive_attentions = 0;
   return(0);
}


/* Convert from an integer 0-99 to BCD */
static inline unsigned int
int_to_bcd(unsigned int val)
{
   int retval;


   retval = (val / 10) << 4;
   retval = retval | val % 10;
   return(retval);
}


/* Convert from BCD to an integer from 0-99 */
static unsigned int
bcd_to_int(unsigned int bcd)
{
   return((((bcd >> 4) & 0x0f) * 10) + (bcd & 0x0f));
}


/*
 * Convert a logical sector value (like the OS would want to use for
 * a block device) to an MSF format.
 */
static void
log_to_msf(unsigned int log, unsigned char *msf)
{
   log = log + LOG_START_OFFSET;
   msf[0] = int_to_bcd(log / 4500);
   log = log % 4500;
   msf[1] = int_to_bcd(log / 75);
   msf[2] = int_to_bcd(log % 75);
}


/*
 * Convert an MSF format to a logical sector.
 */
static unsigned int
msf_to_log(unsigned char *msf)
{
   unsigned int log;


   log = bcd_to_int(msf[2]);
   log += bcd_to_int(msf[1]) * 75;
   log += bcd_to_int(msf[0]) * 4500;
   log = log - LOG_START_OFFSET;

   return log;
}


/*
 * Take in integer size value and put it into a buffer like
 * the drive would want to see a number-of-sector value.
 */
static void
size_to_buf(unsigned int size,
            unsigned char *buf)
{
   buf[0] = size / 65536;
   size = size % 65536;
   buf[1] = size / 256;
   buf[2] = size % 256;
}


/*
 * The OS calls this to perform a read or write operation to the drive.
 * Write obviously fail.  Reads to a read ahead of sony_buffer_size
 * bytes to help speed operations.  This especially helps since the OS
 * uses 1024 byte blocks and the drive uses 2048 byte blocks.  Since most
 * data access on a CD is done sequentially, this saves a lot of operations.
 */
static void
do_cdu31a_request(void)
{
   int block;
   unsigned int dev;
   int nsect;
   unsigned char params[10];
   unsigned char res_reg[2];
   unsigned int res_size;
   int copyoff;
   int spin_up_retry;
   unsigned int read_size;


   if (!sony_spun_up)
   {
      scd_open (NULL,NULL);
   }

   while (1)
   {
cdu31a_request_startover:
      /*
       * The beginning here is stolen from the hard disk driver.  I hope
       * its right.
       */
      if (!(CURRENT) || CURRENT->dev < 0)
      {
         return;
      }

      INIT_REQUEST;
      dev = MINOR(CURRENT->dev);
      block = CURRENT->sector;
      nsect = CURRENT->nr_sectors;
      if (dev != 0)
      {
         end_request(0);
         goto cdu31a_request_startover;
      }

      switch(CURRENT->cmd)
      {
      case READ:
         /*
          * If the block address is invalid or the request goes beyond the end of
          * the media, return an error.
          */
         if ((block / 4) >= sony_toc->lead_out_start_lba)
         {
            end_request(0);
            goto cdu31a_request_startover;
         }
         if (((block + nsect) / 4) >= sony_toc->lead_out_start_lba)
         {
            end_request(0);
            goto cdu31a_request_startover;
         }

         while (nsect > 0)
         {
            /*
             * If the requested sector is not currently in the read-ahead buffer,
             * it must be read in.
             */
            if ((block < sony_first_block) || (block > sony_last_block))
            {
               sony_first_block = (block / 4) * 4;
               log_to_msf(block/4, params);

               /*
                * If the full read-ahead would go beyond the end of the media, trim
                * it back to read just till the end of the media.
                */
               if (((block / 4) + sony_buffer_sectors) >= sony_toc->lead_out_start_lba)
               {
                  read_size = sony_toc->lead_out_start_lba - (block / 4);
               }
               else
               {
                  read_size = sony_buffer_sectors;
               }
               size_to_buf(read_size, &params[3]);

               /*
                * Read the data.  If the drive was not spinning, spin it up and try
                * once more.  I know, the goto is ugly, but I am too lazy to fix it.
                */
               spin_up_retry = 0;
try_read_again:
               sony_last_block =   sony_first_block
                                 + (get_data(sony_buffer,
                                             params,
                                             (read_size * 2048),
                                             res_reg,
                                             &res_size) * 4) - 1;
               if ((res_size < 2) || (res_reg[0] != 0))
               {
                  if ((res_reg[1] == SONY_NOT_SPIN_ERR) && (!spin_up_retry))
                  {
                     do_sony_cd_cmd(SONY_SPIN_UP_CMD, NULL, 0, res_reg, &res_size);
                     spin_up_retry = 1;
                     goto try_read_again;
                  }

                  printk("Sony CDROM Read error: 0x%2.2x\n", res_reg[1]);
                  sony_first_block = -1;
                  sony_last_block = -1;
                  end_request(0);
                  goto cdu31a_request_startover;
               }
            }
   
            /*
             * The data is in memory now, copy it to the buffer and advance to the
             * next block to read.
             */
            copyoff = (block - sony_first_block) * 512;
            memcpy(CURRENT->buffer, sony_buffer+copyoff, 512);
               
            block += 1;
            nsect -= 1;
            CURRENT->buffer += 512;
         }
               
         end_request(1);
         break;
            
      case WRITE:
         end_request(0);
         break;
            
      default:
         panic("Unkown SONY CD cmd");
      }
   }
}


/*
 * Read the table of contents from the drive and set sony_toc_read if
 * successful.
 */
static void
sony_get_toc(void)
{
   unsigned int res_size;


   if (!sony_toc_read)
   {
      do_sony_cd_cmd(SONY_REQ_TOC_DATA_CMD,
                     NULL,
                     0, 
                     (unsigned char *) sony_toc, 
                     &res_size);
      if ((res_size < 2) || ((sony_toc->exec_status[0] & 0x20) == 0x20))
      {
         return;
      }
      sony_toc->lead_out_start_lba = msf_to_log(sony_toc->lead_out_start_msf);
      sony_toc_read = 1;
   }
}


/*
 * Search for a specific track in the table of contents.
 */
static int
find_track(int track)
{
   int i;
   int num_tracks;


   num_tracks = sony_toc->last_track_num + sony_toc->first_track_num + 1;
   for (i = 0; i < num_tracks; i++)
   {
      if (sony_toc->tracks[i].track == track)
      {
         return i;
      }
   }

   return -1;
}


/*
 * Read the subcode and put it int last_sony_subcode for future use.
 */
static int
read_subcode(void)
{
   unsigned int res_size;


   do_sony_cd_cmd(SONY_REQ_SUBCODE_ADDRESS_CMD,
                  NULL,
                  0, 
                  (unsigned char *) last_sony_subcode, 
                  &res_size);
   if ((res_size < 2) || ((last_sony_subcode->exec_status[0] & 0x20) == 0x20))
   {
      printk("Sony CDROM error 0x%2.2x (read_subcode)\n",
             last_sony_subcode->exec_status[1]);
      return -EIO;
   }

   return 0;
}


/*
 * Get the subchannel info like the CDROMSUBCHNL command wants to see it.  If
 * the drive is playing, the subchannel needs to be read (since it would be
 * changing).  If the drive is paused or completed, the subcode information has
 * already been stored, just use that.  The ioctl call wants things in decimal
 * (not BCD), so all the conversions are done.
 */
static int
sony_get_subchnl_info(long arg)
{
   struct cdrom_subchnl schi;


   /* Get attention stuff */
   while (handle_sony_cd_attention())
      ;

   sony_get_toc();
   if (!sony_toc_read)
   {
      return -EIO;
   }

   verify_area(VERIFY_READ, (char *) arg, sizeof(schi));
   verify_area(VERIFY_WRITE, (char *) arg, sizeof(schi));

   memcpy_fromfs(&schi, (char *) arg, sizeof(schi));
   
   switch (sony_audio_status)
   {
   case CDROM_AUDIO_PLAY:
      if (read_subcode() < 0)
      {
         return -EIO;
      }
      break;

   case CDROM_AUDIO_PAUSED:
   case CDROM_AUDIO_COMPLETED:
      break;

   case CDROM_AUDIO_NO_STATUS:
      schi.cdsc_audiostatus = sony_audio_status;
      memcpy_tofs((char *) arg, &schi, sizeof(schi));
      return 0;
      break;

   case CDROM_AUDIO_INVALID:
   case CDROM_AUDIO_ERROR:
   default:
      return -EIO;
   }

   schi.cdsc_audiostatus = sony_audio_status;
   schi.cdsc_adr = last_sony_subcode->address;
   schi.cdsc_ctrl = last_sony_subcode->control;
   schi.cdsc_trk = bcd_to_int(last_sony_subcode->track_num);
   schi.cdsc_ind = bcd_to_int(last_sony_subcode->index_num);
   if (schi.cdsc_format == CDROM_MSF)
   {
      schi.cdsc_absaddr.msf.minute = bcd_to_int(last_sony_subcode->abs_msf[0]);
      schi.cdsc_absaddr.msf.second = bcd_to_int(last_sony_subcode->abs_msf[1]);
      schi.cdsc_absaddr.msf.frame = bcd_to_int(last_sony_subcode->abs_msf[2]);

      schi.cdsc_reladdr.msf.minute = bcd_to_int(last_sony_subcode->rel_msf[0]);
      schi.cdsc_reladdr.msf.second = bcd_to_int(last_sony_subcode->rel_msf[1]);
      schi.cdsc_reladdr.msf.frame = bcd_to_int(last_sony_subcode->rel_msf[2]);
   }
   else if (schi.cdsc_format == CDROM_LBA)
   {
      schi.cdsc_absaddr.lba = msf_to_log(last_sony_subcode->abs_msf);
      schi.cdsc_reladdr.lba = msf_to_log(last_sony_subcode->rel_msf);
   }
   
   memcpy_tofs((char *) arg, &schi, sizeof(schi));
   return 0;
}


/*
 * The big ugly ioctl handler.
 */
static int
scd_ioctl(struct inode *inode,
          struct file  *file,
          unsigned int  cmd,
          unsigned long arg)
{
   unsigned int dev;
   unsigned char res_reg[2];
   unsigned int res_size;
   unsigned char params[7];
   int i;


   if (!inode)
   {
      return -EINVAL;
   }
   dev = MINOR(inode->i_rdev) >> 6;
   if (dev != 0)
   {
      return -EINVAL;
   }

   switch (cmd)
   {
   case CDROMSTART:     /* Spin up the drive */
      do_sony_cd_cmd(SONY_SPIN_UP_CMD, NULL, 0, res_reg, &res_size);
      if ((res_size < 2) || ((res_reg[0] & 0x20) == 0x20))
      {
         printk("Sony CDROM error 0x%2.2x (CDROMSTART)\n", res_reg[1]);
         return -EIO;
      }
      return 0;
      break;
      
   case CDROMSTOP:      /* Spin down the drive */
      do_sony_cd_cmd(SONY_AUDIO_STOP_CMD, NULL, 0, res_reg, &res_size);

      /*
       * Spin the drive down, ignoring the error if the disk was
       * already not spinning.
       */
      sony_audio_status = CDROM_AUDIO_NO_STATUS;
      do_sony_cd_cmd(SONY_SPIN_DOWN_CMD, NULL, 0, res_reg, &res_size);
      if (   ((res_size < 2) || ((res_reg[0] & 0x20) == 0x20))
          && (res_reg[1] != SONY_NOT_SPIN_ERR))
      {
         printk("Sony CDROM error 0x%2.2x (CDROMSTOP)\n", res_reg[1]);
         return -EIO;
      }
      
      return 0;
      break;

   case CDROMPAUSE:     /* Pause the drive */
      do_sony_cd_cmd(SONY_AUDIO_STOP_CMD, NULL, 0, res_reg, &res_size);
      if ((res_size < 2) || ((res_reg[0] & 0x20) == 0x20))
      {
         printk("Sony CDROM error 0x%2.2x (CDROMPAUSE)\n", res_reg[1]);
         return -EIO;
      }

      /* Get the current position and save it for resuming */
      if (read_subcode() < 0)
      {
         return -EIO;
      }
      cur_pos_msf[0] = last_sony_subcode->abs_msf[0];
      cur_pos_msf[1] = last_sony_subcode->abs_msf[1];
      cur_pos_msf[2] = last_sony_subcode->abs_msf[2];
      sony_audio_status = CDROM_AUDIO_PAUSED;
      return 0;
      break;

   case CDROMRESUME:    /* Start the drive after being paused */
      if (sony_audio_status != CDROM_AUDIO_PAUSED)
      {
         return -EINVAL;
      }
      
      do_sony_cd_cmd(SONY_SPIN_UP_CMD, NULL, 0, res_reg, &res_size);
      
      /* Start the drive at the saved position. */
      params[1] = cur_pos_msf[0];
      params[2] = cur_pos_msf[1];
      params[3] = cur_pos_msf[2];
      params[4] = final_pos_msf[0];
      params[5] = final_pos_msf[1];
      params[6] = final_pos_msf[2];
      params[0] = 0x03;
      do_sony_cd_cmd(SONY_AUDIO_PLAYBACK_CMD, params, 7, res_reg, &res_size);
      if ((res_size < 2) || ((res_reg[0] & 0x20) == 0x20))
      {
         printk("Sony CDROM error 0x%2.2x (CDROMRESUME)\n", res_reg[1]);
         return -EIO;
      }
      sony_audio_status = CDROM_AUDIO_PLAY;
      return 0;
      break;

   case CDROMPLAYMSF:   /* Play starting at the given MSF address. */
      verify_area(VERIFY_READ, (char *) arg, 6);
      do_sony_cd_cmd(SONY_SPIN_UP_CMD, NULL, 0, res_reg, &res_size);
      memcpy_fromfs(&(params[1]), (void *) arg, 6);
      
      /* The parameters are given in int, must be converted */
      for (i=1; i<7; i++)
      {
         params[i] = int_to_bcd(params[i]);
      }
      params[0] = 0x03;
      do_sony_cd_cmd(SONY_AUDIO_PLAYBACK_CMD, params, 7, res_reg, &res_size);
      if ((res_size < 2) || ((res_reg[0] & 0x20) == 0x20))
      {
         printk("Sony CDROM error 0x%2.2x (CDROMPLAYMSF)\n", res_reg[1]);
         return -EIO;
      }
      
      /* Save the final position for pauses and resumes */
      final_pos_msf[0] = params[4];
      final_pos_msf[1] = params[5];
      final_pos_msf[2] = params[6];
      sony_audio_status = CDROM_AUDIO_PLAY;
      return 0;
      break;

   case CDROMREADTOCHDR:        /* Read the table of contents header */
      {
         struct cdrom_tochdr *hdr;
         struct cdrom_tochdr loc_hdr;
         
         sony_get_toc();
         if (!sony_toc_read)
         {
            return -EIO;
         }
         
         hdr = (struct cdrom_tochdr *) arg;
         verify_area(VERIFY_WRITE, hdr, sizeof(*hdr));
         loc_hdr.cdth_trk0 = bcd_to_int(sony_toc->first_track_num);
         loc_hdr.cdth_trk1 = bcd_to_int(sony_toc->last_track_num);
         memcpy_tofs(hdr, &loc_hdr, sizeof(*hdr));
      }
      return 0;
      break;

   case CDROMREADTOCENTRY:      /* Read a given table of contents entry */
      {
         struct cdrom_tocentry *entry;
         struct cdrom_tocentry loc_entry;
         int track_idx;
         unsigned char *msf_val = NULL;
         
         sony_get_toc();
         if (!sony_toc_read)
         {
            return -EIO;
         }
         
         entry = (struct cdrom_tocentry *) arg;
         verify_area(VERIFY_READ, entry, sizeof(*entry));
         verify_area(VERIFY_WRITE, entry, sizeof(*entry));
         
         memcpy_fromfs(&loc_entry, entry, sizeof(loc_entry));
         
         /* Lead out is handled separately since it is special. */
         if (loc_entry.cdte_track == CDROM_LEADOUT)
         {
            loc_entry.cdte_adr = sony_toc->address2;
            loc_entry.cdte_ctrl = sony_toc->control2;
            msf_val = sony_toc->lead_out_start_msf;
         }
         else
         {
            track_idx = find_track(int_to_bcd(loc_entry.cdte_track));
            if (track_idx < 0)
            {
               return -EINVAL;
            }
            
            loc_entry.cdte_adr = sony_toc->tracks[track_idx].address;
            loc_entry.cdte_ctrl = sony_toc->tracks[track_idx].control;
            msf_val = sony_toc->tracks[track_idx].track_start_msf;
         }
         
         /* Logical buffer address or MSF format requested? */
         if (loc_entry.cdte_format == CDROM_LBA)
         {
            loc_entry.cdte_addr.lba = msf_to_log(msf_val);
         }
         else if (loc_entry.cdte_format == CDROM_MSF)
         {
            loc_entry.cdte_addr.msf.minute = bcd_to_int(*msf_val);
            loc_entry.cdte_addr.msf.second = bcd_to_int(*(msf_val+1));
            loc_entry.cdte_addr.msf.frame = bcd_to_int(*(msf_val+2));
         }
         memcpy_tofs(entry, &loc_entry, sizeof(*entry));
      }
      return 0;
      break;

   case CDROMPLAYTRKIND:     /* Play a track.  This currently ignores index. */
      {
         struct cdrom_ti ti;
         int track_idx;
         
         sony_get_toc();
         if (!sony_toc_read)
         {
            return -EIO;
         }
         
         verify_area(VERIFY_READ, (char *) arg, sizeof(ti));
         
         memcpy_fromfs(&ti, (char *) arg, sizeof(ti));
         if (   (ti.cdti_trk0 < sony_toc->first_track_num)
             || (ti.cdti_trk0 > sony_toc->last_track_num)
             || (ti.cdti_trk1 < ti.cdti_trk0))
         {
            return -EINVAL;
         }
         
         track_idx = find_track(int_to_bcd(ti.cdti_trk0));
         if (track_idx < 0)
         {
            return -EINVAL;
         }
         params[1] = sony_toc->tracks[track_idx].track_start_msf[0];
         params[2] = sony_toc->tracks[track_idx].track_start_msf[1];
         params[3] = sony_toc->tracks[track_idx].track_start_msf[2];
         
         /*
          * If we want to stop after the last track, use the lead-out
          * MSF to do that.
          */
         if (ti.cdti_trk1 >= bcd_to_int(sony_toc->last_track_num))
         {
            log_to_msf(msf_to_log(sony_toc->lead_out_start_msf)-1,
                       &(params[4]));
         }
         else
         {
            track_idx = find_track(int_to_bcd(ti.cdti_trk1+1));
            if (track_idx < 0)
            {
               return -EINVAL;
            }
            log_to_msf(msf_to_log(sony_toc->tracks[track_idx].track_start_msf)-1,
                       &(params[4]));
         }
         params[0] = 0x03;
         
         do_sony_cd_cmd(SONY_SPIN_UP_CMD, NULL, 0, res_reg, &res_size);
         
         do_sony_cd_cmd(SONY_AUDIO_PLAYBACK_CMD, params, 7, res_reg, &res_size);
         if ((res_size < 2) || ((res_reg[0] & 0x20) == 0x20))
         {
            printk("Params: %x %x %x %x %x %x %x\n", params[0], params[1],
                   params[2], params[3], params[4], params[5], params[6]);
            printk("Sony CDROM error 0x%2.2x (CDROMPLAYTRKIND\n", res_reg[1]);
            return -EIO;
         }
         
         /* Save the final position for pauses and resumes */
         final_pos_msf[0] = params[4];
         final_pos_msf[1] = params[5];
         final_pos_msf[2] = params[6];
         sony_audio_status = CDROM_AUDIO_PLAY;
         return 0;
      }
     
   case CDROMSUBCHNL:   /* Get subchannel info */
      return sony_get_subchnl_info(arg);

   case CDROMVOLCTRL:   /* Volume control.  What volume does this change, anyway? */
      {
         struct cdrom_volctrl volctrl;
         
         verify_area(VERIFY_READ, (char *) arg, sizeof(volctrl));
         
         memcpy_fromfs(&volctrl, (char *) arg, sizeof(volctrl));
         params[0] = SONY_SD_AUDIO_VOLUME;
         params[1] = volctrl.channel0;
         params[2] = volctrl.channel1;
         do_sony_cd_cmd(SONY_SET_DRIVE_PARAM_CMD, params, 3, res_reg, &res_size);
         if ((res_size < 2) || ((res_reg[0] & 0x20) == 0x20))
         {
            printk("Sony CDROM error 0x%2.2x (CDROMVOLCTRL)\n", res_reg[1]);
            return -EIO;
         }
      }
      return 0;

   case CDROMEJECT:     /* Eject the drive */
      do_sony_cd_cmd(SONY_AUDIO_STOP_CMD, NULL, 0, res_reg, &res_size);
      do_sony_cd_cmd(SONY_SPIN_DOWN_CMD, NULL, 0, res_reg, &res_size);

      sony_audio_status = CDROM_AUDIO_INVALID;
      do_sony_cd_cmd(SONY_EJECT_CMD, NULL, 0, res_reg, &res_size);
      if ((res_size < 2) || ((res_reg[0] & 0x20) == 0x20))
      {
         printk("Sony CDROM error 0x%2.2x (CDROMEJECT)\n", res_reg[1]);
         return -EIO;
      }
      return 0;
      break;
     
   default:
      return -EINVAL;
   }
}


/*
 * Open the drive for operations.  Spin the drive up and read the table of
 * contents if these have not already been done.
 */
static int
scd_open(struct inode *inode,
         struct file *filp)
{
   unsigned char res_reg[2];
   unsigned int res_size;
   int num_spin_ups;


   if (!sony_spun_up)
   {
      num_spin_ups = 0;

respinup_on_open:
      do_sony_cd_cmd(SONY_SPIN_UP_CMD, NULL, 0, res_reg, &res_size);

      /* The drive sometimes returns error 0.  I don't know why, but ignore
         it.  It seems to mean the drive has already done the operation. */
      if ((res_size < 2) || ((res_reg[0] != 0) && (res_reg[1] != 0)))
      {
         printk("Sony CDROM error 0x%2.2x (scd_open, spin up)\n", res_reg[1]);
         return -EIO;
      }
      
      do_sony_cd_cmd(SONY_READ_TOC_CMD, NULL, 0, res_reg, &res_size);

      /* The drive sometimes returns error 0.  I don't know why, but ignore
         it.  It seems to mean the drive has already done the operation. */
      if ((res_size < 2) || ((res_reg[0] != 0) && (res_reg[1] != 0)))
      {
         /* If the drive is already playing, its ok.  */
         if ((res_reg[1] == SONY_AUDIO_PLAYING_ERR) || (res_reg[1] == 0))
         {
            goto drive_spinning;
         }

         /* If the drive says it is not spun up (even though we just did it!)
            then retry the operation at least a few times. */
         if (   (res_reg[1] == SONY_NOT_SPIN_ERR)
             && (num_spin_ups < MAX_CDU31A_RETRIES))
         {
            num_spin_ups++;
            goto respinup_on_open;
         }

         printk("Sony CDROM error 0x%2.2x (scd_open, read toc)\n", res_reg[1]);
         do_sony_cd_cmd(SONY_SPIN_DOWN_CMD, NULL, 0, res_reg, &res_size);
         
         return -EIO;
      }

      sony_get_toc();
      if (!sony_toc_read)
      {
         do_sony_cd_cmd(SONY_SPIN_DOWN_CMD, NULL, 0, res_reg, &res_size);
         return -EIO;
      }

      sony_spun_up = 1;
   }

drive_spinning:

   if (inode)
   {
      check_disk_change(inode->i_rdev);
   }

   sony_usage++;

   return 0;
}


/*
 * Close the drive.  Spin it down if no task is using it.  The spin
 * down will fail if playing audio, so audio play is OK.
 */
static void
scd_release(struct inode *inode,
         struct file *filp)
{
   unsigned char res_reg[2];
   unsigned int  res_size;


   if (sony_usage > 0)
   {
      sony_usage--;
   }
   if (sony_usage == 0)
   {
      sync_dev(inode->i_rdev);
      do_sony_cd_cmd(SONY_SPIN_DOWN_CMD, NULL, 0, res_reg, &res_size);

      sony_spun_up = 0;
   }
}


static struct file_operations scd_fops = {
   NULL,                   /* lseek - default */
   block_read,             /* read - general block-dev read */
   block_write,            /* write - general block-dev write */
   NULL,                   /* readdir - bad */
   NULL,                   /* select */
   scd_ioctl,              /* ioctl */
   NULL,                   /* mmap */
   scd_open,               /* open */
   scd_release,            /* release */
   NULL                    /* fsync */
};


/* The different types of disc loading mechanisms supported */
static char *load_mech[] = { "caddy", "tray", "pop-up", "unknown" };

/* Read-ahead buffer sizes for different drives.  These are just arbitrary
   values, I don't know what is really optimum. */
static unsigned int mem_size[] = { 16384, 16384, 16384, 2048 };

void
get_drive_configuration(unsigned short base_io,
                        unsigned char res_reg[],
                        unsigned int *res_size)
{
   int retry_count;


   /* Set the base address */
   sony_cd_base_io = base_io;

   /* Set up all the register locations */
   sony_cd_cmd_reg = sony_cd_base_io + SONY_CMD_REG_OFFSET;
   sony_cd_param_reg = sony_cd_base_io + SONY_PARAM_REG_OFFSET;
   sony_cd_write_reg = sony_cd_base_io + SONY_WRITE_REG_OFFSET;
   sony_cd_control_reg = sony_cd_base_io + SONY_CONTROL_REG_OFFSET;
   sony_cd_status_reg = sony_cd_base_io + SONY_STATUS_REG_OFFSET;
   sony_cd_result_reg = sony_cd_base_io + SONY_RESULT_REG_OFFSET;
   sony_cd_read_reg = sony_cd_base_io + SONY_READ_REG_OFFSET;
   sony_cd_fifost_reg = sony_cd_base_io + SONY_FIFOST_REG_OFFSET;

   /*
    * Check to see if anything exists at the status register location.
    * I don't know if this is a good way to check, but it seems to work
    * ok for me.
    */
   if (read_status_register() != 0xff)
   {
      /*
       * Reset the drive and wait for attention from it (to say its reset).
       * If you don't wait, the next operation will probably fail.
       */
      reset_drive();
      retry_count = jiffies + SONY_RESET_TIMEOUT;
      while ((retry_count > jiffies) && (!is_attention()))
      {
         sony_sleep();
      }

      /* If attention is never seen probably not a CDU31a present */
      if (!is_attention())
      {
         res_reg[0] = 0x20;
         return;
      }

      /*
       * Get the drive configuration.
       */
      do_sony_cd_cmd(SONY_REQ_DRIVE_CONFIG_CMD,
                     NULL,
                     0,
                     (unsigned char *) res_reg,
                     res_size);
      return;
   }

   /* Return an error */
   res_reg[0] = 0x20;
}


/*
 * Initialize the driver.
 */
unsigned long
cdu31a_init(unsigned long mem_start, unsigned long mem_end)
{
   struct s_sony_drive_config drive_config;
   unsigned int res_size;
   int i;
   int drive_found;


   /*
    * According to Alex Freed (freed@europa.orion.adobe.com), this is
    * required for the Fusion CD-16 package.  If the sound driver is
    * loaded, it should work fine, but just in case...
    *
    * The following turn on the CD-ROM interface for a Fusion CD-16.
    */
   outb(0xbc, 0x9a01);
   outb(0xe2, 0x9a01);

   i = 0;
   drive_found = 0;
   while (   (cdu31a_addresses[i] != 0)
          && (!drive_found))
   {
      if (check_region(cdu31a_addresses[i], 4)) {
	  i++;
	  continue;
      }
      get_drive_configuration(cdu31a_addresses[i],
                               drive_config.exec_status,
                               &res_size);
      if ((res_size > 2) && ((drive_config.exec_status[0] & 0x20) == 0x00))
      {
         drive_found = 1;
	 snarf_region(cdu31a_addresses[i], 4);

         if (register_blkdev(MAJOR_NR,"cdu31a",&scd_fops))
         {
            printk("Unable to get major %d for CDU-31a\n", MAJOR_NR);
            return mem_start;
         }

         sony_buffer_size = mem_size[SONY_HWC_GET_BUF_MEM_SIZE(drive_config)];
         sony_buffer_sectors = sony_buffer_size / 2048;

         printk("Sony I/F CDROM : %8.8s %16.16s %8.8s with %s load mechanism\n",
                drive_config.vendor_id,
                drive_config.product_id,
                drive_config.product_rev_level,
                load_mech[SONY_HWC_GET_LOAD_MECH(drive_config)]);
         printk("  using %d byte buffer", sony_buffer_size);
         if (SONY_HWC_AUDIO_PLAYBACK(drive_config))
         {
            printk(", capable of audio playback");
         }
         printk("\n");

         set_drive_params();

         blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;
         read_ahead[MAJOR_NR] = 8;               /* 8 sector (4kB) read-ahead */

         sony_toc = (struct s_sony_toc *) mem_start;
         mem_start += sizeof(*sony_toc);
         last_sony_subcode = (struct s_sony_subcode *) mem_start;
         mem_start += sizeof(*last_sony_subcode);
         sony_buffer = (unsigned char *) mem_start;
         mem_start += sony_buffer_size;
      }

      i++;
   }
   
   return mem_start;
}

