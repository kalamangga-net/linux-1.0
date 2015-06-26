/*
 *  linux/kernel/serial.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Extensively rewritten by Theodore Ts'o, 8/16/92 -- 9/14/92.  Now
 *  much more extensible to support other serial cards based on the
 *  16450/16550A UART's.  Added support for the AST FourPort and the
 *  Accent Async board.  
 *
 *  set_serial_info fixed to set the flags, custom divisor, and uart
 * 	type fields.  Fix suggested by Michael K. Johnson 12/12/92.
 *
 * This module exports the following rs232 io functions:
 *
 *	long rs_init(long);
 * 	int  rs_open(struct tty_struct * tty, struct file * filp)
 */

#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/tty.h>
#include <linux/serial.h>
#include <linux/interrupt.h>
#include <linux/config.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/fcntl.h>
#include <linux/ptrace.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>
#include <asm/bitops.h>

/*
 * Serial driver configuration section.  Here are the various options:
 *
 * CONFIG_AUTO_IRQ
 *		Enables automatic IRQ detection.  I've put in some
 * 		fixes to this which should make this work much more
 * 		cleanly than it used to in 0.98pl2-6.  It should be
 * 		much less vulnerable to false IRQs now.
 * 
 * CONFIG_AST_FOURPORT
 *		Enables support for the AST Fourport serial port.
 * 
 * CONFIG_ACCENT_ASYNC
 *		Enables support for the Accent Async 4 port serial
 * 		port.
 *
 * CONFIG_HUB6
 *		Enables support for the venerable Bell Technologies
 *		HUB6 card.
 */

#undef ISR_HACK

/*
 * rs_event		- Bitfield of serial lines that events pending
 * 				to be processed at the next clock tick.
 * IRQ_timeout		- How long the timeout should be for each IRQ
 * 				should be after the IRQ has been active.
 * IRQ_timer		- Array of timeout values for each interrupt IRQ.
 * 				This is based on jiffies; not offsets.
 * 
 * We assume here that int's are 32 bits, so an array of two gives us
 * 64 lines, which is the maximum we can support.
 */
static int rs_event[2];

static struct async_struct *IRQ_ports[16];
static int IRQ_active;
static unsigned long IRQ_timer[16];
static int IRQ_timeout[16];
static volatile int rs_irq_triggered;
static volatile int rs_triggered;
static int rs_wild_int_mask;

static void autoconfig(struct async_struct * info);
static void change_speed(unsigned int line);
	
/*
 * This assumes you have a 1.8432 MHz clock for your UART.
 *
 * It'd be nice if someone built a serial card with a 24.576 MHz
 * clock, since the 16550A is capable of handling a top speed of 1.5
 * megabits/second; but this requires the faster clock.
 */
#define BASE_BAUD ( 1843200 / 16 )

#ifdef CONFIG_AUTO_IRQ
#define AUTO_IRQ_FLAG ASYNC_AUTO_IRQ
#else
#define AUTO_IRQ_FLAG 0
#endif

/* Standard COM flags (except for COM4, because of the 8514 problem) */
#define STD_COM_FLAGS (ASYNC_BOOT_AUTOCONF | ASYNC_SKIP_TEST | AUTO_IRQ_FLAG)
#define STD_COM4_FLAGS (ASYNC_BOOT_AUTOCONF | AUTO_IRQ_FLAG)

#ifdef CONFIG_AST_FOURPORT
#define FOURPORT_FLAGS (ASYNC_BOOT_AUTOCONF | ASYNC_FOURPORT | AUTO_IRQ_FLAG)
#else
#define FOURPORT_FLAGS (ASYNC_FOURPORT | AUTO_IRQ_FLAG)
#endif

#ifdef CONFIG_ACCENT_ASYNC
#define ACCENT_FLAGS (ASYNC_BOOT_AUTOCONF | AUTO_IRQ_FLAG)
#else
#define ACCENT_FLAGS AUTO_IRQ_FLAG
#endif

#ifdef CONFIG_BOCA
#define BOCA_FLAGS (ASYNC_BOOT_AUTOCONF | AUTO_IRQ_FLAG)
#else
#define BOCA_FLAGS AUTO_IRQ_FLAG
#endif

#ifdef CONFIG_HUB6
#define HUB6_FLAGS (ASYNC_BOOT_AUTOCONF)
#else
#define HUB6_FLAGS 0
#endif
	
/*
 * The following define the access methods for the HUB6 card. All
 * access is through two ports for all 24 possible chips. The card is
 * selected through the high 2 bits, the port on that card with the
 * "middle" 3 bits, and the register on that port with the bottom
 * 3 bits.
 *
 * While the access port and interrupt is configurable, the default
 * port locations are 0x302 for the port control register, and 0x303
 * for the data read/write register. Normally, the interrupt is at irq3
 * but can be anything from 3 to 7 inclusive. Note tht using 3 will
 * require disabling com2.
 */

#define C_P(card,port) (((card)<<6|(port)<<3) + 1)

struct async_struct rs_table[] = {
	/* UART CLK   PORT IRQ     FLAGS        */
	{ BASE_BAUD, 0x3F8, 4, STD_COM_FLAGS },		/* ttyS0 */
	{ BASE_BAUD, 0x2F8, 3, STD_COM_FLAGS },		/* ttyS1 */
	{ BASE_BAUD, 0x3E8, 4, STD_COM_FLAGS },		/* ttyS2 */
	{ BASE_BAUD, 0x2E8, 3, STD_COM4_FLAGS },	/* ttyS3 */

	{ BASE_BAUD, 0x1A0, 9, FOURPORT_FLAGS }, 	/* ttyS4 */
	{ BASE_BAUD, 0x1A8, 9, FOURPORT_FLAGS },	/* ttyS5 */
	{ BASE_BAUD, 0x1B0, 9, FOURPORT_FLAGS },	/* ttyS6 */
	{ BASE_BAUD, 0x1B8, 9, FOURPORT_FLAGS },	/* ttyS7 */

	{ BASE_BAUD, 0x2A0, 5, FOURPORT_FLAGS },	/* ttyS8 */
	{ BASE_BAUD, 0x2A8, 5, FOURPORT_FLAGS },	/* ttyS9 */
	{ BASE_BAUD, 0x2B0, 5, FOURPORT_FLAGS },	/* ttyS10 */
	{ BASE_BAUD, 0x2B8, 5, FOURPORT_FLAGS },	/* ttyS11 */
	
	{ BASE_BAUD, 0x330, 4, ACCENT_FLAGS },		/* ttyS12 */
	{ BASE_BAUD, 0x338, 4, ACCENT_FLAGS },		/* ttyS13 */
	{ BASE_BAUD, 0x000, 0, 0 },	/* ttyS14 (spare; user configurable) */
	{ BASE_BAUD, 0x000, 0, 0 },	/* ttyS15 (spare; user configurable) */

	{ BASE_BAUD, 0x100, 12, BOCA_FLAGS },	/* ttyS16 */
	{ BASE_BAUD, 0x108, 12, BOCA_FLAGS },	/* ttyS17 */
	{ BASE_BAUD, 0x110, 12, BOCA_FLAGS },	/* ttyS18 */
	{ BASE_BAUD, 0x118, 12, BOCA_FLAGS },	/* ttyS19 */
	{ BASE_BAUD, 0x120, 12, BOCA_FLAGS },	/* ttyS20 */
	{ BASE_BAUD, 0x128, 12, BOCA_FLAGS },	/* ttyS21 */
	{ BASE_BAUD, 0x130, 12, BOCA_FLAGS },	/* ttyS22 */
	{ BASE_BAUD, 0x138, 12, BOCA_FLAGS },	/* ttyS23 */
	{ BASE_BAUD, 0x140, 12, BOCA_FLAGS },	/* ttyS24 */
	{ BASE_BAUD, 0x148, 12, BOCA_FLAGS },	/* ttyS25 */
	{ BASE_BAUD, 0x150, 12, BOCA_FLAGS },	/* ttyS26 */
	{ BASE_BAUD, 0x158, 12, BOCA_FLAGS },	/* ttyS27 */
	{ BASE_BAUD, 0x160, 12, BOCA_FLAGS },	/* ttyS28 */
	{ BASE_BAUD, 0x168, 12, BOCA_FLAGS },	/* ttyS29 */
	{ BASE_BAUD, 0x170, 12, BOCA_FLAGS },	/* ttyS30 */
	{ BASE_BAUD, 0x178, 12, BOCA_FLAGS },	/* ttyS31 */

/* You can have up to four HUB6's in the system, but I've only
 * included two cards here for a total of twelve ports.
 */
	{ BASE_BAUD, 0x302, 3, HUB6_FLAGS, C_P(0,0) },	/* ttyS32 */
	{ BASE_BAUD, 0x302, 3, HUB6_FLAGS, C_P(0,1) },	/* ttyS33 */
	{ BASE_BAUD, 0x302, 3, HUB6_FLAGS, C_P(0,2) },	/* ttyS34 */
	{ BASE_BAUD, 0x302, 3, HUB6_FLAGS, C_P(0,3) },	/* ttyS35 */
	{ BASE_BAUD, 0x302, 3, HUB6_FLAGS, C_P(0,4) },	/* ttyS36 */
	{ BASE_BAUD, 0x302, 3, HUB6_FLAGS, C_P(0,5) },	/* ttyS37 */
	{ BASE_BAUD, 0x302, 3, HUB6_FLAGS, C_P(1,0) },	/* ttyS32 */
	{ BASE_BAUD, 0x302, 3, HUB6_FLAGS, C_P(1,1) },	/* ttyS33 */
	{ BASE_BAUD, 0x302, 3, HUB6_FLAGS, C_P(1,2) },	/* ttyS34 */
	{ BASE_BAUD, 0x302, 3, HUB6_FLAGS, C_P(1,3) },	/* ttyS35 */
	{ BASE_BAUD, 0x302, 3, HUB6_FLAGS, C_P(1,4) },	/* ttyS36 */
	{ BASE_BAUD, 0x302, 3, HUB6_FLAGS, C_P(1,5) },	/* ttyS37 */
};

#define NR_PORTS	(sizeof(rs_table)/sizeof(struct async_struct))

/*
 * This is used to figure out the divsor speeds and the timeouts
 */
static int baud_table[] = {
	0, 50, 75, 110, 134, 150, 200, 300, 600, 1200, 1800, 2400, 4800,
	9600, 19200, 38400, 57600, 115200, 0 };

static void rs_throttle(struct tty_struct * tty, int status);

static inline unsigned int serial_in(struct async_struct *info, int offset)
{
    if (info->hub6) {
	outb(info->hub6 - 1 + offset, info->port);
	return inb(info->port+1);
    } else
	return inb(info->port + offset);
}

static inline unsigned int serial_inp(struct async_struct *info, int offset)
{
    if (info->hub6) {
	outb(info->hub6 - 1 + offset, info->port);
	return inb_p(info->port+1);
    } else
	return inb_p(info->port + offset);
}

static inline void serial_out(struct async_struct *info, int offset, int value)
{
    if (info->hub6) {
	outb(info->hub6 - 1 + offset, info->port);
	outb(value, info->port+1);
    } else
	outb(value, info->port+offset);
}

static inline void serial_outp(struct async_struct *info, int offset,
			       int value)
{
    if (info->hub6) {
	outb(info->hub6 - 1 + offset, info->port);
	outb_p(value, info->port+1);
    } else
	outb_p(value, info->port+offset);
}

/*
 * ------------------------------------------------------------
 * rs_stop() and rs_start()
 *
 * This routines are called before setting or resetting tty->stopped.
 * They enable or disable transmitter interrupts, as necessary.
 * ------------------------------------------------------------
 */
static void rs_stop(struct tty_struct *tty)
{
	struct async_struct *info;
	
	info = rs_table + DEV_TO_SL(tty->line);

	if (info->flags & ASYNC_CLOSING) {
		tty->stopped = 0;
		tty->hw_stopped = 0;
		return;
	}

	info->IER = UART_IER_MSI | UART_IER_RLSI | UART_IER_RDI;
#ifdef ISR_HACK
	serial_out(info, UART_IER, info->IER);
#endif
}

static void rs_start(struct tty_struct *tty)
{
	struct async_struct *info;
	
	info = rs_table + DEV_TO_SL(tty->line);
	
	info->IER = (UART_IER_MSI | UART_IER_RLSI |
		     UART_IER_THRI | UART_IER_RDI);
#ifdef ISR_HACK
	serial_out(info, UART_IER, info->IER);
#endif
}

/*
 * ----------------------------------------------------------------------
 *
 * Here starts the interrupt handling routines.  All of the following
 * subroutines are declared as inline and are folded into
 * rs_interrupt().  They were separated out for readability's sake.
 *
 * Note: rs_interrupt() is a "fast" interrupt, which means that it
 * runs with interrupts turned off.  People who may want to modify
 * rs_interrupt() should try to keep the interrupt handler as fast as
 * possible.  After you are done making modifications, it is not a bad
 * idea to do:
 * 
 * gcc -S -DKERNEL -Wall -Wstrict-prototypes -O6 -fomit-frame-pointer serial.c
 *
 * and look at the resulting assemble code in serial.s.
 *
 * 				- Ted Ts'o (tytso@mit.edu), 7-Mar-93
 * -----------------------------------------------------------------------
 */

/*
 * This is the serial driver's interrupt routine while we are probing
 * for submarines.
 */
static void rs_probe(int irq)
{
	rs_irq_triggered = irq;
	rs_triggered |= 1 << irq;
	return;
}

/*
 * This routine is used by the interrupt handler to schedule
 * processing in the software interrupt portion of the driver.
 */
static inline void rs_sched_event(struct async_struct *info,
				  int event)
{
	info->event |= 1 << event;
	set_bit(info->line, rs_event);
	mark_bh(SERIAL_BH);
}

static inline void receive_chars(struct async_struct *info,
				 int *status)
{
	struct tty_queue * queue;
	int head, tail, ch;

/*
 * Just like the LEFT(x) macro, except it uses the loal tail
 * and head variables.
 */
#define VLEFT ((tail-head-1)&(TTY_BUF_SIZE-1))

	queue = &info->tty->read_q;
	head = queue->head;
	tail = queue->tail;
	do {
		ch = serial_inp(info, UART_RX);
		/*
		 * There must be at least 2 characters
		 * free in the queue; otherwise we punt.
		 */
		if (VLEFT < 2)
			break;
		if (*status & info->read_status_mask) {
			set_bit(head, &info->tty->readq_flags);
			if (*status & (UART_LSR_BI)) {
				queue->buf[head++]= TTY_BREAK;
				rs_sched_event(info, RS_EVENT_BREAK);
			} else if (*status & UART_LSR_PE)
				queue->buf[head++]= TTY_PARITY;
			else if (*status & UART_LSR_FE)
				queue->buf[head++]= TTY_FRAME;
			else if (*status & UART_LSR_OE)
				queue->buf[head++]= TTY_OVERRUN;
			head &= TTY_BUF_SIZE-1;
		}
		queue->buf[head++] = ch;
		head &= TTY_BUF_SIZE-1;
	} while ((*status = serial_inp(info, UART_LSR)) & UART_LSR_DR);
	queue->head = head;
	if ((VLEFT < RQ_THRESHOLD_LW) && !set_bit(TTY_RQ_THROTTLED,
						  &info->tty->flags)) 
		rs_throttle(info->tty, TTY_THROTTLE_RQ_FULL);
	rs_sched_event(info, RS_EVENT_READ_PROCESS);
#ifdef SERIAL_DEBUG_INTR
	printk("DR...");
#endif
}

static inline void transmit_chars(struct async_struct *info, int *done_work)
{
	struct tty_queue * queue;
	int head, tail, count;
	
	queue = &info->tty->write_q;
	head = queue->head;
	tail = queue->tail;
	if (head==tail && !info->x_char) {
		info->IER = UART_IER_MSI | UART_IER_RLSI | UART_IER_RDI;
#ifdef ISR_HACK
		serial_out(info, UART_IER, info->IER);
#endif
		return;
	}
	count = info->xmit_fifo_size;
	if (info->x_char) {
		serial_outp(info, UART_TX, info->x_char);
		info->x_char = 0;
		count--;
	}
	while (count-- && (tail != head)) {
		serial_outp(info, UART_TX, queue->buf[tail++]);
		tail &= TTY_BUF_SIZE-1;
	}
	queue->tail = tail;
	if (VLEFT > WAKEUP_CHARS) {
		rs_sched_event(info, RS_EVENT_WRITE_WAKEUP);
		if (info->tty->write_data_cnt) {
			set_bit(info->tty->line, &tty_check_write);
			mark_bh(TTY_BH);
		}
	}
#ifdef SERIAL_DEBUG_INTR
	printk("THRE...");
#endif
	(*done_work)++;
}

static inline int check_modem_status(struct async_struct *info)
{
	int	status;
	
	status = serial_in(info, UART_MSR);
		
	if ((status & UART_MSR_DDCD) && !C_CLOCAL(info->tty)) {
#if (defined(SERIAL_DEBUG_OPEN) || defined(SERIAL_DEBUG_INTR))
		printk("ttys%d CD now %s...", info->line,
		       (status & UART_MSR_DCD) ? "on" : "off");
#endif		
		if (status & UART_MSR_DCD)
			rs_sched_event(info, RS_EVENT_OPEN_WAKEUP);
		else if (!((info->flags & ASYNC_CALLOUT_ACTIVE) &&
			   (info->flags & ASYNC_CALLOUT_NOHUP))) {
#ifdef SERIAL_DEBUG_OPEN
			printk("scheduling hangup...");
#endif
			rs_sched_event(info, RS_EVENT_HANGUP);
		}
	}
	if (C_CRTSCTS(info->tty) && !(info->flags & ASYNC_CLOSING)) {
		if (info->tty->hw_stopped) {
			if (status & UART_MSR_CTS) {
#ifdef SERIAL_DEBUG_INTR
				printk("CTS tx start...");
#endif
				info->tty->hw_stopped = 0;
				rs_start(info->tty);
				return 1;
			}
		} else {
			if (!(status & UART_MSR_CTS)) {
#ifdef SERIAL_DEBUG_INTR
				printk("CTS tx stop...");
#endif
				info->tty->hw_stopped = 1;
				rs_stop(info->tty);
			}
		}
	}
	return 0;
}

static inline void figure_RS_timer(void)
{
	int	timeout = jiffies + 60*HZ; /* 60 seconds; really big :-) */
	int	i, mask;
	
	if (!IRQ_active)
		return;
	for (i=0, mask = 1; mask <= IRQ_active; i++, mask <<= 1) {
		if (!(mask & IRQ_active))
			continue;
		if (IRQ_timer[i] < timeout)
			timeout = IRQ_timer[i];
	}
	timer_table[RS_TIMER].expires = timeout;
	timer_active |= 1 << RS_TIMER;
}


/*
 * This is the serial driver's generic interrupt routine
 */
static void rs_interrupt(int irq)
{
	int status;
	struct async_struct * info;
	int done, done_work, pass_number, recheck_count;

	rs_irq_triggered = irq;
	rs_triggered |= 1 << irq;
	
	info = IRQ_ports[irq];
	done = 1;
	done_work = 0;
	pass_number = 0;
	while (info) {
		if (info->tty &&
		    info->tty->termios &&
		    (!pass_number ||
		     !(serial_inp(info, UART_IIR) & UART_IIR_NO_INT))) {
			done = 0;
			status = serial_inp(info, UART_LSR);
			if (status & UART_LSR_DR) {
				receive_chars(info, &status);
				done_work++;
			}
		recheck_count = 0;
		recheck_write:
			if (status & UART_LSR_THRE) {
				wake_up_interruptible(&info->xmit_wait);
				if (!info->tty->stopped &&
				    !info->tty->hw_stopped)
					transmit_chars(info, &done_work);
			}
			if (check_modem_status(info) &&
			    (recheck_count++ <= 64))
				goto recheck_write;
#ifdef SERIAL_DEBUG_INTR
			if (recheck_count > 16)
				printk("recheck_count = %d\n", recheck_count);
#endif
		}
#ifdef ISR_HACK
		serial_outp(info, UART_IER, 0);
		serial_out(info, UART_IER, info->IER);
#endif
		
		info = info->next_port;
		if (!info && !done) {
			info = IRQ_ports[irq];
			done = 1;
			if (pass_number++ > 64)
				break; 		/* Prevent infinite loops */
		}
	}
	if ((info = IRQ_ports[irq]) != NULL) {
#ifdef 0
		do {
			serial_outp(info, UART_IER, 0);
			serial_out(info, UART_IER, info->IER);
			info = info->next_port;
		} while (info);
#endif			
		if (irq && !done_work)
			IRQ_timer[irq] = jiffies + 1500;
		else
			IRQ_timer[irq] = jiffies + IRQ_timeout[irq];
		IRQ_active |= 1 << irq;
	}
	figure_RS_timer();
}

/*
 * -------------------------------------------------------------------
 * Here ends the serial interrupt routines.
 * -------------------------------------------------------------------
 */

/*
 * This routine is called when we receive a break on a serial line.
 * It is executed out of the software interrupt routine.
 */
static inline void handle_rs_break(struct async_struct *info)
{
	if (info->flags & ASYNC_SAK)
		do_SAK(info->tty);
		
	if (!I_IGNBRK(info->tty) && I_BRKINT(info->tty)) {
		flush_input(info->tty);
		flush_output(info->tty);
		if (info->tty->pgrp > 0)
			kill_pg(info->tty->pgrp, SIGINT,1);
	}
}

/*
 * This routine is used to handle the "bottom half" processing for the
 * serial driver, known also the "software interrupt" processing.
 * This processing is done at the kernel interrupt level, after the
 * rs_interrupt() has returned, BUT WITH INTERRUPTS TURNED ON.  This
 * is where time-consuming activities which can not be done in the
 * interrupt driver proper are done; the interrupt driver schedules
 * them using rs_sched_event(), and they get done here.
 */
static void do_softint(void *unused)
{
	int			i;
	struct async_struct	*info;
	
	for (i = 0, info = rs_table; i < NR_PORTS; i++,info++) {
		if (clear_bit(i, rs_event)) {
			if (!info->tty)	
				continue;
			if (clear_bit(RS_EVENT_READ_PROCESS, &info->event)) {
				TTY_READ_FLUSH(info->tty);
			}
			if (clear_bit(RS_EVENT_WRITE_WAKEUP, &info->event)) {
				wake_up_interruptible(&info->tty->write_q.proc_list);
			}
			if (clear_bit(RS_EVENT_HANGUP, &info->event)) {
				tty_hangup(info->tty);
				wake_up_interruptible(&info->open_wait);
				info->flags &= ~(ASYNC_NORMAL_ACTIVE|
						 ASYNC_CALLOUT_ACTIVE);
			}
			if (clear_bit(RS_EVENT_BREAK, &info->event))
				handle_rs_break(info);
			if (clear_bit(RS_EVENT_OPEN_WAKEUP, &info->event)) {
				wake_up_interruptible(&info->open_wait);
			}
		}
	}
}

/*
 * This subroutine is called when the RS_TIMER goes off.  It is used
 * by the serial driver to run the rs_interrupt routine at certain
 * intervals, either because a serial interrupt might have been lost,
 * or because (in the case of IRQ=0) the serial port does not have an
 * interrupt, and is being checked only via the timer interrupts.
 */
static void rs_timer(void)
{
	int	i, mask;
	int	timeout = 0;

	for (i = 0, mask = 1; mask <= IRQ_active; i++, mask <<= 1) {
		if ((mask & IRQ_active) && (IRQ_timer[i] <= jiffies)) {
			IRQ_active &= ~mask;
			cli();
#ifdef SERIAL_DEBUG_TIMER
			printk("rs_timer: rs_interrupt(%d)...", i);
#endif
			rs_interrupt(i);
			sti();
		}
		if (mask & IRQ_active) {
			if (!timeout || (IRQ_timer[i] < timeout))
				timeout = IRQ_timer[i];
		}
	}
	if (timeout) {
		timer_table[RS_TIMER].expires = timeout;
		timer_active |= 1 << RS_TIMER;
	}
}

/*
 * ---------------------------------------------------------------
 * Low level utility subroutines for the serial driver:  routines to
 * figure out the appropriate timeout for an interrupt chain, routines
 * to initialize and startup a serial port, and routines to shutdown a
 * serial port.  Useful stuff like that.
 * ---------------------------------------------------------------
 */

/*
 * Grab all interrupts in preparation for doing an automatic irq
 * detection.  dontgrab is a mask of irq's _not_ to grab.  Returns a
 * mask of irq's which were grabbed and should therefore be freed
 * using free_all_interrupts().
 */
static int grab_all_interrupts(int dontgrab)
{
	int 			irq_lines = 0;
	int			i, mask;
	struct sigaction 	sa;
	
	sa.sa_handler = rs_probe;
	sa.sa_flags = (SA_INTERRUPT);
	sa.sa_mask = 0;
	sa.sa_restorer = NULL;
	
	for (i = 0, mask = 1; i < 16; i++, mask <<= 1) {
		if (!(mask & dontgrab) && !irqaction(i, &sa)) {
			irq_lines |= mask;
		}
	}
	return irq_lines;
}

/*
 * Release all interrupts grabbed by grab_all_interrupts
 */
static void free_all_interrupts(int irq_lines)
{
	int	i;
	
	for (i = 0; i < 16; i++) {
		if (irq_lines & (1 << i))
			free_irq(i);
	}
}

/*
 * This routine figures out the correct timeout for a particular IRQ.
 * It uses the smallest timeout of all of the serial ports in a
 * particular interrupt chain.
 */
static void figure_IRQ_timeout(int irq)
{
	struct	async_struct	*info;
	int	timeout = 6000;	/* 60 seconds === a long time :-) */

	info = IRQ_ports[irq];
	if (!info) {
		IRQ_timeout[irq] = 6000;
		return;
	}
	while (info) {
		if (info->timeout < timeout)
			timeout = info->timeout;
		info = info->next_port;
	}
	if (!irq)
		timeout = timeout / 2;
	IRQ_timeout[irq] = timeout ? timeout : 1;
}

static int startup(struct async_struct * info, int get_irq)
{
	unsigned short ICP;
	unsigned long flags;
	struct sigaction	sa;
	int			retval;

	if (info->flags & ASYNC_INITIALIZED)
		return 0;

	if (!info->port || !info->type) {
		if (info->tty)
			set_bit(TTY_IO_ERROR, &info->tty->flags);
		return 0;
	}

	save_flags(flags); cli();

#ifdef SERIAL_DEBUG_OPEN
	printk("starting up ttys%d (irq %d)...", info->line, info->irq);
#endif

	/*
	 * Allocate the IRQ if necessary
	 */
	if (get_irq && info->irq && !IRQ_ports[info->irq]) {
		sa.sa_handler = rs_interrupt;
		sa.sa_flags = (SA_INTERRUPT);
		sa.sa_mask = 0;
		sa.sa_restorer = NULL;
		retval = irqaction(info->irq,&sa);
		if (retval) {
			restore_flags(flags);
			return retval;
		}
	}

	/*
	 * Clear the FIFO buffers and disable them
	 * (they will be reenabled in change_speed())
	 */
	if (info->type == PORT_16550A) {
		serial_outp(info, UART_FCR, (UART_FCR_CLEAR_RCVR |
					     UART_FCR_CLEAR_XMIT));
		info->xmit_fifo_size = 16;
	} else
		info->xmit_fifo_size = 1;

	/*
	 * Clear the interrupt registers.
	 */
	(void)serial_inp(info, UART_LSR);
	(void)serial_inp(info, UART_RX);
	(void)serial_inp(info, UART_IIR);
	(void)serial_inp(info, UART_MSR);

	/*
	 * Now, initialize the UART 
	 */
	serial_outp(info, UART_LCR, UART_LCR_WLEN8);	/* reset DLAB */
	if (info->flags & ASYNC_FOURPORT) 
		serial_outp(info, UART_MCR, UART_MCR_DTR | UART_MCR_RTS);
	else
		serial_outp(info, UART_MCR,
			    UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);
	
	/*
	 * Finally, enable interrupts
	 */
#ifdef ISR_HACK
	info->IER = UART_IER_MSI | UART_IER_RLSI | UART_IER_RDI;
	serial_outp(info, UART_IER, info->IER);	/* enable interrupts */
#else
	info->IER = (UART_IER_MSI | UART_IER_RLSI |
		     UART_IER_THRI | UART_IER_RDI);
	serial_outp(info, UART_IER, info->IER);	/* enable all intrs */
#endif
	if (info->flags & ASYNC_FOURPORT) {
		/* Enable interrupts on the AST Fourport board */
		ICP = (info->port & 0xFE0) | 0x01F;
		outb_p(0x80, ICP);
		(void) inb_p(ICP);
	}

	/*
	 * And clear the interrupt registers again for luck.
	 */
	(void)serial_inp(info, UART_LSR);
	(void)serial_inp(info, UART_RX);
	(void)serial_inp(info, UART_IIR);
	(void)serial_inp(info, UART_MSR);

	if (info->tty)
		clear_bit(TTY_IO_ERROR, &info->tty->flags);
	/*
	 * Set up parity check flag
	 */
	if (info->tty && info->tty->termios && I_INPCK(info->tty))
		info->read_status_mask = (UART_LSR_OE | UART_LSR_BI |
					  UART_LSR_FE | UART_LSR_PE);
	else
		info->read_status_mask = (UART_LSR_OE | UART_LSR_BI |
					  UART_LSR_FE);

	/*
	 * Insert serial port into IRQ chain.
	 */
	info->prev_port = 0;
	info->next_port = IRQ_ports[info->irq];
	if (info->next_port)
		info->next_port->prev_port = info;
	IRQ_ports[info->irq] = info;
	figure_IRQ_timeout(info->irq);

	/*
	 * Set up serial timers...
	 */
	IRQ_active |= 1 << info->irq;
	figure_RS_timer();

	/*
	 * and set the speed of the serial port
	 */
	change_speed(info->line);

	info->flags |= ASYNC_INITIALIZED;
	restore_flags(flags);
	return 0;
}

/*
 * This routine will shutdown a serial port; interrupts are disabled, and
 * DTR is dropped if the hangup on close termio flag is on.
 */
static void shutdown(struct async_struct * info, int do_free_irq)
{
	unsigned long flags;

	if (!(info->flags & ASYNC_INITIALIZED))
		return;

#ifdef SERIAL_DEBUG_OPEN
	printk("Shutting down serial port %d (irq %d)....", info->line,
	       info->irq);
#endif
	
	save_flags(flags); cli(); /* Disable interrupts */
	
	/*
	 * First unlink the serial port from the IRQ chain...
	 */
	if (info->next_port)
		info->next_port->prev_port = info->prev_port;
	if (info->prev_port)
		info->prev_port->next_port = info->next_port;
	else
		IRQ_ports[info->irq] = info->next_port;
	figure_IRQ_timeout(info->irq);
	
	/*
	 * Free the IRQ, if necessary
	 */
	if (do_free_irq && info->irq && !IRQ_ports[info->irq])
		free_irq(info->irq);
	
	info->IER = 0;
	serial_outp(info, UART_IER, 0x00);	/* disable all intrs */
	if (info->flags & ASYNC_FOURPORT) {
		/* reset interrupts on the AST Fourport board */
		(void) inb((info->port & 0xFE0) | 0x01F);
	}
	if (info->tty && !(info->tty->termios->c_cflag & HUPCL))
		serial_outp(info, UART_MCR, UART_MCR_DTR);
	else
		/* reset DTR,RTS,OUT_2 */		
		serial_outp(info, UART_MCR, 0x00);

	/* disable FIFO's */	
	serial_outp(info, UART_FCR, (UART_FCR_CLEAR_RCVR |
				     UART_FCR_CLEAR_XMIT));
	(void)serial_in(info, UART_RX);    /* read data port to reset things */
	
	if (info->tty)
		set_bit(TTY_IO_ERROR, &info->tty->flags);
	
	info->flags &= ~ASYNC_INITIALIZED;
	restore_flags(flags);
}

/*
 * This routine is called to set the UART divisor registers to match
 * the specified baud rate for a serial port.
 */
static void change_speed(unsigned int line)
{
	struct async_struct * info;
	unsigned short port;
	int	quot = 0;
	unsigned cflag,cval,mcr,fcr;
	int	i;

	if (line >= NR_PORTS)
		return;
	info = rs_table + line;
	if (!info->tty || !info->tty->termios)
		return;
	cflag = info->tty->termios->c_cflag;
	if (!(port = info->port))
		return;
	i = cflag & CBAUD;
	if (i == 15) {
		if ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_HI)
			i += 1;
		if ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_VHI)
			i += 2;
		if ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_CUST)
			quot = info->custom_divisor;
	}
	if (quot) {
		info->timeout = ((info->xmit_fifo_size*HZ*15*quot) /
				 info->baud_base) + 2;
	} else if (baud_table[i] == 134) {
		quot = (2*info->baud_base / 269);
		info->timeout = (info->xmit_fifo_size*HZ*30/269) + 2;
	} else if (baud_table[i]) {
		quot = info->baud_base / baud_table[i];
		info->timeout = (info->xmit_fifo_size*HZ*15/baud_table[i]) + 2;
	} else {
		quot = 0;
		info->timeout = 0;
	}
	cli();
	mcr = serial_in(info, UART_MCR);
	if (quot) {
		serial_out(info, UART_MCR, mcr | UART_MCR_DTR);
	} else {
		serial_out(info, UART_MCR, mcr & ~UART_MCR_DTR);
		sti();
		return;
	}
	sti();
	/* byte size and parity */
	cval = cflag & (CSIZE | CSTOPB);
	cval >>= 4;
	if (cflag & PARENB)
		cval |= UART_LCR_PARITY;
	if (!(cflag & PARODD))
		cval |= UART_LCR_EPAR;
	if (info->type == PORT_16550A) {
		if ((info->baud_base / quot) < 2400)
			fcr = UART_FCR_ENABLE_FIFO | UART_FCR_TRIGGER_1;
		else
			fcr = UART_FCR_ENABLE_FIFO | UART_FCR_TRIGGER_8;
	} else
		fcr = 0;
	
	cli();
	serial_outp(info, UART_LCR, cval | UART_LCR_DLAB);	/* set DLAB */
	serial_outp(info, UART_DLL, quot & 0xff);	/* LS of divisor */
	serial_outp(info, UART_DLM, quot >> 8);		/* MS of divisor */
	serial_outp(info, UART_LCR, cval);		/* reset DLAB */
	serial_outp(info, UART_FCR, fcr); 	/* set fcr */
	sti();
}

/*
 * ------------------------------------------------------------
 * rs_write() and friends
 * ------------------------------------------------------------
 */

/*
 * This routine is used by rs_write to restart transmitter interrupts,
 * which are disabled after we have a transmitter interrupt which went
 * unacknowledged because we had run out of data to transmit.
 * 
 * Note: this subroutine must be called with the interrupts *off*
 */
static inline void restart_port(struct async_struct *info)
{
	struct tty_queue * queue;
	int head, tail, count;
	
	if (!info)
		return;

	if (serial_inp(info, UART_LSR) & UART_LSR_THRE) {
		if (info->x_char) {
			serial_outp(info, UART_TX, info->x_char);
			info->x_char = 0;
		} else {
			queue = &info->tty->write_q;
			head = queue->head;
			tail = queue->tail;
			count = info->xmit_fifo_size;
			while (count--) {
				if (tail == head)
					break;
				serial_outp(info, UART_TX, queue->buf[tail++]);
				tail &= TTY_BUF_SIZE-1;
			}
			queue->tail = tail;
		}
	}
}	

/*
 * This routine gets called when tty_write has put something into
 * the write_queue.  
 */
void rs_write(struct tty_struct * tty)
{
	struct async_struct *info;

	if (!tty || tty->stopped || tty->hw_stopped)
		return;
	info = rs_table + DEV_TO_SL(tty->line);
	cli();
	if (!info || !info->tty || !(info->flags & ASYNC_INITIALIZED)) {
		sti();
		return;
	}
	restart_port(info);
	info->IER = (UART_IER_MSI | UART_IER_RLSI |
		     UART_IER_THRI | UART_IER_RDI);
#ifdef ISR_HACK
	serial_out(info, UART_IER, info->IER);
#endif
	sti();
}

/*
 * ------------------------------------------------------------
 * rs_throttle()
 * 
 * This routine is called by the upper-layer tty layer to signal that
 * incoming characters should be throttled (and that the throttle
 * should be released).
 * ------------------------------------------------------------
 */
static void rs_throttle(struct tty_struct * tty, int status)
{
	struct async_struct *info;
	unsigned char mcr;
	unsigned long flags;

	save_flags(flags); cli();
#if SERIAL_DEBUG_THROTTLE
	printk("throttle tty%d: %d (%d, %d)....\n", DEV_TO_SL(tty->line),
	       status, LEFT(&tty->read_q), LEFT(&tty->secondary));
#endif
	switch (status) {
	case TTY_THROTTLE_RQ_FULL:
		info = rs_table + DEV_TO_SL(tty->line);
		if (I_IXOFF(tty)) {
			info->x_char = STOP_CHAR(tty);
		} else {
			mcr = serial_inp(info, UART_MCR);
			mcr &= ~UART_MCR_RTS;
			serial_out(info, UART_MCR, mcr);
		}
		break;
	case TTY_THROTTLE_RQ_AVAIL:
		info = rs_table + DEV_TO_SL(tty->line);
		if (I_IXOFF(tty)) {
			if (info->x_char)
				info->x_char = 0;
			else
				info->x_char = START_CHAR(tty);
		} else {
			mcr = serial_in(info, UART_MCR);
			mcr |= UART_MCR_RTS;
			serial_out(info, UART_MCR, mcr);
		}
		break;
	}
	restore_flags(flags);
}

/*
 * ------------------------------------------------------------
 * rs_ioctl() and friends
 * ------------------------------------------------------------
 */

static int get_serial_info(struct async_struct * info,
			   struct serial_struct * retinfo)
{
	struct serial_struct tmp;
  
	if (!retinfo)
		return -EFAULT;
	memset(&tmp, 0, sizeof(tmp));
	tmp.type = info->type;
	tmp.line = info->line;
	tmp.port = info->port;
	tmp.irq = info->irq;
	tmp.flags = info->flags;
	tmp.baud_base = info->baud_base;
	tmp.close_delay = info->close_delay;
	tmp.custom_divisor = info->custom_divisor;
	tmp.hub6 = info->hub6;
	memcpy_tofs(retinfo,&tmp,sizeof(*retinfo));
	return 0;
}

static int set_serial_info(struct async_struct * info,
			   struct serial_struct * new_info)
{
	struct serial_struct new_serial;
	struct async_struct old_info;
	unsigned int		i,change_irq,change_port;
	int 			retval;
	struct 			sigaction sa;

	if (!new_info)
		return -EFAULT;
	memcpy_fromfs(&new_serial,new_info,sizeof(new_serial));
	old_info = *info;

	change_irq = new_serial.irq != info->irq;
	change_port = (new_serial.port != info->port) || (new_serial.hub6 != info->hub6);

	if (!suser()) {
		if (change_irq || change_port ||
		    (new_serial.baud_base != info->baud_base) ||
		    (new_serial.type != info->type) ||
		    (new_serial.close_delay != info->close_delay) ||
		    ((new_serial.flags & ~ASYNC_USR_MASK) !=
		     (info->flags & ~ASYNC_USR_MASK)))
			return -EPERM;
		info->flags = ((info->flags & ~ASYNC_USR_MASK) |
			       (new_serial.flags & ASYNC_USR_MASK));
		info->custom_divisor = new_serial.custom_divisor;
		goto check_and_exit;
	}

	if (new_serial.irq == 2)
		new_serial.irq = 9;

	if ((new_serial.irq > 15) || (new_serial.port > 0xffff) ||
	    (new_serial.type < PORT_UNKNOWN) || (new_serial.type > PORT_MAX)) {
		return -EINVAL;
	}

	/* Make sure address is not already in use */
	for (i = 0 ; i < NR_PORTS; i++)
		if ((info != &rs_table[i]) &&
		    (rs_table[i].port == new_serial.port) && rs_table[i].type)
			return -EADDRINUSE;

	/*
	 * If necessary, first we try to grab the new IRQ for serial
	 * interrupts.  (We have to do this early, since we may get an
	 * error trying to do this.)
	 */
	if (new_serial.port && new_serial.type && new_serial.irq &&
	    (change_irq || !(info->flags & ASYNC_INITIALIZED))) {
		if (!IRQ_ports[new_serial.irq]) {
			sa.sa_handler = rs_interrupt;
			sa.sa_flags = (SA_INTERRUPT);
			sa.sa_mask = 0;
			sa.sa_restorer = NULL;
			retval = irqaction(new_serial.irq,&sa);
			if (retval)
				return retval;
		}
	}

	if ((change_port || change_irq) && (info->count > 1))
		return -EBUSY;

	/*
	 * OK, past this point, all the error checking has been done.
	 * At this point, we start making changes.....
	 */

	info->baud_base = new_serial.baud_base;
	info->flags = ((info->flags & ~ASYNC_FLAGS) |
			(new_serial.flags & ASYNC_FLAGS));
	info->custom_divisor = new_serial.custom_divisor;
	info->type = new_serial.type;
	info->close_delay = new_serial.close_delay;

	if (change_port || change_irq) {
		/*
		 * We need to shutdown the serial port at the old
		 * port/irq combination.
		 */
		shutdown(info, change_irq);
		info->irq = new_serial.irq;
		info->port = new_serial.port;
		info->hub6 = new_serial.hub6;
	}
	
check_and_exit:
	if (!info->port || !info->type)
		return 0;
	if (info->flags & ASYNC_INITIALIZED) {
		if (((old_info.flags & ASYNC_SPD_MASK) !=
		     (info->flags & ASYNC_SPD_MASK)) ||
		    (old_info.custom_divisor != info->custom_divisor))
			change_speed(info->line);
	} else
		(void) startup(info, 0);
	return 0;
}

static int get_modem_info(struct async_struct * info, unsigned int *value)
{
	unsigned char control, status;
	unsigned int result;

	cli();
	control = serial_in(info, UART_MCR);
	status = serial_in(info, UART_MSR);
	sti();
	result =  ((control & UART_MCR_RTS) ? TIOCM_RTS : 0)
		| ((control & UART_MCR_DTR) ? TIOCM_DTR : 0)
		| ((status  & UART_MSR_DCD) ? TIOCM_CAR : 0)
		| ((status  & UART_MSR_RI) ? TIOCM_RNG : 0)
		| ((status  & UART_MSR_DSR) ? TIOCM_DSR : 0)
		| ((status  & UART_MSR_CTS) ? TIOCM_CTS : 0);
	put_fs_long(result,(unsigned long *) value);
	return 0;
}

static int set_modem_info(struct async_struct * info, unsigned int cmd,
			  unsigned int *value)
{
	unsigned char control;
	unsigned int arg = get_fs_long((unsigned long *) value);
	
	cli();
	control = serial_in(info, UART_MCR);
	sti();

	switch (cmd) {
		case TIOCMBIS:
			if (arg & TIOCM_RTS)
				control |= UART_MCR_RTS;
			if (arg & TIOCM_DTR)
				control |= UART_MCR_DTR;
			break;
		case TIOCMBIC:
			if (arg & TIOCM_RTS)
				control &= ~UART_MCR_RTS;
			if (arg & TIOCM_DTR)
				control &= ~UART_MCR_DTR;
			break;
		case TIOCMSET:
			control = (control & ~(UART_MCR_RTS | UART_MCR_DTR))
				| ((arg & TIOCM_RTS) ? UART_MCR_RTS : 0)
				| ((arg & TIOCM_DTR) ? UART_MCR_DTR : 0);
			break;
		default:
			return -EINVAL;
	}
	cli();
	serial_out(info, UART_MCR, control);
	sti();
	return 0;
}

static int do_autoconfig(struct async_struct * info)
{
	int			retval;
	
	if (!suser())
		return -EPERM;
	
	if (info->count > 1)
		return -EBUSY;
	
	shutdown(info, 1);

	cli();
	autoconfig(info);
	sti();

	retval = startup(info, 1);
	if (retval)
		return retval;
	return 0;
}


/*
 * This routine sends a break character out the serial port.
 */
static void send_break(	struct async_struct * info, int duration)
{
	if (!info->port)
		return;
	current->state = TASK_INTERRUPTIBLE;
	current->timeout = jiffies + duration;
	cli();
	serial_out(info, UART_LCR, serial_inp(info, UART_LCR) | UART_LCR_SBC);
	schedule();
	serial_out(info, UART_LCR, serial_inp(info, UART_LCR) & ~UART_LCR_SBC);
	sti();
}

/*
 * This routine returns a bitfield of "wild interrupts".  Basically,
 * any unclaimed interrupts which is flapping around.
 */
static int check_wild_interrupts(int doprint)
{
	int	i, mask;
	int	wild_interrupts = 0;
	int	irq_lines;
	unsigned long timeout;
	unsigned long flags;
	
	/* Turn on interrupts (they may be off) */
	save_flags(flags); sti();

	irq_lines = grab_all_interrupts(0);
	
	/*
	 * Delay for 0.1 seconds -- we use a busy loop since this may 
	 * occur during the bootup sequence
	 */
	timeout = jiffies+10;
	while (timeout >= jiffies)
		;
	
	rs_triggered = 0;	/* Reset after letting things settle */

	timeout = jiffies+10;
	while (timeout >= jiffies)
		;
	
	for (i = 0, mask = 1; i < 16; i++, mask <<= 1) {
		if ((rs_triggered & (1 << i)) &&
		    (irq_lines & (1 << i))) {
			wild_interrupts |= mask;
			if (doprint)
				printk("Wild interrupt?  (IRQ %d)\n", i);
		}
	}
	free_all_interrupts(irq_lines);
	restore_flags(flags);
	return wild_interrupts;
}

static int rs_ioctl(struct tty_struct *tty, struct file * file,
		    unsigned int cmd, unsigned long arg)
{
	int error, line;
	struct async_struct * info;

	line = DEV_TO_SL(tty->line);
	if (line < 0 || line >= NR_PORTS)
		return -ENODEV;
	info = rs_table + line;
	
	switch (cmd) {
		case TCSBRK:	/* SVID version: non-zero arg --> no break */
			if (!arg)
				send_break(info, HZ/4);	/* 1/4 second */
			return 0;
		case TCSBRKP:	/* support for POSIX tcsendbreak() */
			send_break(info, arg ? arg*(HZ/10) : HZ/4);
			return 0;
		case TIOCGSOFTCAR:
			error = verify_area(VERIFY_WRITE, (void *) arg,sizeof(long));
			if (error)
				return error;
			put_fs_long(C_CLOCAL(tty) ? 1 : 0,
				    (unsigned long *) arg);
			return 0;
		case TIOCSSOFTCAR:
			arg = get_fs_long((unsigned long *) arg);
			tty->termios->c_cflag =
				((tty->termios->c_cflag & ~CLOCAL) |
				 (arg ? CLOCAL : 0));
			return 0;
		case TIOCMGET:
			error = verify_area(VERIFY_WRITE, (void *) arg,
				sizeof(unsigned int));
			if (error)
				return error;
			return get_modem_info(info, (unsigned int *) arg);
		case TIOCMBIS:
		case TIOCMBIC:
		case TIOCMSET:
			return set_modem_info(info, cmd, (unsigned int *) arg);
		case TIOCGSERIAL:
			error = verify_area(VERIFY_WRITE, (void *) arg,
						sizeof(struct serial_struct));
			if (error)
				return error;
			return get_serial_info(info,
					       (struct serial_struct *) arg);
		case TIOCSSERIAL:
			return set_serial_info(info,
					       (struct serial_struct *) arg);
		case TIOCSERCONFIG:
			return do_autoconfig(info);

		case TIOCSERGWILD:
			error = verify_area(VERIFY_WRITE, (void *) arg,
					    sizeof(int));
			if (error)
				return error;
			put_fs_long(rs_wild_int_mask, (unsigned long *) arg);
			return 0;

		case TIOCSERSWILD:
			if (!suser())
				return -EPERM;
			rs_wild_int_mask = get_fs_long((unsigned long *) arg);
			if (rs_wild_int_mask < 0)
				rs_wild_int_mask = check_wild_interrupts(0);
			return 0;

		default:
			return -EINVAL;
		}
	return 0;
}

static void rs_set_termios(struct tty_struct *tty, struct termios *old_termios)
{
	struct async_struct *info;

	if (tty->termios->c_cflag == old_termios->c_cflag)
		return;

	info = &rs_table[DEV_TO_SL(tty->line)];

	change_speed(DEV_TO_SL(tty->line));
	
	if ((old_termios->c_cflag & CRTSCTS) &&
	    !(tty->termios->c_cflag & CRTSCTS)) {
		tty->hw_stopped = 0;
		rs_write(tty);
	}

	if (!(old_termios->c_cflag & CLOCAL) &&
	    (tty->termios->c_cflag & CLOCAL))
		wake_up_interruptible(&info->open_wait);

	if (I_INPCK(tty))
		info->read_status_mask = (UART_LSR_OE | UART_LSR_BI |
					  UART_LSR_FE | UART_LSR_PE);
	else
		info->read_status_mask = (UART_LSR_OE | UART_LSR_BI |
					  UART_LSR_FE);
}

/*
 * ------------------------------------------------------------
 * rs_close()
 * 
 * This routine is called when the serial port gets closed.  First, we
 * wait for the last remaining data to be sent.  Then, we unlink its
 * async structure from the interrupt chain if necessary, and we free
 * that IRQ if nothing is left in the chain.
 * ------------------------------------------------------------
 */
static void rs_close(struct tty_struct *tty, struct file * filp)
{
	struct async_struct * info;
	int line;

	if (tty_hung_up_p(filp))
		return;
	
	line = DEV_TO_SL(tty->line);
	if ((line < 0) || (line >= NR_PORTS))
		return;
	info = rs_table + line;
#ifdef SERIAL_DEBUG_OPEN
	printk("rs_close ttys%d, count = %d\n", info->line, info->count);
#endif
	if ((tty->count == 1) && (info->count != 1)) {
		/*
		 * Uh, oh.  tty->count is 1, which means that the tty
		 * structure will be freed.  Info->count should always
		 * be one in these conditions.  If it's greater than
		 * one, we've got real problems, since it means the
		 * serial port won't be shutdown.
		 */
		printk("rs_close: bad serial port count; tty->count is 1, "
		       "info->count is %d\n", info->count);
		info->count = 1;
	}
	if (--info->count < 0) {
		printk("rs_close: bad serial port count for ttys%d: %d\n",
		       info->line, info->count);
		info->count = 0;
	}
	if (info->count)
		return;
	info->flags |= ASYNC_CLOSING;
	/*
	 * Save the termios structure, since this port may have
	 * separate termios for callout and dialin.
	 */
	if (info->flags & ASYNC_NORMAL_ACTIVE)
		info->normal_termios = *tty->termios;
	if (info->flags & ASYNC_CALLOUT_ACTIVE)
		info->callout_termios = *tty->termios;
	tty->stopped = 0;		/* Force flush to succeed */
	tty->hw_stopped = 0;
	if (info->flags & ASYNC_INITIALIZED) {
		rs_start(tty);
		wait_until_sent(tty, 6000); /* 60 seconds timeout */
	} else
		flush_output(tty);
	flush_input(tty);
	cli();
	/*
	 * Make sure the UART transmitter has completely drained; this
	 * is especially important if there is a transmit FIFO!
	 */
	if (!(serial_inp(info, UART_LSR) & UART_LSR_THRE)) {
		rs_start(tty);	/* Make sure THRI interrupt enabled */
		interruptible_sleep_on(&info->xmit_wait);
	}
	sti();
	shutdown(info, 1);
	clear_bit(line, rs_event);
	info->event = 0;
	info->tty = 0;
	if (info->blocked_open) {
		if (info->close_delay) {
			tty->count++; /* avoid race condition */
			current->state = TASK_INTERRUPTIBLE;
			current->timeout = jiffies + info->close_delay;
			schedule();
			tty->count--;
		}
		wake_up_interruptible(&info->open_wait);
	}
	info->flags &= ~(ASYNC_NORMAL_ACTIVE|ASYNC_CALLOUT_ACTIVE|
			 ASYNC_CLOSING);
	wake_up_interruptible(&info->close_wait);
}

/*
 * rs_hangup() --- called by tty_hangup() when a hangup is signaled.
 */
void rs_hangup(struct tty_struct *tty)
{
	struct async_struct * info;
	int line;

	line = DEV_TO_SL(tty->line);
	if ((line < 0) || (line >= NR_PORTS))
		return;
	info = rs_table + line;
	
	shutdown(info, 1);
	clear_bit(line, rs_event);
	info->event = 0;
	info->count = 0;
	info->flags &= ~(ASYNC_NORMAL_ACTIVE|ASYNC_CALLOUT_ACTIVE);
	info->tty = 0;
	wake_up_interruptible(&info->open_wait);
}

/*
 * ------------------------------------------------------------
 * rs_open() and friends
 * ------------------------------------------------------------
 */
static int block_til_ready(struct tty_struct *tty, struct file * filp,
			   struct async_struct *info)
{
	struct wait_queue wait = { current, NULL };
	int		retval;
	int		do_clocal = C_CLOCAL(tty);

	/*
	 * If the device is in the middle of being closed, then block
	 * until it's done, and then try again.
	 */
	if (info->flags & ASYNC_CLOSING) {
		interruptible_sleep_on(&info->close_wait);
#ifdef SERIAL_DO_RESTART
		if (info->flags & ASYNC_HUP_NOTIFY)
			return -EAGAIN;
		else
			return -ERESTARTSYS;
#else
		return -EAGAIN;
#endif
	}

	/*
	 * If this is a callout device, then just make sure the normal
	 * device isn't being used.
	 */
	if (MAJOR(filp->f_rdev) == TTYAUX_MAJOR) {
		if (info->flags & ASYNC_NORMAL_ACTIVE)
			return -EBUSY;
		if ((info->flags & ASYNC_CALLOUT_ACTIVE) &&
		    (info->flags & ASYNC_SESSION_LOCKOUT) &&
		    (info->session != current->session))
		    return -EBUSY;
		if ((info->flags & ASYNC_CALLOUT_ACTIVE) &&
		    (info->flags & ASYNC_PGRP_LOCKOUT) &&
		    (info->pgrp != current->pgrp))
		    return -EBUSY;
		info->flags |= ASYNC_CALLOUT_ACTIVE;
		return 0;
	}
	
	/*
	 * If non-blocking mode is set, then make the check up front
	 * and then exit.
	 */
	if (filp->f_flags & O_NONBLOCK) {
		if (info->flags & ASYNC_CALLOUT_ACTIVE)
			return -EBUSY;
		info->flags |= ASYNC_NORMAL_ACTIVE;
		return 0;
	}

	/*
	 * Block waiting for the carrier detect and the line to become
	 * free (i.e., not in use by the callout).  While we are in
	 * this loop, info->count is dropped by one, so that
	 * rs_close() knows when to free things.  We restore it upon
	 * exit, either normal or abnormal.
	 */
	retval = 0;
	add_wait_queue(&info->open_wait, &wait);
#ifdef SERIAL_DEBUG_OPEN
	printk("block_til_ready before block: ttys%d, count = %d\n",
	       info->line, info->count);
#endif
	info->count--;
	info->blocked_open++;
	while (1) {
		cli();
		if (!(info->flags & ASYNC_CALLOUT_ACTIVE))
			serial_out(info, UART_MCR,
				   serial_inp(info, UART_MCR) |
				   (UART_MCR_DTR | UART_MCR_RTS));
		sti();
		current->state = TASK_INTERRUPTIBLE;
		if (tty_hung_up_p(filp) ||
		    !(info->flags & ASYNC_INITIALIZED)) {
#ifdef SERIAL_DO_RESTART
			if (info->flags & ASYNC_HUP_NOTIFY)
				retval = -EAGAIN;
			else
				retval = -ERESTARTSYS;
#else
			retval = -EAGAIN;
#endif
			break;
		}
		if (!(info->flags & ASYNC_CALLOUT_ACTIVE) &&
		    !(info->flags & ASYNC_CLOSING) &&
		    (do_clocal || (serial_in(info, UART_MSR) &
				   UART_MSR_DCD)))
			break;
		if (current->signal & ~current->blocked) {
			retval = -ERESTARTSYS;
			break;
		}
#ifdef SERIAL_DEBUG_OPEN
		printk("block_til_ready blocking: ttys%d, count = %d\n",
		       info->line, info->count);
#endif
		schedule();
	}
	current->state = TASK_RUNNING;
	remove_wait_queue(&info->open_wait, &wait);
	if (!tty_hung_up_p(filp))
		info->count++;
	info->blocked_open--;
#ifdef SERIAL_DEBUG_OPEN
	printk("block_til_ready after blocking: ttys%d, count = %d\n",
	       info->line, info->count);
#endif
	if (retval)
		return retval;
	info->flags |= ASYNC_NORMAL_ACTIVE;
	return 0;
}	

/*
 * This routine is called whenever a serial port is opened.  It
 * enables interrupts for a serial port, linking in its async structure into
 * the IRQ chain.   It also performs the serial-speicific
 * initalization for the tty structure.
 */
int rs_open(struct tty_struct *tty, struct file * filp)
{
	struct async_struct	*info;
	int 			retval, line;

	line = DEV_TO_SL(tty->line);
	if ((line < 0) || (line >= NR_PORTS))
		return -ENODEV;
	info = rs_table + line;
#ifdef SERIAL_DEBUG_OPEN
	printk("rs_open ttys%d, count = %d\n", info->line, info->count);
#endif
	info->count++;
	info->tty = tty;
	
	tty->write = rs_write;
	tty->close = rs_close;
	tty->ioctl = rs_ioctl;
	tty->throttle = rs_throttle;
	tty->set_termios = rs_set_termios;
	tty->stop = rs_stop;
	tty->start = rs_start;
	tty->hangup = rs_hangup;
	if ((info->count == 1) && (info->flags & ASYNC_SPLIT_TERMIOS)) {
		if (MAJOR(filp->f_rdev) == TTY_MAJOR)
			*tty->termios = info->normal_termios;
		else 
			*tty->termios = info->callout_termios;
	}
	/*
	 * Start up serial port
	 */
	retval = startup(info, 1);
	if (retval)
		return retval;

	retval = block_til_ready(tty, filp, info);
	if (retval) {
#ifdef SERIAL_DEBUG_OPEN
		printk("rs_open returning after block_til_ready with %d\n",
		       retval);
#endif
		return retval;
	}

	info->session = current->session;
	info->pgrp = current->pgrp;

#ifdef SERIAL_DEBUG_OPEN
	printk("rs_open ttys%d successful...", info->line);
#endif
	return 0;
}

/*
 * ---------------------------------------------------------------------
 * rs_init() and friends
 *
 * rs_init() is called at boot-time to initialize the serial driver.
 * ---------------------------------------------------------------------
 */

/*
 * This routine prints out the appropriate serial driver version
 * number, and identifies which options were configured into this
 * driver.
 */
static void show_serial_version(void)
{
	printk("Serial driver version 3.99a with");
#ifdef CONFIG_AST_FOURPORT
	printk(" AST_FOURPORT");
#define SERIAL_OPT
#endif
#ifdef CONFIG_ACCENT_ASYNC
	printk(" ACCENT_ASYNC");
#define SERIAL_OPT
#endif
#ifdef CONFIG_HUB6
	printk(" HUB-6");
#define SERIAL_OPT
#endif
#ifdef CONFIG_AUTO_IRQ
	printk (" AUTO_IRQ");
#define SERIAL_OPT
#endif
#ifdef SERIAL_OPT
	printk(" enabled\n");
#else
	printk(" no serial options enabled\n");
#endif
#undef SERIAL_OPT
}

/*
 * This routine is called by do_auto_irq(); it attempts to determine
 * which interrupt a serial port is configured to use.  It is not
 * fool-proof, but it works a large part of the time.
 */
static int get_auto_irq(struct async_struct *info)
{
	unsigned char save_MCR, save_IER, save_ICP=0;
	unsigned short ICP=0, port = info->port;
	unsigned long timeout;
	
	/*
	 * Enable interrupts and see who answers
	 */
	rs_irq_triggered = 0;
	cli();
	save_IER = serial_inp(info, UART_IER);
	save_MCR = serial_inp(info, UART_MCR);
	if (info->flags & ASYNC_FOURPORT)  {
		serial_outp(info, UART_MCR, UART_MCR_DTR | UART_MCR_RTS);
		serial_outp(info, UART_IER, 0x0f);	/* enable all intrs */
		ICP = (port & 0xFE0) | 0x01F;
		save_ICP = inb_p(ICP);
		outb_p(0x80, ICP);
		(void) inb_p(ICP);
	} else {
		serial_outp(info, UART_MCR,
			    UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);
		serial_outp(info, UART_IER, 0x0f);	/* enable all intrs */
	}
	sti();
	/*
	 * Next, clear the interrupt registers.
	 */
	(void)serial_inp(info, UART_LSR);
	(void)serial_inp(info, UART_RX);
	(void)serial_inp(info, UART_IIR);
	(void)serial_inp(info, UART_MSR);
	
	timeout = jiffies+2;
	while (timeout >= jiffies) {
		if (rs_irq_triggered)
			break;
	}
	/*
	 * Now check to see if we got any business, and clean up.
	 */
	cli();
	serial_outp(info, UART_IER, save_IER);
	serial_outp(info, UART_MCR, save_MCR);
	if (info->flags & ASYNC_FOURPORT)
		outb_p(save_ICP, ICP);
	sti();
	return(rs_irq_triggered);
}

/*
 * Calls get_auto_irq() multiple times, to make sure we don't get
 * faked out by random interrupts
 */
static int do_auto_irq(struct async_struct * info)
{
	unsigned 		port = info->port;
	int 			irq_lines = 0;
	int			irq_try_1 = 0, irq_try_2 = 0;
	int			retries;
	unsigned long flags;

	if (!port)
		return 0;

	/* Turn on interrupts (they may be off) */
	save_flags(flags); sti();

	irq_lines = grab_all_interrupts(rs_wild_int_mask);
	
	for (retries = 0; retries < 5; retries++) {
		if (!irq_try_1)
			irq_try_1 = get_auto_irq(info);
		if (!irq_try_2)
			irq_try_2 = get_auto_irq(info);
		if (irq_try_1 && irq_try_2) {
			if (irq_try_1 == irq_try_2)
				break;
			irq_try_1 = irq_try_2 = 0;
		}
	}
	restore_flags(flags);
	free_all_interrupts(irq_lines);
	return (irq_try_1 == irq_try_2) ? irq_try_1 : 0;
}

/*
 * This routine is called by rs_init() to initialize a specific serial
 * port.  It determines what type of UART ship this serial port is
 * using: 8250, 16450, 16550, 16550A.  The important question is
 * whether or not this UART is a 16550A or not, since this will
 * determine whether or not we can use its FIFO features or not.
 */
static void autoconfig(struct async_struct * info)
{
	unsigned char status1, status2, scratch, scratch2;
	unsigned port = info->port;
	unsigned long flags;

	info->type = PORT_UNKNOWN;
	
	if (!port)
		return;

	save_flags(flags); cli();
	
	/*
	 * Do a simple existence test first; if we fail this, there's
	 * no point trying anything else.
	 */
	scratch = serial_inp(info, UART_IER);
	serial_outp(info, UART_IER, 0);
	scratch2 = serial_inp(info, UART_IER);
	serial_outp(info, UART_IER, scratch);
	if (scratch2) {
		restore_flags(flags);
		return;		/* We failed; there's nothing here */
	}

	/* 
	 * Check to see if a UART is really there.  Certain broken
	 * internal modems based on the Rockwell chipset fail this
	 * test, because they apparently don't implement the loopback
	 * test mode.  So this test is skipped on the COM 1 through
	 * COM 4 ports.  This *should* be safe, since no board
	 * manufactucturer would be stupid enough to design a board
	 * that conflicts with COM 1-4 --- we hope!
	 */
	if (!(info->flags & ASYNC_SKIP_TEST)) {
		scratch = serial_inp(info, UART_MCR);
		serial_outp(info, UART_MCR, UART_MCR_LOOP | scratch);
		scratch2 = serial_inp(info, UART_MSR);
		serial_outp(info, UART_MCR, UART_MCR_LOOP | 0x0A);
		status1 = serial_inp(info, UART_MSR) & 0xF0;
		serial_outp(info, UART_MCR, scratch);
		serial_outp(info, UART_MSR, scratch2);
		if (status1 != 0x90) {
			restore_flags(flags);
			return;
		}
	} 
	
	/*
	 * If the AUTO_IRQ flag is set, try to do the automatic IRQ
	 * detection.
	 */
	if (info->flags & ASYNC_AUTO_IRQ)
		info->irq = do_auto_irq(info);
		
	serial_outp(info, UART_FCR, UART_FCR_ENABLE_FIFO);
	scratch = serial_in(info, UART_IIR) >> 6;
	info->xmit_fifo_size = 1;
	switch (scratch) {
		case 0:
			info->type = PORT_16450;
			break;
		case 1:
			info->type = PORT_UNKNOWN;
			break;
		case 2:
			info->type = PORT_16550;
			break;
		case 3:
			info->type = PORT_16550A;
			info->xmit_fifo_size = 16;
			break;
	}
	if (info->type == PORT_16450) {
		scratch = serial_in(info, UART_SCR);
		serial_outp(info, UART_SCR, 0xa5);
		status1 = serial_in(info, UART_SCR);
		serial_outp(info, UART_SCR, 0x5a);
		status2 = serial_in(info, UART_SCR);
		serial_outp(info, UART_SCR, scratch);

		if ((status1 != 0xa5) || (status2 != 0x5a))
			info->type = PORT_8250;
	}

	/*
	 * Reset the UART.
	 */
	serial_outp(info, UART_MCR, 0x00);
	serial_outp(info, UART_FCR, (UART_FCR_CLEAR_RCVR |
				     UART_FCR_CLEAR_XMIT));
	(void)serial_in(info, UART_RX);
	
	restore_flags(flags);
}

/*
 * The serial driver boot-time initialization code!
 */
long rs_init(long kmem_start)
{
	int i;
	struct async_struct * info;
	
	memset(&rs_event, 0, sizeof(rs_event));
	bh_base[SERIAL_BH].routine = do_softint;
	timer_table[RS_TIMER].fn = rs_timer;
	timer_table[RS_TIMER].expires = 0;
	IRQ_active = 0;
#ifdef CONFIG_AUTO_IRQ
	rs_wild_int_mask = check_wild_interrupts(1);
#endif

	for (i = 0; i < 16; i++) {
		IRQ_ports[i] = 0;
		IRQ_timeout[i] = 0;
	}
	
	show_serial_version();
	for (i = 0, info = rs_table; i < NR_PORTS; i++,info++) {
		info->line = i;
		info->tty = 0;
		info->type = PORT_UNKNOWN;
		info->custom_divisor = 0;
		info->close_delay = 50;
		info->x_char = 0;
		info->event = 0;
		info->count = 0;
		info->blocked_open = 0;
		memset(&info->callout_termios, 0, sizeof(struct termios));
		memset(&info->normal_termios, 0, sizeof(struct termios));
		info->open_wait = 0;
		info->xmit_wait = 0;
		info->close_wait = 0;
		info->next_port = 0;
		info->prev_port = 0;
		if (info->irq == 2)
			info->irq = 9;
		if (!(info->flags & ASYNC_BOOT_AUTOCONF))
			continue;
		autoconfig(info);
		if (info->type == PORT_UNKNOWN)
			continue;
		printk("tty%02d%s at 0x%04x (irq = %d)", info->line, 
		       (info->flags & ASYNC_FOURPORT) ? " FourPort" : "",
		       info->port, info->irq);
		switch (info->type) {
			case PORT_8250:
				printk(" is a 8250\n");
				break;
			case PORT_16450:
				printk(" is a 16450\n");
				break;
			case PORT_16550:
				printk(" is a 16550\n");
				break;
			case PORT_16550A:
				printk(" is a 16550A\n");
				break;
			default:
				printk("\n");
				break;
		}
	}
	return kmem_start;
}

