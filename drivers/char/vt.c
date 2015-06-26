/*
 *  kernel/chr_drv/vt.c
 *
 *  Copyright (C) 1992 obz under the linux copyright
 *  Dynamic diacritical handling - aeb@cwi.nl - Dec 1993
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/timer.h>
#include <linux/kernel.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <linux/string.h>

#include <asm/io.h>
#include <asm/segment.h>

#include "kbd_kern.h"
#include "vt_kern.h"
#include "diacr.h"

/*
 * Console (vt and kd) routines, as defined by USL SVR4 manual, and by
 * experimentation and study of X386 SYSV handling.
 *
 * One point of difference: SYSV vt's are /dev/vtX, which X >= 0, and
 * /dev/console is a separate ttyp. Under Linux, /dev/tty0 is /dev/console,
 * and the vc start at /dev/ttyX, X >= 1. We maintain that here, so we will
 * always treat our set of vt as numbered 1..NR_CONSOLES (corresponding to
 * ttys 0..NR_CONSOLES-1). Explicitly naming VT 0 is illegal, but using
 * /dev/tty0 (fg_console) as a target is legal, since an implicit aliasing
 * to the current console is done by the main ioctl code.
 */

struct vt_struct vt_cons[NR_CONSOLES];

asmlinkage int sys_ioperm(unsigned long from, unsigned long num, int on);

extern void compute_shiftstate(void);
extern void change_console(unsigned int new_console);
extern void complete_change_console(unsigned int new_console);
extern int vt_waitactive(void);

/*
 * routines to load custom translation table and EGA/VGA font from console.c
 */
extern int con_set_trans(char * table);
extern int con_get_trans(char * table);
extern int con_set_font(char * fontmap);
extern int con_get_font(char * fontmap);

/*
 * these are the valid i/o ports we're allowed to change. they map all the
 * video ports
 */
#define GPFIRST 0x3b4
#define GPLAST 0x3df
#define GPNUM (GPLAST - GPFIRST + 1)

/*
 * Generates sound of some count for some number of clock ticks
 * [count = 1193180 / frequency]
 *
 * If freq is 0, will turn off sound, else will turn it on for that time.
 * If msec is 0, will return immediately, else will sleep for msec time, then
 * turn sound off.
 *
 * We use the BEEP_TIMER vector since we're using the same method to
 * generate sound, and we'll overwrite any beep in progress. That may
 * be something to fix later, if we like.
 *
 * We also return immediately, which is what was implied within the X
 * comments - KDMKTONE doesn't put the process to sleep.
 */
static void
kd_nosound(unsigned long ignored)
{
	/* disable counter 2 */
	outb(inb_p(0x61)&0xFC, 0x61);
	return;
}

void
kd_mksound(unsigned int count, unsigned int ticks)
{
	static struct timer_list sound_timer = { NULL, NULL, 0, 0, kd_nosound };

	cli();
	del_timer(&sound_timer);
	if (count) {
		/* enable counter 2 */
		outb_p(inb_p(0x61)|3, 0x61);
		/* set command for counter 2, 2 byte write */
		outb_p(0xB6, 0x43);
		/* select desired HZ */
		outb_p(count & 0xff, 0x42);
		outb((count >> 8) & 0xff, 0x42);

		if (ticks) {
			sound_timer.expires = ticks;
			add_timer(&sound_timer);
		}
	} else
		kd_nosound(0);
	sti();
	return;
}

/*
 * We handle the console-specific ioctl's here.  We allow the
 * capability to modify any console, not just the fg_console. 
 */
int vt_ioctl(struct tty_struct *tty, struct file * file,
	     unsigned int cmd, unsigned long arg)
{
	int console, i;
	unsigned char ucval;
	struct kbd_struct * kbd;

	console = tty->line - 1;

	if (console < 0 || console >= NR_CONSOLES)
		return -EINVAL;

	kbd = kbd_table + console;
	switch (cmd) {
	case KIOCSOUND:
		kd_mksound((unsigned int)arg, 0);
		return 0;

	case KDMKTONE:
	{
		unsigned int ticks = HZ * ((arg >> 16) & 0xffff) / 1000;

		/*
		 * Generate the tone for the appropriate number of ticks.
		 * If the time is zero, turn off sound ourselves.
		 */
		kd_mksound(arg & 0xffff, ticks);
		if (ticks == 0)
			kd_nosound(0);
		return 0;
	}

	case KDGKBTYPE:
		/*
		 * this is naive.
		 */
		i = verify_area(VERIFY_WRITE, (void *) arg, sizeof(unsigned char));
		if (!i)
			put_fs_byte(KB_101, (char *) arg);
		return i;

	case KDADDIO:
	case KDDELIO:
		/*
		 * KDADDIO and KDDELIO may be able to add ports beyond what
		 * we reject here, but to be safe...
		 */
		if (arg < GPFIRST || arg > GPLAST)
			return -EINVAL;
		return sys_ioperm(arg, 1, (cmd == KDADDIO)) ? -ENXIO : 0;

	case KDENABIO:
	case KDDISABIO:
		return sys_ioperm(GPFIRST, GPNUM,
				  (cmd == KDENABIO)) ? -ENXIO : 0;

	case KDSETMODE:
		/*
		 * currently, setting the mode from KD_TEXT to KD_GRAPHICS
		 * doesn't do a whole lot. i'm not sure if it should do any
		 * restoration of modes or what...
		 */
		switch (arg) {
		case KD_GRAPHICS:
			break;
		case KD_TEXT0:
		case KD_TEXT1:
			arg = KD_TEXT;
		case KD_TEXT:
			break;
		default:
			return -EINVAL;
		}
		if (vt_cons[console].vc_mode == (unsigned char) arg)
			return 0;
		vt_cons[console].vc_mode = (unsigned char) arg;
		if (console != fg_console)
			return 0;
		/*
		 * explicitly blank/unblank the screen if switching modes
		 */
		if (arg == KD_TEXT)
			unblank_screen();
		else {
			timer_active &= ~(1<<BLANK_TIMER);
			blank_screen();
		}
		return 0;

	case KDGETMODE:
		i = verify_area(VERIFY_WRITE, (void *) arg, sizeof(unsigned long));
		if (!i)
			put_fs_long(vt_cons[console].vc_mode, (unsigned long *) arg);
		return i;

	case KDMAPDISP:
	case KDUNMAPDISP:
		/*
		 * these work like a combination of mmap and KDENABIO.
		 * this could be easily finished.
		 */
		return -EINVAL;

	case KDSKBMODE:
		switch(arg) {
		  case K_RAW:
		        set_vc_kbd_mode(kbd, VC_RAW);
			clr_vc_kbd_mode(kbd, VC_MEDIUMRAW);
			break;
		  case K_MEDIUMRAW:
			clr_vc_kbd_mode(kbd, VC_RAW);
			set_vc_kbd_mode(kbd, VC_MEDIUMRAW);
			break;
		  case K_XLATE:
			clr_vc_kbd_mode(kbd, VC_RAW);
			clr_vc_kbd_mode(kbd, VC_MEDIUMRAW);
			compute_shiftstate();
			break;
		  default:
			return -EINVAL;
		}
		flush_input(tty);
		return 0;

	case KDGKBMODE:
		i = verify_area(VERIFY_WRITE, (void *) arg, sizeof(unsigned long));
		if (!i) {
			ucval = (vc_kbd_mode(kbd, VC_RAW) ? K_RAW :
				 vc_kbd_mode(kbd, VC_MEDIUMRAW) ? K_MEDIUMRAW :
				 K_XLATE);
			put_fs_long(ucval, (unsigned long *) arg);
		}
		return i;

	/* this could be folded into KDSKBMODE, but for compatibility
	   reasons it is not so easy to fold KDGKBMETA into KDGKBMODE */
	case KDSKBMETA:
		switch(arg) {
		  case K_METABIT:
			clr_vc_kbd_mode(kbd, VC_META);
			break;
		  case K_ESCPREFIX:
			set_vc_kbd_mode(kbd, VC_META);
			break;
		  default:
			return -EINVAL;
		}
		return 0;

	case KDGKBMETA:
		i = verify_area(VERIFY_WRITE, (void *) arg, sizeof(unsigned long));
		if (!i) {
			ucval = (vc_kbd_mode(kbd, VC_META) ? K_ESCPREFIX :
				 K_METABIT);
			put_fs_long(ucval, (unsigned long *) arg);
		}
		return i;

	case KDGKBENT:
	{
		struct kbentry * const a = (struct kbentry *)arg;
		u_char s;

		i = verify_area(VERIFY_WRITE, (void *)a, sizeof(struct kbentry));
		if (i)
			return i;
		if ((i = get_fs_byte((char *) &a->kb_index)) >= NR_KEYS)
			return -EINVAL;
		if ((s = get_fs_byte((char *) &a->kb_table)) >= NR_KEYMAPS)
			return -EINVAL;
		put_fs_word(key_map[s][i], (short *) &a->kb_value);
		return 0;
	}

	case KDSKBENT:
	{
		const struct kbentry * a = (struct kbentry *)arg;
		u_char s;
		u_short v;

		i = verify_area(VERIFY_READ, (void *)a, sizeof(struct kbentry));
		if (i)
			return i;
		if ((i = get_fs_byte((char *) &a->kb_index)) >= NR_KEYS)
			return -EINVAL;
		if ((s = get_fs_byte((char *) &a->kb_table)) >= NR_KEYMAPS)
			return -EINVAL;
		if (KTYP(v = get_fs_word(&a->kb_value)) >= NR_TYPES)
			return -EINVAL;
		if (KVAL(v) > max_vals[KTYP(v)])
			return -EINVAL;
		key_map[s][i] = v;
		return 0;
	}

	case KDGKBSENT:
	{
		struct kbsentry *a = (struct kbsentry *)arg;
		char *p;
		u_char *q;

		i = verify_area(VERIFY_WRITE, (void *)a, sizeof(struct kbsentry));
		if (i)
			return i;
		if ((i = get_fs_byte(&a->kb_func)) >= NR_FUNC || i < 0)
			return -EINVAL;
		q = a->kb_string;
		p = func_table[i];
		if(!p) {
		    /* beware of tables generated for a smaller NR_FUNC */
		    printk("KDGKBSENT error: func_table[%d] is nil.\n",
			   i);
		    return -EINVAL;
		}
		for ( ; *p; p++)
			put_fs_byte(*p, q++);
		put_fs_byte(0, q);
		return 0;
	}

	case KDSKBSENT:
	{
		struct kbsentry * const a = (struct kbsentry *)arg;
		int delta;
		char *first_free;
		int k;
		u_char *p;
		char *q;

		i = verify_area(VERIFY_READ, (void *)a, sizeof(struct kbsentry));
		if (i)
			return i;
		if ((i = get_fs_byte(&a->kb_func)) >= NR_FUNC)
			return -EINVAL;
		q = func_table[i];
		if (!q) {
		    /* beware of tables generated for a smaller NR_FUNC */
		    printk("KDSKBSENT error: func_table[%d] is nil.\n",
			   i);
		    return -EINVAL;
		}
		delta = -strlen(q);
		for (p = a->kb_string; get_fs_byte(p); p++)
			delta++;
		first_free = func_table[NR_FUNC - 1] +
			strlen(func_table[NR_FUNC - 1]) + 1;
		if (
			delta > 0 &&
			first_free + delta > func_buf + FUNC_BUFSIZE
		)
			return -EINVAL;
		if (i < NR_FUNC - 1) {
			memmove(
				func_table[i + 1] + delta,
				func_table[i + 1],
				first_free - func_table[i + 1]);
			for (k = i + 1; k < NR_FUNC; k++)
			    if (func_table[k])  /* just to be sure */
				func_table[k] += delta;
		}
		for (p = a->kb_string, q = func_table[i]; ; p++, q++)
			if (!(*q = get_fs_byte(p)))
				break;
		return 0;
	}

	case KDGKBDIACR:
	{
	        struct kbdiacrs *a = (struct kbdiacrs *)arg;

	        i = verify_area(VERIFY_WRITE, (void *) a, sizeof(struct kbdiacrs));
		if (i)
		        return i;
		put_fs_long(accent_table_size, &a->kb_cnt);
		memcpy_tofs(a->kbdiacr, accent_table,
			    accent_table_size*sizeof(struct kbdiacr));
		return 0;
	}

	case KDSKBDIACR:
	{
	        struct kbdiacrs *a = (struct kbdiacrs *)arg;
		unsigned int ct;

	        i = verify_area(VERIFY_READ, (void *) a, sizeof(struct kbdiacrs));
		if (i)
		        return i;
		ct = get_fs_long(&a->kb_cnt);
		if (ct >= MAX_DIACR)
		        return -EINVAL;
		accent_table_size = ct;
		memcpy_fromfs(accent_table, a->kbdiacr, ct*sizeof(struct kbdiacr));
		return 0;
	}

	case KDGETLED:
		i = verify_area(VERIFY_WRITE, (void *) arg, sizeof(unsigned char));
		if (i)
			return i;
		put_fs_byte(kbd->ledstate, (char *) arg);
		return 0;

	case KDSETLED:
		if (arg & ~7)
			return -EINVAL;
		kbd->ledstate = arg;
		set_leds();
		return 0;

	case VT_SETMODE:
	{
		struct vt_mode *vtmode = (struct vt_mode *)arg;
		char mode;

		i = verify_area(VERIFY_WRITE, (void *)vtmode, sizeof(struct vt_mode));
		if (i)
			return i;
		mode = get_fs_byte(&vtmode->mode);
		if (mode != VT_AUTO && mode != VT_PROCESS)
			return -EINVAL;
		vt_cons[console].vt_mode.mode = mode;
		vt_cons[console].vt_mode.waitv = get_fs_byte(&vtmode->waitv);
		vt_cons[console].vt_mode.relsig = get_fs_word(&vtmode->relsig);
		vt_cons[console].vt_mode.acqsig = get_fs_word(&vtmode->acqsig);
		/* the frsig is ignored, so we set it to 0 */
		vt_cons[console].vt_mode.frsig = 0;
		vt_cons[console].vt_pid = current->pid;
		vt_cons[console].vt_newvt = 0;
		return 0;
	}

	case VT_GETMODE:
	{
		struct vt_mode *vtmode = (struct vt_mode *)arg;

		i = verify_area(VERIFY_WRITE, (void *)arg, sizeof(struct vt_mode));
		if (i)
			return i;
		put_fs_byte(vt_cons[console].vt_mode.mode, &vtmode->mode);
		put_fs_byte(vt_cons[console].vt_mode.waitv, &vtmode->waitv);
		put_fs_word(vt_cons[console].vt_mode.relsig, &vtmode->relsig);
		put_fs_word(vt_cons[console].vt_mode.acqsig, &vtmode->acqsig);
		put_fs_word(vt_cons[console].vt_mode.frsig, &vtmode->frsig);
		return 0;
	}

	/*
	 * Returns global vt state. Note that VT 0 is always open, since
	 * it's an alias for the current VT, and people can't use it here.
	 */
	case VT_GETSTATE:
	{
		struct vt_stat *vtstat = (struct vt_stat *)arg;
		unsigned short state, mask;

		i = verify_area(VERIFY_WRITE,(void *)vtstat, sizeof(struct vt_stat));
		if (i)
			return i;
		put_fs_word(fg_console + 1, &vtstat->v_active);
		state = 1;	/* /dev/tty0 is always open */
		for (i = 1, mask = 2; i <= NR_CONSOLES; ++i, mask <<= 1)
			if (tty_table[i] && tty_table[i]->count > 0)
				state |= mask;
		put_fs_word(state, &vtstat->v_state);
		return 0;
	}

	/*
	 * Returns the first available (non-opened) console.
	 */
	case VT_OPENQRY:
		i = verify_area(VERIFY_WRITE, (void *) arg, sizeof(long));
		if (i)
			return i;
		for (i = 1; i <= NR_CONSOLES; ++i)
			if (!tty_table[i] || tty_table[i]->count == 0)
				break;
		put_fs_long(i <= NR_CONSOLES ? i : -1, (unsigned long *)arg);
		return 0;

	/*
	 * ioctl(fd, VT_ACTIVATE, num) will cause us to switch to vt # num,
	 * with num >= 1 (switches to vt 0, our console) are not allowed, just
	 * to preserve sanity.
	 */
	case VT_ACTIVATE:
		if (arg == 0 || arg > NR_CONSOLES)
			return -ENXIO;
		change_console(arg - 1);
		return 0;

	/*
	 * wait until the specified VT has been activated
	 */
	case VT_WAITACTIVE:
		if (arg == 0 || arg > NR_CONSOLES)
			return -ENXIO;
		while (fg_console != arg - 1)
		{
			if (vt_waitactive() < 0)
				return -EINTR;
		}
		return 0;

	/*
	 * If a vt is under process control, the kernel will not switch to it
	 * immediately, but postpone the operation until the process calls this
	 * ioctl, allowing the switch to complete.
	 *
	 * According to the X sources this is the behavior:
	 *	0:	pending switch-from not OK
	 *	1:	pending switch-from OK
	 *	2:	completed switch-to OK
	 */
	case VT_RELDISP:
		if (vt_cons[console].vt_mode.mode != VT_PROCESS)
			return -EINVAL;

		/*
		 * Switching-from response
		 */
		if (vt_cons[console].vt_newvt >= 0)
		{
			if (arg == 0)
				/*
				 * Switch disallowed, so forget we were trying
				 * to do it.
				 */
				vt_cons[console].vt_newvt = -1;

			else
			{
				/*
				 * The current vt has been released, so
				 * complete the switch.
				 */
				int newvt = vt_cons[console].vt_newvt;
				vt_cons[console].vt_newvt = -1;
				complete_change_console(newvt);
			}
		}

		/*
		 * Switched-to response
		 */
		else
		{
			/*
			 * If it's just an ACK, ignore it
			 */
			if (arg != VT_ACKACQ)
				return -EINVAL;
		}

		return 0;

	case PIO_FONT:
		return con_set_font((char *)arg);
		/* con_set_font() defined in console.c */

	case GIO_FONT:
		return con_get_font((char *)arg);
		/* con_get_font() defined in console.c */

	case PIO_SCRNMAP:
		return con_set_trans((char *)arg);
		/* con_set_trans() defined in console.c */

	case GIO_SCRNMAP:
		return con_get_trans((char *)arg);
		/* con_get_trans() defined in console.c */

	default:
		return -EINVAL;
	}
}
