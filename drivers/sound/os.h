/*
 *	OS Specific settings for Linux
 * 
 * Copyright by Hannu Savolainen 1993
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#define ALLOW_SELECT

#include <linux/param.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/fcntl.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/tty.h>
#include <linux/ctype.h>
#include <asm/io.h>
#include <asm/segment.h>
#include <asm/system.h>
#include <asm/dma.h>
#include <sys/kd.h>
#include <linux/wait.h>
#include <linux/malloc.h>
#include <linux/soundcard.h>

typedef char snd_rw_buf;

#define FALSE	0
#define TRUE	1

#define COPY_FROM_USER(d, s, o, c)	memcpy_fromfs((d), &((s)[o]), (c))
#define COPY_TO_USER(d, o, s, c)	memcpy_tofs(&((d)[o]), (s), (c))
#define IOCTL_FROM_USER(d, s, o, c)	memcpy_fromfs((d), &((s)[o]), (c))
#define IOCTL_TO_USER(d, o, s, c)	memcpy_tofs(&((d)[o]), (s), (c))

#define GET_BYTE_FROM_USER(target, addr, offs)	target = get_fs_byte(&((addr)[offs]))
#define GET_SHORT_FROM_USER(target, addr, offs)	target = get_fs_word(&((addr)[offs]))
#define GET_WORD_FROM_USER(target, addr, offs)	target = get_fs_long((long*)&((addr)[offs]))
#define PUT_WORD_TO_USER(addr, offs, data)	put_fs_long(data, (long*)&((addr)[offs]))
#define IOCTL_IN(arg)			get_fs_long((long *)(arg))
#define IOCTL_OUT(arg, ret)		snd_ioctl_return((int *)arg, ret)

struct snd_wait {
	  int mode; int aborting;
	};

#define DEFINE_WAIT_QUEUE(name, flag) static struct wait_queue *name = NULL; \
	static volatile struct snd_wait flag = {0}
#define DEFINE_WAIT_QUEUES(name, flag) static struct wait_queue *name = {NULL}; \
	static volatile struct snd_wait flag = {{0}}
#define RESET_WAIT_QUEUE(q, f) {f.aborting = 0;f.mode = WK_NONE;}
#define PROCESS_ABORTING(q, f) (f.aborting | (current->signal & ~current->blocked))
#define SET_ABORT_FLAG(q, f) f.aborting = 1
#define TIMED_OUT(q, f) (f.mode & WK_TIMEOUT)
#define DO_SLEEP(q, f, time_limit)	\
	{ unsigned long tl;\
	  if (time_limit) tl = current->timeout = jiffies + (time_limit); \
	     else tl = 0xffffffff; \
	  f.mode = WK_SLEEP;interruptible_sleep_on(&q); \
	  if (!(f.mode & WK_WAKEUP)) \
	   { \
	     if (current->signal & ~current->blocked) \
	        f.aborting = 1; \
	     else \
	        if (jiffies >= tl) f.mode |= WK_TIMEOUT; \
	   } \
	  f.mode &= ~WK_SLEEP; \
	}
#define SOMEONE_WAITING(q, f) (f.mode & WK_SLEEP)
#define WAKE_UP(q, f)			{f.mode = WK_WAKEUP;wake_up(&q);}

#define ALLOC_DMA_CHN(chn)		request_dma(chn)
#define RELEASE_DMA_CHN(chn)		free_dma(chn)

#define GET_TIME()			jiffies
#define RELEASE_IRQ			free_irq
#define RET_ERROR(err)			-err

/* DISABLE_INTR is used to disable interrupts.
   These macros store the current flags to the (unsigned long) variable given
   as a parameter. RESTORE_INTR returns the interrupt ebable bit to state
   before DISABLE_INTR or ENABLE_INTR */

#define DISABLE_INTR(flags)	__asm__ __volatile__("pushfl ; popl %0 ; cli":"=r" (flags));
#define RESTORE_INTR(flags)	__asm__ __volatile__("pushl %0 ; popfl": \
							:"r" (flags));
/* 
   KERNEL_MALLOC() allocates requested number of memory  and 
   KERNEL_FREE is used to free it. 
   These macros are never called from interrupt, in addition the
   nbytes will never be more than 4096 bytes. Generally the driver
   will allocate memory in blocks of 4k. If the kernel has just a
   page level memory allocation, 4K can be safely used as the size
   (the nbytes parameter can be ignored).
*/
#define KERNEL_MALLOC(nbytes)	kmalloc(nbytes, GFP_KERNEL)
#define KERNEL_FREE(addr)	kfree(addr)

/*
 * The macro PERMANENT_MALLOC(typecast, mem_ptr, size, linux_ptr)
 * returns size bytes of
 * (kernel virtual) memory which will never get freed by the driver.
 * This macro is called only during boot. The linux_ptr is a linux specific
 * parameter which should be ignored in other operating systems.
 * The mem_ptr is a pointer variable where the macro assigns pointer to the
 * memory area. The type is the type of the mem_ptr.
 */
#define PERMANENT_MALLOC(typecast, mem_ptr, size, linux_ptr) \
  {mem_ptr = (typecast)linux_ptr; \
   linux_ptr += (size);}

/*
 * The macro DEFINE_TIMER defines variables for the ACTIVATE_TIMER if
 * required. The name is the variable/name to be used and the proc is
 * the procedure to be called when the timer expires.
 */

#define DEFINE_TIMER(name, proc) \
  static struct timer_list name = \
  {NULL, NULL, 0, 0, proc}

/*
 * The ACTIVATE_TIMER requests system to call 'proc' after 'time' ticks.
 */

#define ACTIVATE_TIMER(name, proc, time) \
  {name.expires = time; \
  add_timer (&name);}

#define INB	inb
#define OUTB	outb
