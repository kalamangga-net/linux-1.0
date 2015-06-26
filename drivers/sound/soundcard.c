
/*
 * linux/kernel/chr_drv/sound/soundcard.c
 * 
 * Soundcard driver for Linux
 * 
 * Copyright by Hannu Savolainen 1993
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer. 2.
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * 
 */

#include "sound_config.h"

#ifdef CONFIGURE_SOUNDCARD

#include <linux/major.h>

extern long     seq_time;

static int      soundcards_installed = 0;	/* Number of installed
						 * soundcards */
static int      soundcard_configured = 0;

static struct fileinfo files[SND_NDEVS];

extern char    *snd_raw_buf[MAX_DSP_DEV][DSP_BUFFCOUNT];
extern unsigned long snd_raw_buf_phys[MAX_DSP_DEV][DSP_BUFFCOUNT];
extern int      snd_raw_count[MAX_DSP_DEV];

int
snd_ioctl_return (int *addr, int value)
{
  if (value < 0)
    return value;

  PUT_WORD_TO_USER (addr, 0, value);
  return 0;
}

static int
sound_read (struct inode *inode, struct file *file, char *buf, int count)
{
  int             dev;

  dev = inode->i_rdev;
  dev = MINOR (dev);

  return sound_read_sw (dev, &files[dev], buf, count);
}

static int
sound_write (struct inode *inode, struct file *file, char *buf, int count)
{
  int             dev;

  dev = inode->i_rdev;
  dev = MINOR (dev);

  return sound_write_sw (dev, &files[dev], buf, count);
}

static int
sound_lseek (struct inode *inode, struct file *file, off_t offset, int orig)
{
  return RET_ERROR (EPERM);
}

static int
sound_open (struct inode *inode, struct file *file)
{
  int             dev;

  dev = inode->i_rdev;
  dev = MINOR (dev);

  if (!soundcard_configured && dev != SND_DEV_CTL && dev != SND_DEV_STATUS)
    {
      printk ("SoundCard Error: The soundcard system has not been configured\n");
      return RET_ERROR (ENXIO);
    }

  files[dev].mode = 0;

  if ((file->f_flags & O_ACCMODE) == O_RDWR)
    files[dev].mode = OPEN_READWRITE;
  if ((file->f_flags & O_ACCMODE) == O_RDONLY)
    files[dev].mode = OPEN_READ;
  if ((file->f_flags & O_ACCMODE) == O_WRONLY)
    files[dev].mode = OPEN_WRITE;

  return sound_open_sw (dev, &files[dev]);
}

static void
sound_release (struct inode *inode, struct file *file)
{
  int             dev;

  dev = inode->i_rdev;
  dev = MINOR (dev);

  sound_release_sw (dev, &files[dev]);
}

static int
sound_ioctl (struct inode *inode, struct file *file,
	     unsigned int cmd, unsigned long arg)
{
  int             dev;

  dev = inode->i_rdev;
  dev = MINOR (dev);

  return sound_ioctl_sw (dev, &files[dev], cmd, arg);
}

static int
sound_select (struct inode *inode, struct file *file, int sel_type, select_table * wait)
{
  int             dev;

  dev = inode->i_rdev;
  dev = MINOR (dev);

  DEB (printk ("sound_select(dev=%d, type=0x%x)\n", dev, sel_type));

  switch (dev & 0x0f)
    {
    case SND_DEV_SEQ:
      return sequencer_select (dev, &files[dev], sel_type, wait);
      break;

    default:
      return 0;
    }

  return 0;
}

static struct file_operations sound_fops =
{
  sound_lseek,
  sound_read,
  sound_write,
  NULL,				/* sound_readdir */
  sound_select,
  sound_ioctl,
  NULL,
  sound_open,
  sound_release
};

long
soundcard_init (long mem_start)
{
  register_chrdev (SOUND_MAJOR, "sound", &sound_fops);

  soundcard_configured = 1;

  mem_start = sndtable_init (mem_start);	/* Initialize call tables and
						 * detect cards */

  if (!(soundcards_installed = sndtable_get_cardcount ()))
    return mem_start;		/* No cards detected */

  if (num_dspdevs)		/* Audio devices present */
    {
      mem_start = DMAbuf_init (mem_start);
      mem_start = audio_init (mem_start);
    }

#ifndef EXCLUDE_MPU401
  if (num_midis)
    mem_start = MIDIbuf_init (mem_start);
#endif

  if (num_midis + num_synths)
    mem_start = sequencer_init (mem_start);

  return mem_start;
}

void
tenmicrosec (void)
{
  int             i;

  for (i = 0; i < 16; i++)
    inb (0x80);
}

int
snd_set_irq_handler (int interrupt_level, void(*hndlr)(int))
{
  int             retcode;

  struct sigaction sa;

  sa.sa_handler = hndlr;

#ifdef SND_SA_INTERRUPT
  sa.sa_flags = SA_INTERRUPT;
#else
  sa.sa_flags = 0;
#endif

  sa.sa_mask = 0;
  sa.sa_restorer = NULL;

  retcode = irqaction (interrupt_level, &sa);

  if (retcode < 0)
    {
      printk ("Sound: IRQ%d already in use\n", interrupt_level);
    }

  return retcode;
}

void
snd_release_irq(int vect)
{
	free_irq(vect);
}

void
request_sound_timer (int count)
{
#ifndef EXCLUDE_SEQUENCER
  if (count < 0)
    count = jiffies + (-count);
  else
    count += seq_time;
  timer_table[SOUND_TIMER].fn = sequencer_timer;
  timer_table[SOUND_TIMER].expires = count;
  timer_active |= 1 << SOUND_TIMER;
#endif
}

void
sound_stop_timer (void)
{
#ifndef EXCLUDE_SEQUENCER
  timer_table[SOUND_TIMER].expires = 0;
  timer_active &= ~(1 << SOUND_TIMER);
#endif
}

#ifndef EXCLUDE_AUDIO
static int
valid_dma_page (unsigned long addr, unsigned long dev_buffsize, unsigned long dma_pagesize)
{
  if (((addr & (dma_pagesize - 1)) + dev_buffsize) <= dma_pagesize)
    return 1;
  else
    return 0;
}

void
sound_mem_init (void)
{
  int             i, dev;
  unsigned long   start_addr, end_addr, mem_ptr, dma_pagesize;

  mem_ptr = high_memory;

  /* Some sanity checks */

  if (mem_ptr > (16 * 1024 * 1024))
    mem_ptr = 16 * 1024 * 1024;	/* Limit to 16M */

  for (dev = 0; dev < num_dspdevs; dev++)	/* Enumerate devices */
    if (sound_buffcounts[dev] > 0 && sound_dsp_dmachan[dev] > 0)
      {
	if (sound_dma_automode[dev])
	  sound_buffcounts[dev] = 1;

	if (sound_dsp_dmachan[dev] > 3 && sound_buffsizes[dev] > 65536)
	  dma_pagesize = 131072;/* 128k */
	else
	  dma_pagesize = 65536;

	/* More sanity checks */

	if (sound_buffsizes[dev] > dma_pagesize)
	  sound_buffsizes[dev] = dma_pagesize;
	sound_buffsizes[dev] &= 0xfffff000;	/* Truncate to n*4k */
	if (sound_buffsizes[dev] < 4096)
	  sound_buffsizes[dev] = 4096;

	/* Now allocate the buffers */

	for (snd_raw_count[dev] = 0; snd_raw_count[dev] < sound_buffcounts[dev]; snd_raw_count[dev]++)
	  {
	    start_addr = mem_ptr - sound_buffsizes[dev];
	    if (!valid_dma_page (start_addr, sound_buffsizes[dev], dma_pagesize))
	      start_addr &= ~(dma_pagesize - 1);	/* Align address to
							 * dma_pagesize */

	    end_addr = start_addr + sound_buffsizes[dev] - 1;

	    snd_raw_buf[dev][snd_raw_count[dev]] = (char *) start_addr;
	    snd_raw_buf_phys[dev][snd_raw_count[dev]] = start_addr;
	    mem_ptr = start_addr;

	    for (i = MAP_NR (start_addr); i <= MAP_NR (end_addr); i++)
	      {
		if (mem_map[i])
		  panic ("sound_mem_init: Page not free (driver incompatible with kernel).\n");

		mem_map[i] = MAP_PAGE_RESERVED;
	      }
	  }
      }				/* for dev */
}

#endif

#else

long
soundcard_init (long mem_start)	/* Dummy version */
{
  return mem_start;
}

#endif

#if !defined(CONFIGURE_SOUNDCARD) || defined(EXCLUDE_AUDIO)
void
sound_mem_init (void)
{
  /* Dummy version */
}

#endif
