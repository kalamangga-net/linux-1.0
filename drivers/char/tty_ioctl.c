/*
 *  linux/kernel/drivers/char/tty_ioctl.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 * Modified by Fred N. van Kempen, 01/29/93, to add line disciplines
 * which can be dynamically activated and de-activated by the line
 * discipline handling modules (like SLIP).
 */

#include <linux/types.h>
#include <linux/termios.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/tty.h>
#include <linux/fcntl.h>
#include <linux/string.h>

#include <asm/io.h>
#include <asm/bitops.h>
#include <asm/segment.h>
#include <asm/system.h>

#undef	DEBUG
#ifdef DEBUG
# define	PRINTK(x)	printk (x)
#else
# define	PRINTK(x)	/**/
#endif

extern int session_of_pgrp(int pgrp);
extern int do_screendump(int arg);
extern int kill_pg(int pgrp, int sig, int priv);

#ifdef CONFIG_SELECTION
extern int set_selection(const int arg);
extern int paste_selection(struct tty_struct *tty);
#endif /* CONFIG_SELECTION */

static int tty_set_ldisc(struct tty_struct *tty, int ldisc);

void flush_input(struct tty_struct * tty)
{
	cli();
	tty->read_q.head = tty->read_q.tail = 0;
	tty->secondary.head = tty->secondary.tail = 0;
	tty->canon_head = tty->canon_data = tty->erasing = 0;
	memset(&tty->readq_flags, 0, sizeof tty->readq_flags);
	memset(&tty->secondary_flags, 0, sizeof tty->secondary_flags);
	sti();
	if (!tty->link)
		return;
	/* No cli() since ptys don't use interrupts. */
	tty->link->write_q.head = tty->link->write_q.tail = 0;
	wake_up_interruptible(&tty->link->write_q.proc_list);
	if (tty->link->packet) {
		tty->ctrl_status |= TIOCPKT_FLUSHREAD;
		wake_up_interruptible(&tty->link->secondary.proc_list);
	}
}

void flush_output(struct tty_struct * tty)
{
	cli();
	tty->write_q.head = tty->write_q.tail = 0;
	sti();
	wake_up_interruptible(&tty->write_q.proc_list);
	if (!tty->link)
		return;
	/* No cli() since ptys don't use interrupts. */
	tty->link->read_q.head = tty->link->read_q.tail = 0;
	tty->link->secondary.head = tty->link->secondary.tail = 0;
	tty->link->canon_head = tty->link->canon_data = tty->link->erasing = 0;
	memset(&tty->link->readq_flags, 0, sizeof tty->readq_flags);
	memset(&tty->link->secondary_flags, 0, sizeof tty->secondary_flags);
	if (tty->link->packet) {
		tty->ctrl_status |= TIOCPKT_FLUSHWRITE;
		wake_up_interruptible(&tty->link->secondary.proc_list);
	}
}

void wait_until_sent(struct tty_struct * tty, int timeout)
{
	struct wait_queue wait = { current, NULL };

	TTY_WRITE_FLUSH(tty);
	if (EMPTY(&tty->write_q))
		return;
	add_wait_queue(&tty->write_q.proc_list, &wait);
	current->counter = 0;	/* make us low-priority */
	if (timeout)
		current->timeout = timeout + jiffies;
	else
		current->timeout = (unsigned) -1;
	do {
		current->state = TASK_INTERRUPTIBLE;
		if (current->signal & ~current->blocked)
			break;
		TTY_WRITE_FLUSH(tty);
		if (EMPTY(&tty->write_q))
			break;
		schedule();
	} while (current->timeout);
	current->state = TASK_RUNNING;
	remove_wait_queue(&tty->write_q.proc_list, &wait);
}

static int do_get_ps_info(int arg)
{
	struct tstruct {
		int flag;
		int present[NR_TASKS];
		struct task_struct tasks[NR_TASKS];
	};
	struct tstruct *ts = (struct tstruct *)arg;
	struct task_struct **p;
	char *c, *d;
	int i, n = 0;
	
	i = verify_area(VERIFY_WRITE, (void *)arg, sizeof(struct tstruct));
	if (i)
		return i;
	for (p = &FIRST_TASK ; p <= &LAST_TASK ; p++, n++)
		if (*p)
		{
			c = (char *)(*p);
			d = (char *)(ts->tasks+n);
			for (i=0 ; i<sizeof(struct task_struct) ; i++)
				put_fs_byte(*c++, d++);
			put_fs_long(1, (unsigned long *)(ts->present+n));
		}
		else	
			put_fs_long(0, (unsigned long *)(ts->present+n));
	return(0);			
}

static void unset_locked_termios(struct termios *termios,
				 struct termios *old,
				 struct termios *locked)
{
	int	i;
	
#define NOSET_MASK(x,y,z) (x = ((x) & ~(z)) | ((y) & (z)))

	if (!locked) {
		printk("Warning?!? termios_locked is NULL.\n");
		return;
	}

	NOSET_MASK(termios->c_iflag, old->c_iflag, locked->c_iflag);
	NOSET_MASK(termios->c_oflag, old->c_oflag, locked->c_oflag);
	NOSET_MASK(termios->c_cflag, old->c_cflag, locked->c_cflag);
	NOSET_MASK(termios->c_lflag, old->c_lflag, locked->c_lflag);
	termios->c_line = locked->c_line ? old->c_line : termios->c_line;
	for (i=0; i < NCCS; i++)
		termios->c_cc[i] = locked->c_cc[i] ?
			old->c_cc[i] : termios->c_cc[i];
}

int check_change(struct tty_struct * tty, int channel)
{
	/* If we try to set the state of terminal and we're not in the
	   foreground, send a SIGTTOU.  If the signal is blocked or
	   ignored, go ahead and perform the operation.  POSIX 7.2) */
	if (current->tty != channel)
		return 0;
	if (tty->pgrp <= 0) {
		printk("check_change: tty->pgrp <= 0!\n");
		return 0;
	}
	if (current->pgrp == tty->pgrp)
		return 0;
	if (is_ignored(SIGTTOU))
		return 0;
	if (is_orphaned_pgrp(current->pgrp))
		return -EIO;
	(void) kill_pg(current->pgrp,SIGTTOU,1);
	return -ERESTARTSYS;
}

static int set_termios_2(struct tty_struct * tty, struct termios * termios)
{
	struct termios old_termios = *tty->termios;
	int canon_change;

	canon_change = (old_termios.c_lflag ^ termios->c_lflag) & ICANON;
	cli();
	*tty->termios = *termios;
	if (canon_change) {
		memset(&tty->secondary_flags, 0, sizeof tty->secondary_flags);
		tty->canon_head = tty->secondary.tail;
		tty->canon_data = 0;
		tty->erasing = 0;
	}
	sti();
	if (canon_change && !L_ICANON(tty) && !EMPTY(&tty->secondary))
		/* Get characters left over from canonical mode. */
		wake_up_interruptible(&tty->secondary.proc_list);

	/* see if packet mode change of state */

	if (tty->link && tty->link->packet) {
		int old_flow = ((old_termios.c_iflag & IXON) &&
				(old_termios.c_cc[VSTOP] == '\023') &&
				(old_termios.c_cc[VSTART] == '\021'));
		int new_flow = (I_IXON(tty) &&
				STOP_CHAR(tty) == '\023' &&
				START_CHAR(tty) == '\021');
		if (old_flow != new_flow) {
			tty->ctrl_status &= ~(TIOCPKT_DOSTOP | TIOCPKT_NOSTOP);
			if (new_flow)
				tty->ctrl_status |= TIOCPKT_DOSTOP;
			else
				tty->ctrl_status |= TIOCPKT_NOSTOP;
			wake_up_interruptible(&tty->link->secondary.proc_list);
		}
	}

	unset_locked_termios(tty->termios, &old_termios,
			     termios_locked[tty->line]);

	if (tty->set_termios)
		(*tty->set_termios)(tty, &old_termios);

	return 0;
}

static int set_termios(struct tty_struct * tty, struct termios * termios,
		       int channel)
{
	struct termios tmp_termios;

	memcpy_fromfs(&tmp_termios, termios, sizeof (struct termios));
	return set_termios_2(tty, &tmp_termios);
}

static int get_termio(struct tty_struct * tty, struct termio * termio)
{
	int i;
	struct termio tmp_termio;

	i = verify_area(VERIFY_WRITE, termio, sizeof (struct termio));
	if (i)
		return i;
	tmp_termio.c_iflag = tty->termios->c_iflag;
	tmp_termio.c_oflag = tty->termios->c_oflag;
	tmp_termio.c_cflag = tty->termios->c_cflag;
	tmp_termio.c_lflag = tty->termios->c_lflag;
	tmp_termio.c_line = tty->termios->c_line;
	for(i=0 ; i < NCC ; i++)
		tmp_termio.c_cc[i] = tty->termios->c_cc[i];
	memcpy_tofs(termio, &tmp_termio, sizeof (struct termio));
	return 0;
}

static int set_termio(struct tty_struct * tty, struct termio * termio,
		      int channel)
{
	struct termio tmp_termio;
	struct termios tmp_termios;

	tmp_termios = *tty->termios;
	memcpy_fromfs(&tmp_termio, termio, sizeof (struct termio));

#define SET_LOW_BITS(x,y)	((x) = (0xffff0000 & (x)) | (y))

	SET_LOW_BITS(tmp_termios.c_iflag, tmp_termio.c_iflag);
	SET_LOW_BITS(tmp_termios.c_oflag, tmp_termio.c_oflag);
	SET_LOW_BITS(tmp_termios.c_cflag, tmp_termio.c_cflag);
	SET_LOW_BITS(tmp_termios.c_lflag, tmp_termio.c_lflag);
	memcpy(&tmp_termios.c_cc, &tmp_termio.c_cc, NCC);

#undef SET_LOW_BITS

	return set_termios_2(tty, &tmp_termios);
}

static int set_window_size(struct tty_struct * tty, struct winsize * ws)
{
	struct winsize tmp_ws;

	memcpy_fromfs(&tmp_ws, ws, sizeof (struct winsize));
	if (memcmp(&tmp_ws, &tty->winsize, sizeof (struct winsize)) &&
	    tty->pgrp > 0)
		kill_pg(tty->pgrp, SIGWINCH, 1);
	tty->winsize = tmp_ws;
	return 0;
}

/* Set the discipline of a tty line. */
static int tty_set_ldisc(struct tty_struct *tty, int ldisc)
{
	if ((ldisc < N_TTY) || (ldisc >= NR_LDISCS) ||
	    !(ldiscs[ldisc].flags & LDISC_FLAG_DEFINED))
		return -EINVAL;

	if (tty->disc == ldisc)
		return 0;	/* We are already in the desired discipline */

	/* Shutdown the current discipline. */
	wait_until_sent(tty, 0);
	flush_input(tty);
	if (ldiscs[tty->disc].close)
		ldiscs[tty->disc].close(tty);

	/* Now set up the new line discipline. */
	tty->disc = ldisc;
	tty->termios->c_line = ldisc;
	if (ldiscs[tty->disc].open)
		return(ldiscs[tty->disc].open(tty));
	else
		return 0;
}

static unsigned long inq_canon(struct tty_struct * tty)
{
	int nr, head, tail;

	if (!tty->canon_data)
		return 0;
	head = tty->canon_head;
	tail = tty->secondary.tail;
	nr = (head - tail) & (TTY_BUF_SIZE-1);
	/* Skip EOF-chars.. */
	while (head != tail) {
		if (test_bit(tail, &tty->secondary_flags) &&
		    tty->secondary.buf[tail] == __DISABLED_CHAR)
			nr--;
		INC(tail);
	}
	return nr;
}

int tty_ioctl(struct inode * inode, struct file * file,
	unsigned int cmd, unsigned long arg)
{
	struct tty_struct * tty;
	struct tty_struct * other_tty;
	struct tty_struct * termios_tty;
	pid_t pgrp;
	int dev;
	int termios_dev;
	int retval;

	if (MAJOR(file->f_rdev) != TTY_MAJOR) {
		printk("tty_ioctl: tty pseudo-major != TTY_MAJOR\n");
		return -EINVAL;
	}
	dev = MINOR(file->f_rdev);
	tty = TTY_TABLE(dev);
	if (!tty)
		return -EINVAL;
	if (IS_A_PTY(dev))
		other_tty = tty_table[PTY_OTHER(dev)];
	else
		other_tty = NULL;
	if (IS_A_PTY_MASTER(dev)) {
		termios_tty = other_tty;
		termios_dev = PTY_OTHER(dev);
	} else {
		termios_tty = tty;
		termios_dev = dev;
	}
	switch (cmd) {
		case TCGETS:
			retval = verify_area(VERIFY_WRITE, (void *) arg,
					     sizeof (struct termios));
			if (retval)
				return retval;
			memcpy_tofs((struct termios *) arg,
				    termios_tty->termios,
				    sizeof (struct termios));
			return 0;
		case TCSETSF:
		case TCSETSW:
		case TCSETS:
			retval = check_change(termios_tty, termios_dev);
			if (retval)
				return retval;
			if (cmd == TCSETSF || cmd == TCSETSW) {
				if (cmd == TCSETSF)
					flush_input(termios_tty);
				wait_until_sent(termios_tty, 0);
			}
			return set_termios(termios_tty, (struct termios *) arg,
					   termios_dev);
		case TCGETA:
			return get_termio(termios_tty,(struct termio *) arg);
		case TCSETAF:
		case TCSETAW:
		case TCSETA:
			retval = check_change(termios_tty, termios_dev);
			if (retval)
				return retval;
			if (cmd == TCSETAF || cmd == TCSETAW) {
				if (cmd == TCSETAF)
					flush_input(termios_tty);
				wait_until_sent(termios_tty, 0);
			}
			return set_termio(termios_tty, (struct termio *) arg,
					  termios_dev);
		case TCXONC:
			retval = check_change(tty, dev);
			if (retval)
				return retval;
			switch (arg) {
			case TCOOFF:
				stop_tty(tty);
				break;
			case TCOON:
				start_tty(tty);
				break;
			case TCIOFF:
				if (STOP_CHAR(tty) != __DISABLED_CHAR)
					put_tty_queue(STOP_CHAR(tty),
						      &tty->write_q);
				break;
			case TCION:
				if (START_CHAR(tty) != __DISABLED_CHAR)
					put_tty_queue(START_CHAR(tty),
						      &tty->write_q);
				break;
			default:
				return -EINVAL;
			}
			return 0;
		case TCFLSH:
			retval = check_change(tty, dev);
			if (retval)
				return retval;
			switch (arg) {
			case TCIFLUSH:
				flush_input(tty);
				break;
			case TCIOFLUSH:
				flush_input(tty);
				/* fall through */
			case TCOFLUSH:
				flush_output(tty);
				break;
			default:
				return -EINVAL;
			}
			return 0;
		case TIOCEXCL:
			set_bit(TTY_EXCLUSIVE, &tty->flags);
			return 0;
		case TIOCNXCL:
			clear_bit(TTY_EXCLUSIVE, &tty->flags);
			return 0;
		case TIOCSCTTY:
			if (current->leader &&
			    (current->session == tty->session))
				return 0;
			/*
			 * The process must be a session leader and
			 * not have a controlling tty already.
			 */
			if (!current->leader || (current->tty >= 0))
				return -EPERM;
			if (tty->session > 0) {
				/*
				 * This tty is already the controlling
				 * tty for another session group!
				 */
				if ((arg == 1) && suser()) {
					/*
					 * Steal it away
					 */
					struct task_struct *p;

					for_each_task(p)
						if (p->tty == dev)
							p->tty = -1;
				} else
					return -EPERM;
			}
			current->tty = dev;
			tty->session = current->session;
			tty->pgrp = current->pgrp;
			return 0;
		case TIOCGPGRP:
			retval = verify_area(VERIFY_WRITE, (void *) arg,
					     sizeof (pid_t));
			if (retval)
				return retval;
			if (current->tty != termios_dev)
				return -ENOTTY;
			put_fs_long(termios_tty->pgrp, (pid_t *) arg);
			return 0;
		case TIOCSPGRP:
			retval = check_change(termios_tty, termios_dev);
			if (retval)
				return retval;
			if ((current->tty < 0) ||
			    (current->tty != termios_dev) ||
			    (termios_tty->session != current->session))
				return -ENOTTY;
			pgrp = get_fs_long((pid_t *) arg);
			if (pgrp < 0)
				return -EINVAL;
			if (session_of_pgrp(pgrp) != current->session)
				return -EPERM;
			termios_tty->pgrp = pgrp;
			return 0;
		case TIOCOUTQ:
			retval = verify_area(VERIFY_WRITE, (void *) arg,
					     sizeof (unsigned long));
			if (retval)
				return retval;
			put_fs_long(CHARS(&tty->write_q),
				    (unsigned long *) arg);
			return 0;
		case TIOCINQ:
			retval = verify_area(VERIFY_WRITE, (void *) arg,
					     sizeof (unsigned long));
			if (retval)
				return retval;
			if (L_ICANON(tty))
				put_fs_long(inq_canon(tty),
					(unsigned long *) arg);
			else
				put_fs_long(CHARS(&tty->secondary),
					(unsigned long *) arg);
			return 0;
		case TIOCSTI:
			if ((current->tty != dev) && !suser())
				return -EPERM;
			retval = verify_area(VERIFY_READ, (void *) arg, 1);
			if (retval)
				return retval;
			put_tty_queue(get_fs_byte((char *) arg), &tty->read_q);
			TTY_READ_FLUSH(tty);
			return 0;
		case TIOCGWINSZ:
			retval = verify_area(VERIFY_WRITE, (void *) arg,
					     sizeof (struct winsize));
			if (retval)
				return retval;
			memcpy_tofs((struct winsize *) arg, &tty->winsize,
				    sizeof (struct winsize));
			return 0;
		case TIOCSWINSZ:
			if (IS_A_PTY_MASTER(dev))
				set_window_size(other_tty,(struct winsize *) arg);
			return set_window_size(tty,(struct winsize *) arg);
		case TIOCLINUX:
			switch (get_fs_byte((char *)arg))
			{
				case 0: 
					return do_screendump(arg);
				case 1: 
					return do_get_ps_info(arg);
#ifdef CONFIG_SELECTION
				case 2:
					return set_selection(arg);
				case 3:
					return paste_selection(tty);
				case 4:
					unblank_screen();
					return 0;
#endif /* CONFIG_SELECTION */
				default: 
					return -EINVAL;
			}
		case TIOCCONS:
			if (IS_A_CONSOLE(dev)) {
				if (!suser())
					return -EPERM;
				redirect = NULL;
				return 0;
			}
			if (redirect)
				return -EBUSY;
			if (!suser())
				return -EPERM;
			if (IS_A_PTY_MASTER(dev))
				redirect = other_tty;
			else if (IS_A_PTY_SLAVE(dev))
				redirect = tty;
			else
				return -ENOTTY;
			return 0;
		case FIONBIO:
			arg = get_fs_long((unsigned long *) arg);
			if (arg)
				file->f_flags |= O_NONBLOCK;
			else
				file->f_flags &= ~O_NONBLOCK;
			return 0;
		case TIOCNOTTY:
			if (current->tty != dev)
				return -ENOTTY;
			if (current->leader)
				disassociate_ctty(0);
			current->tty = -1;
			return 0;
		case TIOCGETD:
			retval = verify_area(VERIFY_WRITE, (void *) arg,
					     sizeof (unsigned long));
			if (retval)
				return retval;
			put_fs_long(tty->disc, (unsigned long *) arg);
			return 0;
		case TIOCSETD:
			retval = check_change(tty, dev);
			if (retval)
				return retval;
			arg = get_fs_long((unsigned long *) arg);
			return tty_set_ldisc(tty, arg);
		case TIOCGLCKTRMIOS:
			arg = get_fs_long((unsigned long *) arg);
			retval = verify_area(VERIFY_WRITE, (void *) arg,
					     sizeof (struct termios));
			if (retval)
				return retval;
			memcpy_tofs((struct termios *) arg,
				    &termios_locked[termios_dev],
				    sizeof (struct termios));
			return 0;
		case TIOCSLCKTRMIOS:
			if (!suser())
				return -EPERM;
			arg = get_fs_long((unsigned long *) arg);
			memcpy_fromfs(&termios_locked[termios_dev],
				      (struct termios *) arg,
				      sizeof (struct termios));
			return 0;
		case TIOCPKT:
			if (!IS_A_PTY_MASTER(dev))
				return -ENOTTY;
			retval = verify_area(VERIFY_READ, (void *) arg,
					     sizeof (unsigned long));
			if (retval)
				return retval;
			if (get_fs_long(arg)) {
				if (!tty->packet) {
					tty->packet = 1;
					tty->link->ctrl_status = 0;
				}
			} else
				tty->packet = 0;
			return 0;
		case TCSBRK: case TCSBRKP:
			retval = check_change(tty, dev);
			if (retval)
				return retval;
			wait_until_sent(tty, 0);
			if (!tty->ioctl)
				return 0;
			tty->ioctl(tty, file, cmd, arg);
			return 0;
		default:
			if (tty->ioctl) {
				retval = (tty->ioctl)(tty, file, cmd, arg);
				if (retval != -EINVAL)
					return retval;
			}
			if (ldiscs[tty->disc].ioctl) {
				retval = (ldiscs[tty->disc].ioctl)
					(tty, file, cmd, arg);
				return retval;
			}
			return -EINVAL;
	}
}
