/*
 *  linux/kernel/chr_drv/pty.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 *	pty.c
 *
 * This module exports the following pty function:
 * 
 * 	int  pty_open(struct tty_struct * tty, struct file * filp);
 */

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/string.h>

#include <asm/system.h>
#include <asm/bitops.h>

#define MIN(a,b)	((a) < (b) ? (a) : (b))

static void pty_close(struct tty_struct * tty, struct file * filp)
{
	if (!tty)
		return;
	if (IS_A_PTY_MASTER(tty->line)) {
		if (tty->count > 1)
			printk("master pty_close: count = %d!!\n", tty->count);
	} else {
		if (tty->count > 2)
			return;
	}
	wake_up_interruptible(&tty->secondary.proc_list);
	wake_up_interruptible(&tty->read_q.proc_list);
	wake_up_interruptible(&tty->write_q.proc_list);
	if (!tty->link)
		return;
	wake_up_interruptible(&tty->link->secondary.proc_list);
	wake_up_interruptible(&tty->link->read_q.proc_list);
	wake_up_interruptible(&tty->link->write_q.proc_list);
	if (IS_A_PTY_MASTER(tty->line))
		tty_hangup(tty->link);
	else {
		start_tty(tty);
		set_bit(TTY_SLAVE_CLOSED, &tty->link->flags);
	}
}

static inline void pty_copy(struct tty_struct * from, struct tty_struct * to)
{
	unsigned long count, n;
	struct tty_queue *fq, *tq;

	if (from->stopped || EMPTY(&from->write_q))
		return;
	fq = &from->write_q;
	tq = &to->read_q;
	count = MIN(CHARS(fq), LEFT(tq));
	while (count) {
		n = MIN(MIN(TTY_BUF_SIZE - fq->tail, TTY_BUF_SIZE - tq->head),
			count);
		memcpy(&tq->buf[tq->head], &fq->buf[fq->tail], n);
		count -= n;
		fq->tail = (fq->tail + n) & (TTY_BUF_SIZE - 1);
		tq->head = (tq->head + n) & (TTY_BUF_SIZE - 1);
	}
	TTY_READ_FLUSH(to);
	if (LEFT(fq) > WAKEUP_CHARS)
		wake_up_interruptible(&fq->proc_list);
	if (from->write_data_cnt) {
		set_bit(from->line, &tty_check_write);
		mark_bh(TTY_BH);
	}
}

/*
 * This routine gets called when tty_write has put something into
 * the write_queue. It copies the input to the output-queue of its
 * slave.
 */
static void pty_write(struct tty_struct * tty)
{
	if (tty->link)
		pty_copy(tty,tty->link);
}

int pty_open(struct tty_struct *tty, struct file * filp)
{
	if (!tty || !tty->link)
		return -ENODEV;
	if (IS_A_PTY_SLAVE(tty->line))
		clear_bit(TTY_SLAVE_CLOSED, &tty->link->flags);
	tty->write = tty->link->write = pty_write;
	tty->close = tty->link->close = pty_close;
	wake_up_interruptible(&tty->read_q.proc_list);
	if (filp->f_flags & O_NDELAY)
		return 0;
	while (!tty->link->count && !(current->signal & ~current->blocked))
		interruptible_sleep_on(&tty->link->read_q.proc_list);
	if (!tty->link->count)
		return -ERESTARTSYS;
	return 0;
}
