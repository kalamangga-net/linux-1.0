/*
 *  linux/kernel/printk.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 * Modified to make sys_syslog() more flexible: added commands to
 * return the last 4k of kernel messages, regardless of whether
 * they've been read or not.  Added option to suppress kernel printk's
 * to the console.  Added hook for sending the console messages
 * elsewhere, in preparation for a serial line console (someday).
 * Ted Ts'o, 2/11/93.
 */

#include <stdarg.h>

#include <asm/segment.h>
#include <asm/system.h>

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>

#define LOG_BUF_LEN	4096

static char buf[1024];

extern int vsprintf(char * buf, const char * fmt, va_list args);
extern void console_print(const char *);

#define DEFAULT_MESSAGE_LOGLEVEL 7 /* KERN_DEBUG */
#define DEFAULT_CONSOLE_LOGLEVEL 7 /* anything more serious than KERN_DEBUG */

unsigned long log_size = 0;
struct wait_queue * log_wait = NULL;
int console_loglevel = DEFAULT_CONSOLE_LOGLEVEL;

static void (*console_print_proc)(const char *) = 0;
static char log_buf[LOG_BUF_LEN];
static unsigned long log_start = 0;
static unsigned long logged_chars = 0;

/*
 * Commands to sys_syslog:
 *
 * 	0 -- Close the log.  Currently a NOP.
 * 	1 -- Open the log. Currently a NOP.
 * 	2 -- Read from the log.
 * 	3 -- Read up to the last 4k of messages in the ring buffer.
 * 	4 -- Read and clear last 4k of messages in the ring buffer
 * 	5 -- Clear ring buffer.
 * 	6 -- Disable printk's to console
 * 	7 -- Enable printk's to console
 *	8 -- Set level of messages printed to console
 */
asmlinkage int sys_syslog(int type, char * buf, int len)
{
	unsigned long i, j, count;
	int do_clear = 0;
	char c;
	int error;

	if ((type != 3) && !suser())
		return -EPERM;
	switch (type) {
		case 0:		/* Close log */
			return 0;
		case 1:		/* Open log */
			return 0;
		case 2:		/* Read from log */
			if (!buf || len < 0)
				return -EINVAL;
			if (!len)
				return 0;
			error = verify_area(VERIFY_WRITE,buf,len);
			if (error)
				return error;
			cli();
			while (!log_size) {
				if (current->signal & ~current->blocked) {
					sti();
					return -ERESTARTSYS;
				}
				interruptible_sleep_on(&log_wait);
			}
			i = 0;
			while (log_size && i < len) {
				c = *((char *) log_buf+log_start);
				log_start++;
				log_size--;
				log_start &= LOG_BUF_LEN-1;
				sti();
				put_fs_byte(c,buf);
				buf++;
				i++;
				cli();
			}
			sti();
			return i;
		case 4:		/* Read/clear last kernel messages */
			do_clear = 1; 
			/* FALL THRU */
		case 3:		/* Read last kernel messages */
			if (!buf || len < 0)
				return -EINVAL;
			if (!len)
				return 0;
			error = verify_area(VERIFY_WRITE,buf,len);
			if (error)
				return error;
			count = len;
			if (count > LOG_BUF_LEN)
				count = LOG_BUF_LEN;
			if (count > logged_chars)
				count = logged_chars;
			j = log_start + log_size - count;
			for (i = 0; i < count; i++) {
				c = *((char *) log_buf+(j++ & (LOG_BUF_LEN-1)));
				put_fs_byte(c, buf++);
			}
			if (do_clear)
				logged_chars = 0;
			return i;
		case 5:		/* Clear ring buffer */
			logged_chars = 0;
			return 0;
		case 6:		/* Disable logging to console */
			console_loglevel = 1; /* only panic messages shown */
			return 0;
		case 7:		/* Enable logging to console */
			console_loglevel = DEFAULT_CONSOLE_LOGLEVEL;
			return 0;
		case 8:
			if (len < 0 || len > 8)
				return -EINVAL;
			console_loglevel = len;
			return 0;
	}
	return -EINVAL;
}


asmlinkage int printk(const char *fmt, ...)
{
	va_list args;
	int i;
	char *msg, *p, *buf_end;
	static char msg_level = -1;
	long flags;

	save_flags(flags);
	cli();
	va_start(args, fmt);
	i = vsprintf(buf + 3, fmt, args); /* hopefully i < sizeof(buf)-4 */
	buf_end = buf + 3 + i;
	va_end(args);
	for (p = buf + 3; p < buf_end; p++) {
		msg = p;
		if (msg_level < 0) {
			if (
				p[0] != '<' ||
				p[1] < '0' || 
				p[1] > '7' ||
				p[2] != '>'
			) {
				p -= 3;
				p[0] = '<';
				p[1] = DEFAULT_MESSAGE_LOGLEVEL - 1 + '0';
				p[2] = '>';
			} else
				msg += 3;
			msg_level = p[1] - '0';
		}
		for (; p < buf_end; p++) {
			log_buf[(log_start+log_size) & (LOG_BUF_LEN-1)] = *p;
			if (log_size < LOG_BUF_LEN)
				log_size++;
			else
				log_start++;
			logged_chars++;
			if (*p == '\n')
				break;
		}
		if (msg_level < console_loglevel && console_print_proc) {
			char tmp = p[1];
			p[1] = '\0';
			(*console_print_proc)(msg);
			p[1] = tmp;
		}
		if (*p == '\n')
			msg_level = -1;
	}
	restore_flags(flags);
	wake_up_interruptible(&log_wait);
	return i;
}

/*
 * The console driver calls this routine during kernel initialization
 * to register the console printing procedure with printk() and to
 * print any messages that were printed by the kernel before the
 * console driver was initialized.
 */
void register_console(void (*proc)(const char *))
{
	int	i,j;
	int	p = log_start;
	char	buf[16];
	char	msg_level = -1;
	char	*q;

	console_print_proc = proc;

	for (i=0,j=0; i < log_size; i++) {
		buf[j++] = log_buf[p];
		p++; p &= LOG_BUF_LEN-1;
		if (buf[j-1] != '\n' && i < log_size - 1 && j < sizeof(buf)-1)
			continue;
		buf[j] = 0;
		q = buf;
		if (msg_level < 0) {
			msg_level = buf[1] - '0';
			q = buf + 3;
		}
		if (msg_level < console_loglevel)
			(*proc)(q);
		if (buf[j-1] == '\n')
			msg_level = -1;
		j = 0;
	}
}
