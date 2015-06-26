/*
 *  linux/kernel/tty_io.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * 'tty_io.c' gives an orthogonal feeling to tty's, be they consoles
 * or rs-channels. It also implements echoing, cooked mode etc.
 *
 * Kill-line thanks to John T Kohl, who also corrected VMIN = VTIME = 0.
 *
 * Modified by Theodore Ts'o, 9/14/92, to dynamically allocate the
 * tty_struct and tty_queue structures.  Previously there was a array
 * of 256 tty_struct's which was statically allocated, and the
 * tty_queue structures were allocated at boot time.  Both are now
 * dynamically allocated only when the tty is open.
 *
 * Also restructured routines so that there is more of a separation
 * between the high-level tty routines (tty_io.c and tty_ioctl.c) and
 * the low-level tty routines (serial.c, pty.c, console.c).  This
 * makes for cleaner and more compact code.  -TYT, 9/17/92 
 *
 * Modified by Fred N. van Kempen, 01/29/93, to add line disciplines
 * which can be dynamically activated and de-activated by the line
 * discipline handling modules (like SLIP).
 *
 * NOTE: pay no attention to the line discpline code (yet); its
 * interface is still subject to change in this version...
 * -- TYT, 1/31/92
 *
 * Added functionality to the OPOST tty handling.  No delays, but all
 * other bits should be there.
 *	-- Nick Holloway <alfie@dcs.warwick.ac.uk>, 27th May 1993.
 *
 * Rewrote canonical mode and added more termios flags.
 * 	-- julian@uhunix.uhcc.hawaii.edu (J. Cowley), 13Jan94
 */

#include <linux/types.h>
#include <linux/major.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/fcntl.h>
#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/timer.h>
#include <linux/ctype.h>
#include <linux/kd.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/malloc.h>

#include <asm/segment.h>
#include <asm/system.h>
#include <asm/bitops.h>

#include "kbd_kern.h"
#include "vt_kern.h"

#define CONSOLE_DEV MKDEV(TTY_MAJOR,0)

#define MAX_TTYS 256

struct tty_struct *tty_table[MAX_TTYS];
struct termios *tty_termios[MAX_TTYS];	/* We need to keep the termios state */
				  	/* around, even when a tty is closed */
struct termios *termios_locked[MAX_TTYS]; /* Bitfield of locked termios flags*/
struct tty_ldisc ldiscs[NR_LDISCS];	/* line disc dispatch table	*/
int tty_check_write[MAX_TTYS/32];	/* bitfield for the bh handler */

/*
 * fg_console is the current virtual console,
 * redirect is the pseudo-tty that console output
 * is redirected to if asked by TIOCCONS.
 */
int fg_console = 0;
struct tty_struct * redirect = NULL;
struct wait_queue * keypress_wait = NULL;

static void initialize_tty_struct(int line, struct tty_struct *tty);
static void initialize_termios(int line, struct termios *tp);

static int tty_read(struct inode *, struct file *, char *, int);
static int tty_write(struct inode *, struct file *, char *, int);
static int tty_select(struct inode *, struct file *, int, select_table *);
static int tty_open(struct inode *, struct file *);
static void tty_release(struct inode *, struct file *);

int tty_register_ldisc(int disc, struct tty_ldisc *new_ldisc)
{
	if (disc < N_TTY || disc >= NR_LDISCS)
		return -EINVAL;
	
	if (new_ldisc) {
		ldiscs[disc] = *new_ldisc;
		ldiscs[disc].flags |= LDISC_FLAG_DEFINED;
	} else
		memset(&ldiscs[disc], 0, sizeof(struct tty_ldisc));
	
	return 0;
}

void put_tty_queue(unsigned char c, struct tty_queue * queue)
{
	int head;
	unsigned long flags;

	save_flags(flags);
	cli();
	head = (queue->head + 1) & (TTY_BUF_SIZE-1);
	if (head != queue->tail) {
		queue->buf[queue->head] = c;
		queue->head = head;
	}
	restore_flags(flags);
}

int get_tty_queue(struct tty_queue * queue)
{
	int result = -1;
	unsigned long flags;

	save_flags(flags);
	cli();
	if (queue->tail != queue->head) {
		result = queue->buf[queue->tail];
		INC(queue->tail);
	}
	restore_flags(flags);
	return result;
}

/*
 * This routine copies out a maximum of buflen characters from the
 * read_q; it is a convenience for line disciplines so they can grab a
 * large block of data without calling get_tty_char directly.  It
 * returns the number of characters actually read. Return terminates
 * if an error character is read from the queue and the return value
 * is negated.
 */
int tty_read_raw_data(struct tty_struct *tty, unsigned char *bufp, int buflen)
{
	int	result = 0;
	unsigned char	*p = bufp;
	unsigned long flags;
	int head, tail;
	int ok = 1;

	save_flags(flags);
	cli();
	tail = tty->read_q.tail;
	head = tty->read_q.head;
	while ((result < buflen) && (tail!=head) && ok) {
		ok = !clear_bit (tail, &tty->readq_flags);
		*p++ =  tty->read_q.buf[tail++];
		tail &= TTY_BUF_SIZE-1;
		result++;
	}
	tty->read_q.tail = tail;
	restore_flags(flags);
	return (ok) ? result : -result;
}


void tty_write_flush(struct tty_struct * tty)
{
	if (!tty->write || EMPTY(&tty->write_q))
		return;
	if (set_bit(TTY_WRITE_BUSY,&tty->flags))
		return;
	tty->write(tty);
	if (!clear_bit(TTY_WRITE_BUSY,&tty->flags))
		printk("tty_write_flush: bit already cleared\n");
}

void tty_read_flush(struct tty_struct * tty)
{
	if (!tty || EMPTY(&tty->read_q))
		return;
	if (set_bit(TTY_READ_BUSY, &tty->flags))
		return;
	ldiscs[tty->disc].handler(tty);
	if (!clear_bit(TTY_READ_BUSY, &tty->flags))
		printk("tty_read_flush: bit already cleared\n");
}

static int hung_up_tty_read(struct inode * inode, struct file * file, char * buf, int count)
{
	return 0;
}

static int hung_up_tty_write(struct inode * inode, struct file * file, char * buf, int count)
{
	return -EIO;
}

static int hung_up_tty_select(struct inode * inode, struct file * filp, int sel_type, select_table * wait)
{
	return 1;
}

static int hung_up_tty_ioctl(struct inode * inode, struct file * file,
			     unsigned int cmd, unsigned long arg)
{
	return -EIO;
}

static int tty_lseek(struct inode * inode, struct file * file, off_t offset, int orig)
{
	return -ESPIPE;
}

static struct file_operations tty_fops = {
	tty_lseek,
	tty_read,
	tty_write,
	NULL,		/* tty_readdir */
	tty_select,
	tty_ioctl,
	NULL,		/* tty_mmap */
	tty_open,
	tty_release
};

static struct file_operations hung_up_tty_fops = {
	tty_lseek,
	hung_up_tty_read,
	hung_up_tty_write,
	NULL,		/* hung_up_tty_readdir */
	hung_up_tty_select,
	hung_up_tty_ioctl,
	NULL,		/* hung_up_tty_mmap */
	NULL,		/* hung_up_tty_open */
	tty_release	/* hung_up_tty_release */
};

void do_tty_hangup(struct tty_struct * tty, struct file_operations *fops)
{
	int i;
	struct file * filp;
	struct task_struct *p;
	int dev;

	if (!tty)
		return;
	dev = MKDEV(TTY_MAJOR,tty->line);
	for (filp = first_file, i=0; i<nr_files; i++, filp = filp->f_next) {
		if (!filp->f_count)
			continue;
		if (filp->f_rdev != dev)
			continue;
		if (filp->f_inode && filp->f_inode->i_rdev == CONSOLE_DEV)
			continue;
		if (filp->f_op != &tty_fops)
			continue;
		filp->f_op = fops;
	}
	flush_input(tty);
	flush_output(tty);
	wake_up_interruptible(&tty->secondary.proc_list);
	if (tty->session > 0) {
		kill_sl(tty->session,SIGHUP,1);
		kill_sl(tty->session,SIGCONT,1);
	}
	tty->session = 0;
	tty->pgrp = -1;
 	for_each_task(p) {
		if (p->tty == tty->line)
			p->tty = -1;
	}
	if (tty->hangup)
		(tty->hangup)(tty);
}

void tty_hangup(struct tty_struct * tty)
{
#ifdef TTY_DEBUG_HANGUP
	printk("tty%d hangup...\n", tty->line);
#endif
	do_tty_hangup(tty, &hung_up_tty_fops);
}

void tty_vhangup(struct tty_struct * tty)
{
#ifdef TTY_DEBUG_HANGUP
	printk("tty%d vhangup...\n", tty->line);
#endif
	do_tty_hangup(tty, &hung_up_tty_fops);
}

int tty_hung_up_p(struct file * filp)
{
	return (filp->f_op == &hung_up_tty_fops);
}

/*
 * This function is typically called only by the session leader, when
 * it wants to dissassociate itself from its controlling tty.
 *
 * It performs the following functions:
 * 	(1)  Sends a SIGHUP and SIGCONT to the foreground process group
 * 	(2)  Clears the tty from being controlling the session
 * 	(3)  Clears the controlling tty for all processes in the
 * 		session group.
 */
void disassociate_ctty(int priv)
{
	struct tty_struct *tty;
	struct task_struct *p;

	if (current->tty >= 0) {
		tty = tty_table[current->tty];
		if (tty) {
			if (tty->pgrp > 0) {
				kill_pg(tty->pgrp, SIGHUP, priv);
				kill_pg(tty->pgrp, SIGCONT, priv);
			}
			tty->session = 0;
			tty->pgrp = -1;
		} else
			printk("disassociate_ctty: ctty is NULL?!?");
	}

	for_each_task(p)
	  	if (p->session == current->session)
			p->tty = -1;
}

/*
 * Sometimes we want to wait until a particular VT has been activated. We
 * do it in a very simple manner. Everybody waits on a single queue and
 * get woken up at once. Those that are satisfied go on with their business,
 * while those not ready go back to sleep. Seems overkill to add a wait
 * to each vt just for this - usually this does nothing!
 */
static struct wait_queue *vt_activate_queue = NULL;

/*
 * Sleeps until a vt is activated, or the task is interrupted. Returns
 * 0 if activation, -1 if interrupted.
 */
int vt_waitactive(void)
{
	interruptible_sleep_on(&vt_activate_queue);
	return (current->signal & ~current->blocked) ? -1 : 0;
}

#define vt_wake_waitactive() wake_up(&vt_activate_queue)

extern int kill_proc(int pid, int sig, int priv);

/*
 * Performs the back end of a vt switch
 */
void complete_change_console(unsigned int new_console)
{
	unsigned char old_vc_mode;

	if (new_console == fg_console || new_console >= NR_CONSOLES)
		return;

	/*
	 * If we're switching, we could be going from KD_GRAPHICS to
	 * KD_TEXT mode or vice versa, which means we need to blank or
	 * unblank the screen later.
	 */
	old_vc_mode = vt_cons[fg_console].vc_mode;
	update_screen(new_console);

	/*
	 * If this new console is under process control, send it a signal
	 * telling it that it has acquired. Also check if it has died and
	 * clean up (similar to logic employed in change_console())
	 */
	if (vt_cons[new_console].vt_mode.mode == VT_PROCESS)
	{
		/*
		 * Send the signal as privileged - kill_proc() will
		 * tell us if the process has gone or something else
		 * is awry
		 */
		if (kill_proc(vt_cons[new_console].vt_pid,
			      vt_cons[new_console].vt_mode.acqsig,
			      1) != 0)
		{
		/*
		 * The controlling process has died, so we revert back to
		 * normal operation. In this case, we'll also change back
		 * to KD_TEXT mode. I'm not sure if this is strictly correct
		 * but it saves the agony when the X server dies and the screen
		 * remains blanked due to KD_GRAPHICS! It would be nice to do
		 * this outside of VT_PROCESS but there is no single process
		 * to account for and tracking tty count may be undesirable.
		 */
			vt_cons[new_console].vc_mode = KD_TEXT;
			clr_vc_kbd_mode(kbd_table + new_console, VC_RAW);
			clr_vc_kbd_mode(kbd_table + new_console, VC_MEDIUMRAW);
 			vt_cons[new_console].vt_mode.mode = VT_AUTO;
 			vt_cons[new_console].vt_mode.waitv = 0;
 			vt_cons[new_console].vt_mode.relsig = 0;
			vt_cons[new_console].vt_mode.acqsig = 0;
			vt_cons[new_console].vt_mode.frsig = 0;
			vt_cons[new_console].vt_pid = -1;
			vt_cons[new_console].vt_newvt = -1;
		}
	}

	/*
	 * We do this here because the controlling process above may have
	 * gone, and so there is now a new vc_mode
	 */
	if (old_vc_mode != vt_cons[new_console].vc_mode)
	{
		if (vt_cons[new_console].vc_mode == KD_TEXT)
			unblank_screen();
		else {
			timer_active &= ~(1<<BLANK_TIMER);
			blank_screen();
		}
	}

	/*
	 * Wake anyone waiting for their VT to activate
	 */
	vt_wake_waitactive();
	return;
}

/*
 * Performs the front-end of a vt switch
 */
void change_console(unsigned int new_console)
{
	if (new_console == fg_console || new_console >= NR_CONSOLES)
		return;

	/*
	 * If this vt is in process mode, then we need to handshake with
	 * that process before switching. Essentially, we store where that
	 * vt wants to switch to and wait for it to tell us when it's done
	 * (via VT_RELDISP ioctl).
	 *
	 * We also check to see if the controlling process still exists.
	 * If it doesn't, we reset this vt to auto mode and continue.
	 * This is a cheap way to track process control. The worst thing
	 * that can happen is: we send a signal to a process, it dies, and
	 * the switch gets "lost" waiting for a response; hopefully, the
	 * user will try again, we'll detect the process is gone (unless
	 * the user waits just the right amount of time :-) and revert the
	 * vt to auto control.
	 */
	if (vt_cons[fg_console].vt_mode.mode == VT_PROCESS)
	{
		/*
		 * Send the signal as privileged - kill_proc() will
		 * tell us if the process has gone or something else
		 * is awry
		 */
		if (kill_proc(vt_cons[fg_console].vt_pid,
			      vt_cons[fg_console].vt_mode.relsig,
			      1) == 0)
		{
			/*
			 * It worked. Mark the vt to switch to and
			 * return. The process needs to send us a
			 * VT_RELDISP ioctl to complete the switch.
			 */
			vt_cons[fg_console].vt_newvt = new_console;
			return;
		}

		/*
		 * The controlling process has died, so we revert back to
		 * normal operation. In this case, we'll also change back
		 * to KD_TEXT mode. I'm not sure if this is strictly correct
		 * but it saves the agony when the X server dies and the screen
		 * remains blanked due to KD_GRAPHICS! It would be nice to do
		 * this outside of VT_PROCESS but there is no single process
		 * to account for and tracking tty count may be undesirable.
		 */
		vt_cons[fg_console].vc_mode = KD_TEXT;
		clr_vc_kbd_mode(kbd_table + fg_console, VC_RAW);
		clr_vc_kbd_mode(kbd_table + fg_console, VC_MEDIUMRAW);
		vt_cons[fg_console].vt_mode.mode = VT_AUTO;
		vt_cons[fg_console].vt_mode.waitv = 0;
		vt_cons[fg_console].vt_mode.relsig = 0;
		vt_cons[fg_console].vt_mode.acqsig = 0;
		vt_cons[fg_console].vt_mode.frsig = 0;
		vt_cons[fg_console].vt_pid = -1;
		vt_cons[fg_console].vt_newvt = -1;
		/*
		 * Fall through to normal (VT_AUTO) handling of the switch...
		 */
	}

	/*
	 * Ignore all switches in KD_GRAPHICS+VT_AUTO mode
	 */
	if (vt_cons[fg_console].vc_mode == KD_GRAPHICS)
		return;

	complete_change_console(new_console);
}

void wait_for_keypress(void)
{
	sleep_on(&keypress_wait);
}

void stop_tty(struct tty_struct *tty)
{
	if (tty->stopped)
		return;
	tty->stopped = 1;
	if (tty->link && tty->link->packet) {
		tty->ctrl_status &= ~TIOCPKT_START;
		tty->ctrl_status |= TIOCPKT_STOP;
		wake_up_interruptible(&tty->link->secondary.proc_list);
	}
	if (tty->stop)
		(tty->stop)(tty);
	if (IS_A_CONSOLE(tty->line)) {
		set_vc_kbd_led(kbd_table + fg_console, VC_SCROLLOCK);
		set_leds();
	}
}

void start_tty(struct tty_struct *tty)
{
	if (!tty->stopped)
		return;
	tty->stopped = 0;
	if (tty->link && tty->link->packet) {
		tty->ctrl_status &= ~TIOCPKT_STOP;
		tty->ctrl_status |= TIOCPKT_START;
		wake_up_interruptible(&tty->link->secondary.proc_list);
	}
	if (tty->start)
		(tty->start)(tty);
	TTY_WRITE_FLUSH(tty);
	if (IS_A_CONSOLE(tty->line)) {
		clr_vc_kbd_led(kbd_table + fg_console, VC_SCROLLOCK);
		set_leds();
	}
}

/* Perform OPOST processing.  Returns -1 when the write_q becomes full
   and the character must be retried. */

static int opost(unsigned char c, struct tty_struct *tty)
{
	if (FULL(&tty->write_q))
		return -1;
	if (O_OPOST(tty)) {
		switch (c) {
		case '\n':
			if (O_ONLRET(tty))
				tty->column = 0;
			if (O_ONLCR(tty)) {
				if (LEFT(&tty->write_q) < 2)
					return -1;
				put_tty_queue('\r', &tty->write_q);
				tty->column = 0;
			}
			tty->canon_column = tty->column;
			break;
		case '\r':
			if (O_ONOCR(tty) && tty->column == 0)
				return 0;
			if (O_OCRNL(tty)) {
				c = '\n';
				if (O_ONLRET(tty))
					tty->canon_column = tty->column = 0;
				break;
			}
			tty->canon_column = tty->column = 0;
			break;
		case '\t':
			if (O_TABDLY(tty) == XTABS) {
				if (LEFT(&tty->write_q) < 8)
					return -1;
				do
					put_tty_queue(' ', &tty->write_q);
				while (++tty->column % 8);
				return 0;
			}
			tty->column = (tty->column | 7) + 1;
			break;
		case '\b':
			if (tty->column > 0)
				tty->column--;
			break;
		default:
			if (O_OLCUC(tty))
				c = toupper(c);
			if (!iscntrl(c))
				tty->column++;
			break;
		}
	}
	put_tty_queue(c, &tty->write_q);
	return 0;
}

/* Must be called only when L_ECHO(tty) is true. */

static void echo_char(unsigned char c, struct tty_struct *tty)
{
	if (L_ECHOCTL(tty) && iscntrl(c) && c != '\t') {
		opost('^', tty);
		opost(c ^ 0100, tty);
	} else
		opost(c, tty);
}

static void eraser(unsigned char c, struct tty_struct *tty)
{
	enum { ERASE, WERASE, KILL } kill_type;
	int seen_alnums;

	if (tty->secondary.head == tty->canon_head) {
		/* opost('\a', tty); */		/* what do you think? */
		return;
	}
	if (c == ERASE_CHAR(tty))
		kill_type = ERASE;
	else if (c == WERASE_CHAR(tty))
		kill_type = WERASE;
	else {
		if (!L_ECHO(tty)) {
			tty->secondary.head = tty->canon_head;
			return;
		}
		if (!L_ECHOK(tty) || !L_ECHOKE(tty)) {
			tty->secondary.head = tty->canon_head;
			if (tty->erasing) {
				opost('/', tty);
				tty->erasing = 0;
			}
			echo_char(KILL_CHAR(tty), tty);
			/* Add a newline if ECHOK is on and ECHOKE is off. */
			if (L_ECHOK(tty))
				opost('\n', tty);
			return;
		}
		kill_type = KILL;
	}

	seen_alnums = 0;
	while (tty->secondary.head != tty->canon_head) {
		c = LAST(&tty->secondary);
		if (kill_type == WERASE) {
			/* Equivalent to BSD's ALTWERASE. */
			if (isalnum(c) || c == '_')
				seen_alnums++;
			else if (seen_alnums)
				break;
		}
		DEC(tty->secondary.head);
		if (L_ECHO(tty)) {
			if (L_ECHOPRT(tty)) {
				if (!tty->erasing) {
					opost('\\', tty);
					tty->erasing = 1;
				}
				echo_char(c, tty);
			} else if (!L_ECHOE(tty)) {
				echo_char(ERASE_CHAR(tty), tty);
			} else if (c == '\t') {
				unsigned int col = tty->canon_column;
				unsigned long tail = tty->canon_head;

				/* Find the column of the last char. */
				while (tail != tty->secondary.head) {
					c = tty->secondary.buf[tail];
					if (c == '\t')
						col = (col | 7) + 1;
					else if (iscntrl(c)) {
						if (L_ECHOCTL(tty))
							col += 2;
					} else
						col++;
					INC(tail);
				}

				/* Now backup to that column. */
				while (tty->column > col) {
					/* Can't use opost here. */
					put_tty_queue('\b', &tty->write_q);
					tty->column--;
				}
			} else {
				if (iscntrl(c) && L_ECHOCTL(tty)) {
					opost('\b', tty);
					opost(' ', tty);
					opost('\b', tty);
				}
				if (!iscntrl(c) || L_ECHOCTL(tty)) {
					opost('\b', tty);
					opost(' ', tty);
					opost('\b', tty);
				}
			}
		}
		if (kill_type == ERASE)
			break;
	}
	if (tty->erasing && tty->secondary.head == tty->canon_head) {
		opost('/', tty);
		tty->erasing = 0;
	}
}

static void isig(int sig, struct tty_struct *tty)
{
	kill_pg(tty->pgrp, sig, 1);
	if (!L_NOFLSH(tty)) {
		flush_input(tty);
		flush_output(tty);
	}
}

static void copy_to_cooked(struct tty_struct * tty)
{
	int c, special_flag;
	unsigned long flags;

	if (!tty) {
		printk("copy_to_cooked: called with NULL tty\n");
		return;
	}
	if (!tty->write) {
		printk("copy_to_cooked: tty %d has null write routine\n",
		       tty->line);
	}
	while (1) {
		/*
		 * Check to see how much room we have left in the
		 * secondary queue.  Send a throttle command or abort
		 * if necessary.
		 */
		c = LEFT(&tty->secondary);
		if (tty->throttle && (c < SQ_THRESHOLD_LW)
		    && !set_bit(TTY_SQ_THROTTLED, &tty->flags))
			tty->throttle(tty, TTY_THROTTLE_SQ_FULL);
		if (c == 0)
			break;
		save_flags(flags); cli();
		if (!EMPTY(&tty->read_q)) {
			c = tty->read_q.buf[tty->read_q.tail];
			special_flag = clear_bit(tty->read_q.tail,
						 &tty->readq_flags);
			INC(tty->read_q.tail);
			restore_flags(flags);
		} else {
			restore_flags(flags);
			break;
		}
		if (special_flag) {
			tty->char_error = c;
			continue;
		}
		if (tty->char_error) {
			if (tty->char_error == TTY_BREAK) {
				tty->char_error = 0;
				if (I_IGNBRK(tty))
					continue;
				/* A break is handled by the lower levels. */
				if (I_BRKINT(tty))
					continue;
				if (I_PARMRK(tty)) {
					put_tty_queue('\377', &tty->secondary);
					put_tty_queue('\0', &tty->secondary);
				}
				put_tty_queue('\0', &tty->secondary);
				continue;
			}
			if (tty->char_error == TTY_OVERRUN) {
				tty->char_error = 0;
				printk("tty%d: input overrun\n", tty->line);
				continue;
			}
			/* Must be a parity or frame error */
			tty->char_error = 0;
			if (I_IGNPAR(tty)) {
				continue;
			}
			if (I_PARMRK(tty)) {
				put_tty_queue('\377', &tty->secondary);
				put_tty_queue('\0', &tty->secondary);
				put_tty_queue(c, &tty->secondary);
			} else
				put_tty_queue('\0', &tty->secondary);
			continue;
		}
		if (I_ISTRIP(tty))
			c &= 0x7f;
		if (!tty->lnext) {
			if (c == '\r') {
				if (I_IGNCR(tty))
					continue;
				if (I_ICRNL(tty))
					c = '\n';
			} else if (c == '\n' && I_INLCR(tty))
				c = '\r';
		}
		if (I_IUCLC(tty) && L_IEXTEN(tty))
			c=tolower(c);
		if (c == __DISABLED_CHAR)
			tty->lnext = 1;
		if (L_ICANON(tty) && !tty->lnext) {
			if (c == ERASE_CHAR(tty) || c == KILL_CHAR(tty) ||
			    (c == WERASE_CHAR(tty) && L_IEXTEN(tty))) {
				eraser(c, tty);
				continue;
			}
			if (c == LNEXT_CHAR(tty) && L_IEXTEN(tty)) {
				tty->lnext = 1;
				if (L_ECHO(tty)) {
					if (tty->erasing) {
						opost('/', tty);
						tty->erasing = 0;
					}
					if (L_ECHOCTL(tty)) {
						opost('^', tty);
						opost('\b', tty);
					}
				}
				continue;
			}
			if (c == REPRINT_CHAR(tty) && L_ECHO(tty) &&
			    L_IEXTEN(tty)) {
				unsigned long tail = tty->canon_head;

				if (tty->erasing) {
					opost('/', tty);
					tty->erasing = 0;
				}
				echo_char(c, tty);
				opost('\n', tty);
				while (tail != tty->secondary.head) {
					echo_char(tty->secondary.buf[tail],
						  tty);
					INC(tail);
				}
				continue;
			}
		}
		if (I_IXON(tty) && !tty->lnext) {
			if ((tty->stopped && I_IXANY(tty) && L_IEXTEN(tty)) ||
			    c == START_CHAR(tty)) {
				start_tty(tty);
				continue;
			}
			if (c == STOP_CHAR(tty)) {
				stop_tty(tty);
				continue;
			}
		}
		if (L_ISIG(tty) && !tty->lnext) {
			if (c == INTR_CHAR(tty)) {
				isig(SIGINT, tty);
				continue;
			}
			if (c == QUIT_CHAR(tty)) {
				isig(SIGQUIT, tty);
				continue;
			}
			if (c == SUSP_CHAR(tty)) {
				if (!is_orphaned_pgrp(tty->pgrp))
					isig(SIGTSTP, tty);
				continue;
			}
		}

		if (tty->erasing) {
			opost('/', tty);
			tty->erasing = 0;
		}
		if (c == '\n' && !tty->lnext) {
			if (L_ECHO(tty) || (L_ICANON(tty) && L_ECHONL(tty)))
				opost('\n', tty);
		} else if (L_ECHO(tty)) {
			/* Don't echo the EOF char in canonical mode.  Sun
			   handles this differently by echoing the char and
			   then backspacing, but that's a hack. */
			if (c != EOF_CHAR(tty) || !L_ICANON(tty) ||
			    tty->lnext) {
				/* Record the column of first canon char. */
				if (tty->canon_head == tty->secondary.head)
					tty->canon_column = tty->column;
				echo_char(c, tty);
			}
		}

		if (I_PARMRK(tty) && c == (unsigned char) '\377' &&
		    (c != EOF_CHAR(tty) || !L_ICANON(tty) || tty->lnext))
			put_tty_queue(c, &tty->secondary);

		if (L_ICANON(tty) && !tty->lnext &&
		    (c == '\n' || c == EOF_CHAR(tty) || c == EOL_CHAR(tty) ||
		     (c == EOL2_CHAR(tty) && L_IEXTEN(tty)))) {
			if (c == EOF_CHAR(tty))
				c = __DISABLED_CHAR;
			set_bit(tty->secondary.head, &tty->secondary_flags);
			put_tty_queue(c, &tty->secondary);
			tty->canon_head = tty->secondary.head;
			tty->canon_data++;
		} else
			put_tty_queue(c, &tty->secondary);
		tty->lnext = 0;
	}
	if (!EMPTY(&tty->write_q))
		TTY_WRITE_FLUSH(tty);
	if (L_ICANON(tty) ? tty->canon_data : !EMPTY(&tty->secondary))
		wake_up_interruptible(&tty->secondary.proc_list);

	if (tty->throttle && (LEFT(&tty->read_q) >= RQ_THRESHOLD_HW)
	    && clear_bit(TTY_RQ_THROTTLED, &tty->flags))
		tty->throttle(tty, TTY_THROTTLE_RQ_AVAIL);
}

int is_ignored(int sig)
{
	return ((current->blocked & (1<<(sig-1))) ||
	        (current->sigaction[sig-1].sa_handler == SIG_IGN));
}

static inline int input_available_p(struct tty_struct *tty)
{
	/* Avoid calling TTY_READ_FLUSH unnecessarily. */
	if (L_ICANON(tty) ? tty->canon_data : !EMPTY(&tty->secondary))
		return 1;

	/* Shuffle any pending data down the queues. */
	TTY_READ_FLUSH(tty);
	if (tty->link)
		TTY_WRITE_FLUSH(tty->link);

	if (L_ICANON(tty) ? tty->canon_data : !EMPTY(&tty->secondary))
		return 1;
	return 0;
}

static int read_chan(struct tty_struct *tty, struct file *file,
		     unsigned char *buf, unsigned int nr)
{
	struct wait_queue wait = { current, NULL };
	int c;
	unsigned char *b = buf;
	int minimum, time;
	int retval = 0;

	/* Job control check -- must be done at start and after
	   every sleep (POSIX.1 7.1.1.4). */
	/* NOTE: not yet done after every sleep pending a thorough
	   check of the logic of this change. -- jlc */
	/* don't stop on /dev/console */
	if (file->f_inode->i_rdev != CONSOLE_DEV &&
	    current->tty == tty->line) {
		if (tty->pgrp <= 0)
			printk("read_chan: tty->pgrp <= 0!\n");
		else if (current->pgrp != tty->pgrp) {
			if (is_ignored(SIGTTIN) ||
			    is_orphaned_pgrp(current->pgrp))
				return -EIO;
			kill_pg(current->pgrp, SIGTTIN, 1);
			return -ERESTARTSYS;
		}
	}

	if (L_ICANON(tty)) {
		minimum = time = 0;
		current->timeout = (unsigned long) -1;
	} else {
		time = (HZ / 10) * TIME_CHAR(tty);
		minimum = MIN_CHAR(tty);
		if (minimum)
		  	current->timeout = (unsigned long) -1;
		else {
			if (time) {
				current->timeout = time + jiffies;
				time = 0;
			} else
				current->timeout = 0;
			minimum = 1;
		}
	}

	add_wait_queue(&tty->secondary.proc_list, &wait);
	while (1) {
		/* First test for status change. */
		if (tty->packet && tty->link->ctrl_status) {
			if (b != buf)
				break;
			put_fs_byte(tty->link->ctrl_status, b++);
			tty->link->ctrl_status = 0;
			break;
		}
		/* This statement must be first before checking for input
		   so that any interrupt will set the state back to
		   TASK_RUNNING. */
		current->state = TASK_INTERRUPTIBLE;
		if (!input_available_p(tty)) {
			if (tty->flags & (1 << TTY_SLAVE_CLOSED)) {
				retval = -EIO;
				break;
			}
			if (tty_hung_up_p(file))
				break;
			if (!current->timeout)
				break;
			if (file->f_flags & O_NONBLOCK) {
				retval = -EAGAIN;
				break;
			}
			if (current->signal & ~current->blocked) {
				retval = -ERESTARTSYS;
				break;
			}
			schedule();
			continue;
		}
		current->state = TASK_RUNNING;

		/* Deal with packet mode. */
		if (tty->packet && b == buf) {
			put_fs_byte(TIOCPKT_DATA, b++);
			nr--;
		}

		while (1) {
			int eol;

			cli();
			if (EMPTY(&tty->secondary)) {
				sti();
				break;
			}
			eol = clear_bit(tty->secondary.tail,
					&tty->secondary_flags);
			c = tty->secondary.buf[tty->secondary.tail];
			if (!nr) {
				/* Gobble up an immediately following EOF if
				   there is no more room in buf (this can
				   happen if the user "pushes" some characters
				   using ^D).  This prevents the next read()
				   from falsely returning EOF. */
				if (eol) {
					if (c == __DISABLED_CHAR) {
						tty->canon_data--;
						INC(tty->secondary.tail);
					} else {
						set_bit(tty->secondary.tail,
							&tty->secondary_flags);
					}
				}
				sti();
				break;
			}
			INC(tty->secondary.tail);
			sti();
			if (eol) {
				if (--tty->canon_data < 0) {
					printk("read_chan: canon_data < 0!\n");
					tty->canon_data = 0;
				}
				if (c == __DISABLED_CHAR)
					break;
				put_fs_byte(c, b++);
				nr--;
				break;
			}
			put_fs_byte(c, b++);
			nr--;
		}

		/* If there is enough space in the secondary queue now, let the
		   low-level driver know. */
		if (tty->throttle && (LEFT(&tty->secondary) >= SQ_THRESHOLD_HW)
		    && clear_bit(TTY_SQ_THROTTLED, &tty->flags))
			tty->throttle(tty, TTY_THROTTLE_SQ_AVAIL);

		if (b - buf >= minimum || !nr)
			break;
		if (time)
			current->timeout = time + jiffies;
	}
	remove_wait_queue(&tty->secondary.proc_list, &wait);
	current->state = TASK_RUNNING;
	current->timeout = 0;
	return (b - buf) ? b - buf : retval;
}

static int write_chan(struct tty_struct * tty, struct file * file,
		      unsigned char * buf, unsigned int nr)
{
	struct wait_queue wait = { current, NULL };
	int c;
	unsigned char *b = buf;
	int retval = 0;

	/* Job control check -- must be done at start (POSIX.1 7.1.1.4). */
	if (L_TOSTOP(tty) && file->f_inode->i_rdev != CONSOLE_DEV) {
		retval = check_change(tty, tty->line);
		if (retval)
			return retval;
	}

	add_wait_queue(&tty->write_q.proc_list, &wait);
	while (1) {
		current->state = TASK_INTERRUPTIBLE;
		if (current->signal & ~current->blocked) {
			retval = -ERESTARTSYS;
			break;
		}
		if (tty_hung_up_p(file) || (tty->link && !tty->link->count)) {
			retval = -EIO;
			break;
		}
		while (nr > 0) {
			c = get_fs_byte(b);
			/* Care is needed here: opost() can abort even
			   if the write_q is not full. */
			if (opost(c, tty) < 0)
				break;
			b++; nr--;
		}
		TTY_WRITE_FLUSH(tty);
		if (!nr)
			break;
		if (EMPTY(&tty->write_q) && !need_resched)
			continue;
		if (file->f_flags & O_NONBLOCK) {
			retval = -EAGAIN;
			break;
		}
		schedule();
	}
	current->state = TASK_RUNNING;
	remove_wait_queue(&tty->write_q.proc_list, &wait);
	return (b - buf) ? b - buf : retval;
}

static int tty_read(struct inode * inode, struct file * file, char * buf, int count)
{
	int i, dev;
	struct tty_struct * tty;

	dev = file->f_rdev;
	if (MAJOR(dev) != TTY_MAJOR) {
		printk("tty_read: bad pseudo-major nr #%d\n", MAJOR(dev));
		return -EINVAL;
	}
	dev = MINOR(dev);
	tty = TTY_TABLE(dev);
	if (!tty || (tty->flags & (1 << TTY_IO_ERROR)))
		return -EIO;

	/* This check not only needs to be done before reading, but also
	   whenever read_chan() gets woken up after sleeping, so I've
	   moved it to there.  This should only be done for the N_TTY
	   line discipline, anyway.  Same goes for write_chan(). -- jlc. */
#if 0
	if ((inode->i_rdev != CONSOLE_DEV) && /* don't stop on /dev/console */
	    (tty->pgrp > 0) &&
	    (current->tty == dev) &&
	    (tty->pgrp != current->pgrp))
		if (is_ignored(SIGTTIN) || is_orphaned_pgrp(current->pgrp))
			return -EIO;
		else {
			(void) kill_pg(current->pgrp, SIGTTIN, 1);
			return -ERESTARTSYS;
		}
#endif
	if (ldiscs[tty->disc].read)
		/* XXX casts are for what kernel-wide prototypes should be. */
		i = (ldiscs[tty->disc].read)(tty,file,(unsigned char *)buf,(unsigned int)count);
	else
		i = -EIO;
	if (i > 0)
		inode->i_atime = CURRENT_TIME;
	return i;
}

static int tty_write(struct inode * inode, struct file * file, char * buf, int count)
{
	int dev, i, is_console;
	struct tty_struct * tty;

	dev = file->f_rdev;
	is_console = (inode->i_rdev == CONSOLE_DEV);
	if (MAJOR(dev) != TTY_MAJOR) {
		printk("tty_write: pseudo-major != TTY_MAJOR\n");
		return -EINVAL;
	}
	dev = MINOR(dev);
	if (is_console && redirect)
		tty = redirect;
	else
		tty = TTY_TABLE(dev);
	if (!tty || !tty->write || (tty->flags & (1 << TTY_IO_ERROR)))
		return -EIO;
#if 0
	if (!is_console && L_TOSTOP(tty) && (tty->pgrp > 0) &&
	    (current->tty == dev) && (tty->pgrp != current->pgrp)) {
		if (is_orphaned_pgrp(current->pgrp))
			return -EIO;
		if (!is_ignored(SIGTTOU)) {
			(void) kill_pg(current->pgrp, SIGTTOU, 1);
			return -ERESTARTSYS;
		}
	}
#endif
	if (ldiscs[tty->disc].write)
		/* XXX casts are for what kernel-wide prototypes should be. */
		i = (ldiscs[tty->disc].write)(tty,file,(unsigned char *)buf,(unsigned int)count);
	else
		i = -EIO;
	if (i > 0)
		inode->i_mtime = CURRENT_TIME;
	return i;
}

/*
 * This is so ripe with races that you should *really* not touch this
 * unless you know exactly what you are doing. All the changes have to be
 * made atomically, or there may be incorrect pointers all over the place.
 */
static int init_dev(int dev)
{
	struct tty_struct *tty, *o_tty;
	struct termios *tp, *o_tp, *ltp, *o_ltp;
	int retval;
	int o_dev;

	o_dev = PTY_OTHER(dev);
	tty = o_tty = NULL;
	tp = o_tp = NULL;
	ltp = o_ltp = NULL;
repeat:
	retval = -EAGAIN;
	if (IS_A_PTY_MASTER(dev) && tty_table[dev] && tty_table[dev]->count)
		goto end_init;
	retval = -ENOMEM;
	if (!tty_table[dev] && !tty) {
		if (!(tty = (struct tty_struct*) get_free_page(GFP_KERNEL)))
			goto end_init;
		initialize_tty_struct(dev, tty);
		goto repeat;
	}
	if (!tty_termios[dev] && !tp) {
		tp = (struct termios *) kmalloc(sizeof(struct termios),
						GFP_KERNEL);
		if (!tp)
			goto end_init;
		initialize_termios(dev, tp);
		goto repeat;
	}
	if (!termios_locked[dev] && !ltp) {
		ltp = (struct termios *) kmalloc(sizeof(struct termios),
						 GFP_KERNEL);
		if (!ltp)
			goto end_init;
		memset(ltp, 0, sizeof(struct termios));
		goto repeat;
	}
	if (IS_A_PTY(dev)) {
		if (!tty_table[o_dev] && !o_tty) {
			o_tty = (struct tty_struct *)
				get_free_page(GFP_KERNEL);
			if (!o_tty)
				goto end_init;
			initialize_tty_struct(o_dev, o_tty);
			goto repeat;
		}
		if (!tty_termios[o_dev] && !o_tp) {
			o_tp = (struct termios *)
				kmalloc(sizeof(struct termios), GFP_KERNEL);
			if (!o_tp)
				goto end_init;
			initialize_termios(o_dev, o_tp);
			goto repeat;
		}
		if (!termios_locked[o_dev] && !o_ltp) {
			o_ltp = (struct termios *)
				kmalloc(sizeof(struct termios), GFP_KERNEL);
			if (!o_ltp)
				goto end_init;
			memset(o_ltp, 0, sizeof(struct termios));
			goto repeat;
		}
		
	}
	/* Now we have allocated all the structures: update all the pointers.. */
	if (!tty_termios[dev]) {
		tty_termios[dev] = tp;
		tp = NULL;
	}
	if (!tty_table[dev]) {
		tty->termios = tty_termios[dev];
		tty_table[dev] = tty;
		tty = NULL;
	}
	if (!termios_locked[dev]) {
		termios_locked[dev] = ltp;
		ltp = NULL;
	}
	if (IS_A_PTY(dev)) {
		if (!tty_termios[o_dev]) {
			tty_termios[o_dev] = o_tp;
			o_tp = NULL;
		}
		if (!termios_locked[o_dev]) {
			termios_locked[o_dev] = o_ltp;
			o_ltp = NULL;
		}
		if (!tty_table[o_dev]) {
			o_tty->termios = tty_termios[o_dev];
			tty_table[o_dev] = o_tty;
			o_tty = NULL;
		}
		tty_table[dev]->link = tty_table[o_dev];
		tty_table[o_dev]->link = tty_table[dev];
	}
	tty_table[dev]->count++;
	if (IS_A_PTY_MASTER(dev))
		tty_table[o_dev]->count++;
	retval = 0;
end_init:
	if (tty)
		free_page((unsigned long) tty);
	if (o_tty)
		free_page((unsigned long) o_tty);
	if (tp)
		kfree_s(tp, sizeof(struct termios));
	if (o_tp)
		kfree_s(o_tp, sizeof(struct termios));
	if (ltp)
		kfree_s(ltp, sizeof(struct termios));
	if (o_ltp)
		kfree_s(o_ltp, sizeof(struct termios));
	return retval;
}

/*
 * Even releasing the tty structures is a tricky business.. We have
 * to be very careful that the structures are all released at the
 * same time, as interrupts might otherwise get the wrong pointers.
 */
static void release_dev(int dev, struct file * filp)
{
	struct tty_struct *tty, *o_tty;
	struct termios *tp, *o_tp;
	struct task_struct **p;

	tty = tty_table[dev];
	tp = tty_termios[dev];
	o_tty = NULL;
	o_tp = NULL;
	if (!tty) {
		printk("release_dev: tty_table[%d] was NULL\n", dev);
		return;
	}
	if (!tp) {
		printk("release_dev: tty_termios[%d] was NULL\n", dev);
		return;
	}
#ifdef TTY_DEBUG_HANGUP
	printk("release_dev of tty%d (tty count=%d)...", dev, tty->count);
#endif
	if (IS_A_PTY(dev)) {
		o_tty = tty_table[PTY_OTHER(dev)];
		o_tp = tty_termios[PTY_OTHER(dev)];
		if (!o_tty) {
			printk("release_dev: pty pair(%d) was NULL\n", dev);
			return;
		}
		if (!o_tp) {
			printk("release_dev: pty pair(%d) termios was NULL\n", dev);
			return;
		}
		if (tty->link != o_tty || o_tty->link != tty) {
			printk("release_dev: bad pty pointers\n");
			return;
		}
	}
	tty->write_data_cnt = 0; /* Clear out pending trash */
	if (tty->close)
		tty->close(tty, filp);
	if (IS_A_PTY_MASTER(dev)) {
		if (--tty->link->count < 0) {
			printk("release_dev: bad tty slave count (dev = %d): %d\n",
			       dev, tty->count);
			tty->link->count = 0;
		}
	}
	if (--tty->count < 0) {
		printk("release_dev: bad tty_table[%d]->count: %d\n",
		       dev, tty->count);
		tty->count = 0;
	}
	if (tty->count)
		return;
	
#ifdef TTY_DEBUG_HANGUP
	printk("freeing tty structure...");
#endif

	/*
	 * Make sure there aren't any processes that still think this
	 * tty is their controlling tty.
	 */
	for (p = &LAST_TASK ; p > &FIRST_TASK ; --p) {
		if ((*p) && (*p)->tty == tty->line)
		(*p)->tty = -1;
	}

	/*
	 * Shutdown the current line discipline, and reset it to
	 * N_TTY.
	 */
	if (ldiscs[tty->disc].close != NULL)
		ldiscs[tty->disc].close(tty);
	tty->disc = N_TTY;
	tty->termios->c_line = N_TTY;
	
	if (o_tty) {
		if (o_tty->count)
			return;
		else {
			tty_table[PTY_OTHER(dev)] = NULL;
			tty_termios[PTY_OTHER(dev)] = NULL;
		}
	}
	tty_table[dev] = NULL;
	if (IS_A_PTY(dev)) {
		tty_termios[dev] = NULL;
		kfree_s(tp, sizeof(struct termios));
	}
	if (tty == redirect || o_tty == redirect)
		redirect = NULL;
	free_page((unsigned long) tty);
	if (o_tty)
		free_page((unsigned long) o_tty);
	if (o_tp)
		kfree_s(o_tp, sizeof(struct termios));
}

/*
 * tty_open and tty_release keep up the tty count that contains the
 * number of opens done on a tty. We cannot use the inode-count, as
 * different inodes might point to the same tty.
 *
 * Open-counting is needed for pty masters, as well as for keeping
 * track of serial lines: DTR is dropped when the last close happens.
 * (This is not done solely through tty->count, now.  - Ted 1/27/92)
 *
 * The termios state of a pty is reset on first open so that
 * settings don't persist across reuse.
 */
static int tty_open(struct inode * inode, struct file * filp)
{
	struct tty_struct *tty;
	int major, minor;
	int noctty, retval;

retry_open:
	minor = MINOR(inode->i_rdev);
	major = MAJOR(inode->i_rdev);
	noctty = filp->f_flags & O_NOCTTY;
	if (major == TTYAUX_MAJOR) {
		if (!minor) {
			major = TTY_MAJOR;
			minor = current->tty;
		}
		/* noctty = 1; */
	} else if (major == TTY_MAJOR) {
		if (!minor) {
			minor = fg_console + 1;
			noctty = 1;
		}
	} else {
		printk("Bad major #%d in tty_open\n", MAJOR(inode->i_rdev));
		return -ENODEV;
	}
	if (minor <= 0)
		return -ENXIO;
	if (IS_A_PTY_MASTER(minor))
		noctty = 1;
	filp->f_rdev = (major << 8) | minor;
	retval = init_dev(minor);
	if (retval)
		return retval;
	tty = tty_table[minor];
#ifdef TTY_DEBUG_HANGUP
	printk("opening tty%d...", tty->line);
#endif
	if (test_bit(TTY_EXCLUSIVE, &tty->flags) && !suser())
		return -EBUSY;

#if 0
	/* clean up the packet stuff. */
	/*
	 *  Why is this not done in init_dev?  Right here, if another 
	 * process opens up a tty in packet mode, all the packet 
	 * variables get cleared.  Come to think of it, is anything 
	 * using the packet mode at all???  - Ted, 1/27/93
	 *
	 * Not to worry, a pty master can only be opened once.
	 * And rlogind and telnetd both use packet mode.  -- jrs
	 *
	 * Not needed.  These are cleared in initialize_tty_struct. -- jlc
	 */
	tty->ctrl_status = 0;
	tty->packet = 0;
#endif

	if (tty->open) {
		retval = tty->open(tty, filp);
	} else {
		retval = -ENODEV;
	}
	if (retval) {
#ifdef TTY_DEBUG_HANGUP
		printk("error %d in opening tty%d...", retval, tty->line);
#endif

		release_dev(minor, filp);
		if (retval != -ERESTARTSYS)
			return retval;
		if (current->signal & ~current->blocked)
			return retval;
		schedule();
		goto retry_open;
	}
	if (!noctty &&
	    current->leader &&
	    current->tty<0 &&
	    tty->session==0) {
		current->tty = minor;
		tty->session = current->session;
		tty->pgrp = current->pgrp;
	}
	filp->f_rdev = MKDEV(TTY_MAJOR,minor); /* Set it to something normal */
	return 0;
}

/*
 * Note that releasing a pty master also releases the child, so
 * we have to make the redirection checks after that and on both
 * sides of a pty.
 */
static void tty_release(struct inode * inode, struct file * filp)
{
	int dev;

	dev = filp->f_rdev;
	if (MAJOR(dev) != TTY_MAJOR) {
		printk("tty_release: tty pseudo-major != TTY_MAJOR\n");
		return;
	}
	dev = MINOR(filp->f_rdev);
	if (!dev) {
		printk("tty_release: bad f_rdev\n");
		return;
	}
	release_dev(dev, filp);
}

static int tty_select(struct inode * inode, struct file * filp, int sel_type, select_table * wait)
{
	int dev;
	struct tty_struct * tty;

	dev = filp->f_rdev;
	if (MAJOR(dev) != TTY_MAJOR) {
		printk("tty_select: tty pseudo-major != TTY_MAJOR\n");
		return 0;
	}
	dev = MINOR(filp->f_rdev);
	tty = TTY_TABLE(dev);
	if (!tty) {
		printk("tty_select: tty struct for dev %d was NULL\n", dev);
		return 0;
	}
	if (ldiscs[tty->disc].select)
		return (ldiscs[tty->disc].select)(tty, inode, filp,
						  sel_type, wait);
	return 0;
}

static int normal_select(struct tty_struct * tty, struct inode * inode,
			 struct file * file, int sel_type, select_table *wait)
{
	switch (sel_type) {
		case SEL_IN:
			if (input_available_p(tty))
				return 1;
			/* fall through */
		case SEL_EX:
			if (tty->packet && tty->link->ctrl_status)
				return 1;
			if (tty->flags & (1 << TTY_SLAVE_CLOSED))
				return 1;
			if (tty_hung_up_p(file))
				return 1;
			select_wait(&tty->secondary.proc_list, wait);
			return 0;
		case SEL_OUT:
			if (LEFT(&tty->write_q) > WAKEUP_CHARS)
				return 1;
			select_wait(&tty->write_q.proc_list, wait);
			return 0;
	}
	return 0;
}

/*
 * This implements the "Secure Attention Key" ---  the idea is to
 * prevent trojan horses by killing all processes associated with this
 * tty when the user hits the "Secure Attention Key".  Required for
 * super-paranoid applications --- see the Orange Book for more details.
 * 
 * This code could be nicer; ideally it should send a HUP, wait a few
 * seconds, then send a INT, and then a KILL signal.  But you then
 * have to coordinate with the init process, since all processes associated
 * with the current tty must be dead before the new getty is allowed
 * to spawn.
 */
void do_SAK( struct tty_struct *tty)
{
#ifdef TTY_SOFT_SAK
	tty_hangup(tty);
#else
	struct task_struct **p;
	int line = tty->line;
	int session = tty->session;
	int		i;
	struct file	*filp;
	
	flush_input(tty);
	flush_output(tty);
 	for (p = &LAST_TASK ; p > &FIRST_TASK ; --p) {
		if (!(*p))
			continue;
		if (((*p)->tty == line) ||
		    ((session > 0) && ((*p)->session == session)))
			send_sig(SIGKILL, *p, 1);
		else {
			for (i=0; i < NR_OPEN; i++) {
				filp = (*p)->filp[i];
				if (filp && (filp->f_op == &tty_fops) &&
				    (MINOR(filp->f_rdev) == line)) {
					send_sig(SIGKILL, *p, 1);
					break;
				}
			}
		}
	}
#endif
}

/*
 * This routine allows a kernel routine to send a large chunk of data
 * to a particular tty; if all of the data can be queued up for ouput
 * immediately, tty_write_data() will return 0.  If, however, not all
 * of the data can be immediately queued for delivery, the number of
 * bytes left to be queued up will be returned, and the rest of the
 * data will be queued up when there is room.  The callback function
 * will be called (with the argument callarg) when the last of the
 * data is finally in the queue.
 *
 * Note that the callback routine will _not_ be called if all of the
 * data could be queued immediately.  This is to avoid a problem with
 * the kernel stack getting too deep, which might happen if the
 * callback routine calls tty_write_data with itself as an argument.
 */
int tty_write_data(struct tty_struct *tty, char *bufp, int buflen,
		    void (*callback)(void * data), void * callarg)
{
	int head, tail, count;
	unsigned long flags;
	char *p;

#define VLEFT ((tail-head-1)&(TTY_BUF_SIZE-1))

	save_flags(flags);
	cli();
	if (tty->write_data_cnt) {
		restore_flags(flags);
		return -EBUSY;
	}

	head = tty->write_q.head;
	tail = tty->write_q.tail;
	count = buflen;
	p = bufp;

	while (count && VLEFT > 0) {
		tty->write_q.buf[head++] = *p++;
		head &= TTY_BUF_SIZE-1;
		count--;
	}
	tty->write_q.head = head;
	if (count) {
		tty->write_data_cnt = count;
		tty->write_data_ptr = (unsigned char *) p;
		tty->write_data_callback = callback;
		tty->write_data_arg = callarg;
	}
	restore_flags(flags);
	tty->write(tty);
	return count;
}

/*
 * This routine routine is called after an interrupt has drained a
 * tty's write queue, so that there is more space for data waiting to
 * be sent in tty->write_data_ptr.
 *
 * tty_check_write[8] is a bitstring which indicates which ttys
 * needs to be processed.
 */
void tty_bh_routine(void * unused)
{
	int	i, j, line, mask;
	int	head, tail, count;
	unsigned char * p;
	struct tty_struct * tty;

	for (i = 0, line = 0; i < MAX_TTYS / 32; i++) {
		if (!tty_check_write[i]) {
			line += 32;
			continue;
		}
		for (j=0, mask=0; j < 32; j++, line++, mask <<= 1) {
			if (clear_bit(j, &tty_check_write[i])) {
				tty = tty_table[line];
				if (!tty || !tty->write_data_cnt)
					continue;
				cli();
				head = tty->write_q.head;
				tail = tty->write_q.tail;
				count = tty->write_data_cnt;
				p = tty->write_data_ptr;

				while (count && VLEFT > 0) {
					tty->write_q.buf[head++] = *p++;
					head &= TTY_BUF_SIZE-1;
					count--;
				}
				tty->write_q.head = head;
				tty->write_data_ptr = p;
				tty->write_data_cnt = count;
				sti();
				if (!count)
					(tty->write_data_callback)
						(tty->write_data_arg);
			}
		}
	}
	
}

/*
 * This subroutine initializes a tty structure.  We have to set up
 * things correctly for each different type of tty.
 */
static void initialize_tty_struct(int line, struct tty_struct *tty)
{
	memset(tty, 0, sizeof(struct tty_struct));
	tty->line = line;
	tty->disc = N_TTY;
	tty->pgrp = -1;
	if (IS_A_CONSOLE(line)) {
		tty->open = con_open;
		tty->winsize.ws_row = video_num_lines;
		tty->winsize.ws_col = video_num_columns;
	} else if IS_A_SERIAL(line) {
		tty->open = rs_open;
	} else if IS_A_PTY(line) {
		tty->open = pty_open;
	}
}

static void initialize_termios(int line, struct termios * tp)
{
	memset(tp, 0, sizeof(struct termios));
	memcpy(tp->c_cc, INIT_C_CC, NCCS);
	if (IS_A_CONSOLE(line) || IS_A_PTY_SLAVE(line)) {
		tp->c_iflag = ICRNL | IXON;
		tp->c_oflag = OPOST | ONLCR;
		tp->c_cflag = B38400 | CS8 | CREAD;
		tp->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK |
			ECHOCTL | ECHOKE | IEXTEN;
	} else if (IS_A_SERIAL(line)) {
		tp->c_iflag = ICRNL | IXON;
		tp->c_oflag = OPOST | ONLCR | XTABS;
		tp->c_cflag = B9600 | CS8 | CREAD | HUPCL | CLOCAL;
		tp->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK |
			ECHOCTL | ECHOKE | IEXTEN;
	} else if (IS_A_PTY_MASTER(line))
		tp->c_cflag = B9600 | CS8 | CREAD;
}

static struct tty_ldisc tty_ldisc_N_TTY = {
	0,			/* flags */
	NULL,			/* open */
	NULL,			/* close */
	read_chan,		/* read */
	write_chan,		/* write */
	NULL,			/* ioctl */
	normal_select,		/* select */
	copy_to_cooked		/* handler */
};

	
long tty_init(long kmem_start)
{
	int i;

	if (sizeof(struct tty_struct) > PAGE_SIZE)
		panic("size of tty structure > PAGE_SIZE!");
	if (register_chrdev(TTY_MAJOR,"tty",&tty_fops))
		panic("unable to get major %d for tty device", TTY_MAJOR);
	if (register_chrdev(TTYAUX_MAJOR,"tty",&tty_fops))
		panic("unable to get major %d for tty device", TTYAUX_MAJOR);
	for (i=0 ; i< MAX_TTYS ; i++) {
		tty_table[i] =  0;
		tty_termios[i] = 0;
	}
	memset(tty_check_write, 0, sizeof(tty_check_write));
	bh_base[TTY_BH].routine = tty_bh_routine;

	/* Setup the default TTY line discipline. */
	memset(ldiscs, 0, sizeof(ldiscs));
	(void) tty_register_ldisc(N_TTY, &tty_ldisc_N_TTY);

	kmem_start = kbd_init(kmem_start);
	kmem_start = con_init(kmem_start);
	kmem_start = rs_init(kmem_start);
	return kmem_start;
}
