/*
 * linux/kernel/chr_drv/keyboard.c
 *
 * Keyboard driver for Linux v0.99 using Latin-1.
 *
 * Written for linux by Johan Myreen as a translation from
 * the assembly version by Linus (with diacriticals added)
 *
 * Some additional features added by Christoph Niemann (ChN), March 1993
 * Loadable keymaps by Risto Kankkunen, May 1993
 * Diacriticals redone & other small changes, aeb@cwi.nl, June 1993
 */

#define KEYBOARD_IRQ 1

#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/mm.h>
#include <linux/ptrace.h>
#include <linux/interrupt.h>
#include <linux/config.h>
#include <linux/signal.h>
#include <linux/string.h>

#include <asm/bitops.h>

#include "kbd_kern.h"
#include "diacr.h"

#define SIZE(x) (sizeof(x)/sizeof((x)[0]))

#define KBD_REPORT_ERR
#define KBD_REPORT_UNKN

#ifndef KBD_DEFMODE
#define KBD_DEFMODE ((1 << VC_REPEAT) | (1 << VC_META))
#endif

#ifndef KBD_DEFLEDS
/*
 * Some laptops take the 789uiojklm,. keys as number pad when NumLock
 * is on. This seems a good reason to start with NumLock off.
 */
#define KBD_DEFLEDS 0
#endif

#ifndef KBD_DEFLOCK
#define KBD_DEFLOCK 0
#endif

/*
 * The default IO slowdown is doing 'inb()'s from 0x61, which should be
 * safe. But as that is the keyboard controller chip address, we do our
 * slowdowns here by doing short jumps: the keyboard controller should
 * be able to keep up
 */
#define REALLY_SLOW_IO
#define SLOW_IO_BY_JUMPING
#include <asm/io.h>
#include <asm/system.h>

extern void do_keyboard_interrupt(void);
extern void ctrl_alt_del(void);
extern void change_console(unsigned int new_console);
extern void scrollback(int);
extern void scrollfront(int);

#define fake_keyboard_interrupt() \
__asm__ __volatile__("int $0x21")

unsigned char kbd_read_mask = 0x01;	/* modified by psaux.c */

/*
 * global state includes the following, and various static variables
 * in this module: prev_scancode, shift_state, diacr, npadch,
 *   dead_key_next, last_console
 */

/* shift state counters.. */
static unsigned char k_down[NR_SHIFT] = {0, };
/* keyboard key bitmap */
static unsigned long key_down[8] = { 0, };

static int want_console = -1;
static int last_console = 0;		/* last used VC */
static int dead_key_next = 0;
static int shift_state = 0;
static int npadch = -1;		        /* -1 or number assembled on pad */
static unsigned char diacr = 0;
static char rep = 0;			/* flag telling character repeat */
struct kbd_struct kbd_table[NR_CONSOLES];
static struct kbd_struct * kbd = kbd_table;
static struct tty_struct * tty = NULL;

/* used only by send_data - set by keyboard_interrupt */
static volatile unsigned char acknowledge = 0;
static volatile unsigned char resend = 0;

typedef void (*k_hand)(unsigned char value, char up_flag);
typedef void (k_handfn)(unsigned char value, char up_flag);

static k_handfn
        do_self, do_fn, do_spec, do_pad, do_dead, do_cons, do_cur, do_shift,
	do_meta, do_ascii, do_lock, do_lowercase;

static k_hand key_handler[] = {
	do_self, do_fn, do_spec, do_pad, do_dead, do_cons, do_cur, do_shift,
	do_meta, do_ascii, do_lock, do_lowercase
};

/* maximum values each key_handler can handle */
const int max_vals[] = {
	255, NR_FUNC - 1, 14, 17, 4, 255, 3, NR_SHIFT,
	255, 9, 3, 255
};

const int NR_TYPES = SIZE(max_vals);

static void put_queue(int);
static unsigned char handle_diacr(unsigned char);

/* pt_regs - set by keyboard_interrupt(), used by show_ptregs() */
static struct pt_regs * pt_regs;

static int got_break = 0;

static inline void kb_wait(void)
{
	int i;

	for (i=0; i<0x10000; i++)
		if ((inb_p(0x64) & 0x02) == 0)
			break;
}

static inline void send_cmd(unsigned char c)
{
	kb_wait();
	outb(c,0x64);
}

/*
 * Translation of escaped scancodes to keysyms.
 * This should be user-settable.
 */
#define E0_BASE 96

#define E0_KPENTER (E0_BASE+0)
#define E0_RCTRL   (E0_BASE+1)
#define E0_KPSLASH (E0_BASE+2)
#define E0_PRSCR   (E0_BASE+3)
#define E0_RALT    (E0_BASE+4)
#define E0_BREAK   (E0_BASE+5)  /* (control-pause) */
#define E0_HOME    (E0_BASE+6)
#define E0_UP      (E0_BASE+7)
#define E0_PGUP    (E0_BASE+8)
#define E0_LEFT    (E0_BASE+9)
#define E0_RIGHT   (E0_BASE+10)
#define E0_END     (E0_BASE+11)
#define E0_DOWN    (E0_BASE+12)
#define E0_PGDN    (E0_BASE+13)
#define E0_INS     (E0_BASE+14)
#define E0_DEL     (E0_BASE+15)
/* BTC */
#define E0_MACRO   (E0_BASE+16)
/* LK450 */
#define E0_F13     (E0_BASE+17)
#define E0_F14     (E0_BASE+18)
#define E0_HELP    (E0_BASE+19)
#define E0_DO      (E0_BASE+20)
#define E0_F17     (E0_BASE+21)
#define E0_KPMINPLUS (E0_BASE+22)

#define E1_PAUSE   (E0_BASE+23)

static unsigned char e0_keys[128] = {
  0, 0, 0, 0, 0, 0, 0, 0,			      /* 0x00-0x07 */
  0, 0, 0, 0, 0, 0, 0, 0,			      /* 0x08-0x0f */
  0, 0, 0, 0, 0, 0, 0, 0,			      /* 0x10-0x17 */
  0, 0, 0, 0, E0_KPENTER, E0_RCTRL, 0, 0,	      /* 0x18-0x1f */
  0, 0, 0, 0, 0, 0, 0, 0,			      /* 0x20-0x27 */
  0, 0, 0, 0, 0, 0, 0, 0,			      /* 0x28-0x2f */
  0, 0, 0, 0, 0, E0_KPSLASH, 0, E0_PRSCR,	      /* 0x30-0x37 */
  E0_RALT, 0, 0, 0, 0, E0_F13, E0_F14, E0_HELP,	      /* 0x38-0x3f */
  E0_DO, E0_F17, 0, 0, 0, 0, E0_BREAK, E0_HOME,	      /* 0x40-0x47 */
  E0_UP, E0_PGUP, 0, E0_LEFT, 0, E0_RIGHT, E0_KPMINPLUS, E0_END,/* 0x48-0x4f */
  E0_DOWN, E0_PGDN, E0_INS, E0_DEL, 0, 0, 0, 0,	      /* 0x50-0x57 */
  0, 0, 0, 0, 0, 0, 0, 0,			      /* 0x58-0x5f */
  0, 0, 0, 0, 0, 0, 0, 0,			      /* 0x60-0x67 */
  0, 0, 0, 0, 0, 0, 0, E0_MACRO,		      /* 0x68-0x6f */
  0, 0, 0, 0, 0, 0, 0, 0,			      /* 0x70-0x77 */
  0, 0, 0, 0, 0, 0, 0, 0			      /* 0x78-0x7f */
};

static void keyboard_interrupt(int int_pt_regs)
{
	unsigned char scancode;
	static unsigned int prev_scancode = 0;   /* remember E0, E1 */
	char up_flag;		                 /* 0 or 0200 */
	char raw_mode;

	pt_regs = (struct pt_regs *) int_pt_regs;
	send_cmd(0xAD);		/* disable keyboard */
	kb_wait();
	if ((inb_p(0x64) & kbd_read_mask) != 0x01)
		goto end_kbd_intr;
	scancode = inb(0x60);
	mark_bh(KEYBOARD_BH);
	if (scancode == 0xfa) {
		acknowledge = 1;
		goto end_kbd_intr;
	} else if (scancode == 0xfe) {
		resend = 1;
		goto end_kbd_intr;
	} else if (scancode == 0) {
#ifdef KBD_REPORT_ERR
	        printk("keyboard buffer overflow\n");
#endif
		goto end_kbd_intr;
	} else if (scancode == 0xff) {
#ifdef KBD_REPORT_ERR
	        printk("keyboard error\n");
#endif
	        prev_scancode = 0;
	        goto end_kbd_intr;
	}
	tty = TTY_TABLE(0);
 	kbd = kbd_table + fg_console;
	if ((raw_mode = vc_kbd_mode(kbd,VC_RAW))) {
 		put_queue(scancode);
		/* we do not return yet, because we want to maintain
		   the key_down array, so that we have the correct
		   values when finishing RAW mode or when changing VT's */
 	}
	if (scancode == 0xe0 || scancode == 0xe1) {
		prev_scancode = scancode;
		goto end_kbd_intr;
 	}

 	/*
	 *  Convert scancode to keysym, using prev_scancode.
 	 */
	up_flag = (scancode & 0200);
 	scancode &= 0x7f;
  
	if (prev_scancode) {
	  /*
	   * usually it will be 0xe0, but a Pause key generates
	   * e1 1d 45 e1 9d c5 when pressed, and nothing when released
	   */
	  if (prev_scancode != 0xe0) {
	      if (prev_scancode == 0xe1 && scancode == 0x1d) {
		  prev_scancode = 0x100;
		  goto end_kbd_intr;
	      } else if (prev_scancode == 0x100 && scancode == 0x45) {
		  scancode = E1_PAUSE;
		  prev_scancode = 0;
	      } else {
		  printk("keyboard: unknown e1 escape sequence\n");
		  prev_scancode = 0;
		  goto end_kbd_intr;
	      }
	  } else {
	      prev_scancode = 0;
	      /*
	       *  The keyboard maintains its own internal caps lock and
	       *  num lock statuses. In caps lock mode E0 AA precedes make
	       *  code and E0 2A follows break code. In num lock mode,
	       *  E0 2A precedes make code and E0 AA follows break code.
	       *  We do our own book-keeping, so we will just ignore these.
	       */
	      /*
	       *  For my keyboard there is no caps lock mode, but there are
	       *  both Shift-L and Shift-R modes. The former mode generates
	       *  E0 2A / E0 AA pairs, the latter E0 B6 / E0 36 pairs.
	       *  So, we should also ignore the latter. - aeb@cwi.nl
	       */
	      if (scancode == 0x2a || scancode == 0x36)
		goto end_kbd_intr;

	      if (e0_keys[scancode])
		scancode = e0_keys[scancode];
	      else if (!raw_mode) {
#ifdef KBD_REPORT_UNKN
		  printk("keyboard: unknown scancode e0 %02x\n", scancode);
#endif
		  goto end_kbd_intr;
	      }
	  }
	} else if (scancode >= E0_BASE && !raw_mode) {
#ifdef KBD_REPORT_UNKN
	  printk("keyboard: scancode (%02x) not in range 00 - %2x\n",
		 scancode, E0_BASE - 1);
#endif
	  goto end_kbd_intr;
 	}
  
	/*
	 * At this point the variable `scancode' contains the keysym.
	 * We keep track of the up/down status of the key, and
	 * return the keysym if in MEDIUMRAW mode.
	 * (Note: earlier kernels had a bug and did not pass the up/down
	 * bit to applications.)
	 */

	if (up_flag) {
 		clear_bit(scancode, key_down);
		rep = 0;
	} else
 		rep = set_bit(scancode, key_down);
  
	if (raw_mode)
	        goto end_kbd_intr;

 	if (vc_kbd_mode(kbd, VC_MEDIUMRAW)) {
 		put_queue(scancode + up_flag);
		goto end_kbd_intr;
 	}
  
 	/*
	 * Small change in philosophy: earlier we defined repetition by
	 *	 rep = scancode == prev_keysym;
	 *	 prev_keysym = scancode;
	 * but now by the fact that the depressed key was down already.
	 * Does this ever make a difference?
	 */

	/*
 	 *  Repeat a key only if the input buffers are empty or the
 	 *  characters get echoed locally. This makes key repeat usable
 	 *  with slow applications and under heavy loads.
	 */
	if (!rep || 
	    (vc_kbd_mode(kbd,VC_REPEAT) && tty &&
	     (L_ECHO(tty) || (EMPTY(&tty->secondary) && EMPTY(&tty->read_q)))))
	{
		u_short key_code;
		u_char type;

		/* the XOR below used to be an OR */
		int shift_final = shift_state ^ kbd->lockstate;

		key_code = key_map[shift_final][scancode];
		type = KTYP(key_code);

		if (type == KT_LETTER) {
		    type = KT_LATIN;
		    if (vc_kbd_led(kbd,VC_CAPSLOCK))
			key_code = key_map[shift_final ^ (1<<KG_SHIFT)][scancode];
		}
		(*key_handler[type])(key_code & 0xff, up_flag);
	}

end_kbd_intr:
	send_cmd(0xAE);         /* enable keyboard */
}

static void put_queue(int ch)
{
	struct tty_queue *qp;

	wake_up(&keypress_wait);
	if (!tty)
		return;
	qp = &tty->read_q;

	if (LEFT(qp)) {
		qp->buf[qp->head] = ch;
		INC(qp->head);
	}
}

static void puts_queue(char *cp)
{
	struct tty_queue *qp;
	char ch;

	/* why interruptible here, plain wake_up above? */
	wake_up_interruptible(&keypress_wait);
	if (!tty)
		return;
	qp = &tty->read_q;

	while ((ch = *(cp++)) != 0) {
		if (LEFT(qp)) {
			qp->buf[qp->head] = ch;
			INC(qp->head);
		}
	}
}

static void applkey(int key, char mode)
{
	static char buf[] = { 0x1b, 'O', 0x00, 0x00 };

	buf[1] = (mode ? 'O' : '[');
	buf[2] = key;
	puts_queue(buf);
}

static void enter(void)
{
	put_queue(13);
	if (vc_kbd_mode(kbd,VC_CRLF))
		put_queue(10);
}

static void caps_toggle(void)
{
	if (rep)
		return;
	chg_vc_kbd_led(kbd,VC_CAPSLOCK);
}

static void caps_on(void)
{
	if (rep)
		return;
	set_vc_kbd_led(kbd,VC_CAPSLOCK);
}

static void show_ptregs(void)
{
	if (!pt_regs)
		return;
	printk("\n");
	printk("EIP: %04x:%08lx",0xffff & pt_regs->cs,pt_regs->eip);
	if (pt_regs->cs & 3)
		printk(" ESP: %04x:%08lx",0xffff & pt_regs->ss,pt_regs->esp);
	printk(" EFLAGS: %08lx\n",pt_regs->eflags);
	printk("EAX: %08lx EBX: %08lx ECX: %08lx EDX: %08lx\n",
		pt_regs->orig_eax,pt_regs->ebx,pt_regs->ecx,pt_regs->edx);
	printk("ESI: %08lx EDI: %08lx EBP: %08lx",
		pt_regs->esi, pt_regs->edi, pt_regs->ebp);
	printk(" DS: %04x ES: %04x FS: %04x GS: %04x\n",
		0xffff & pt_regs->ds,0xffff & pt_regs->es,
		0xffff & pt_regs->fs,0xffff & pt_regs->gs);
}

static void hold(void)
{
	if (rep || !tty)
		return;

	/*
	 * Note: SCROLLOCK wil be set (cleared) by stop_tty (start_tty);
	 * these routines are also activated by ^S/^Q.
	 * (And SCROLLOCK can also be set by the ioctl KDSETLED.)
	 */
	if (tty->stopped)
		start_tty(tty);
	else
		stop_tty(tty);
}

#if 0
/* unused at present - and the VC_PAUSE bit is not used anywhere either */
static void pause(void)
{
	chg_vc_kbd_mode(kbd,VC_PAUSE);
}
#endif

static void num(void)
{
	if (vc_kbd_mode(kbd,VC_APPLIC)) {
		applkey('P', 1);
		return;
	}
	if (!rep)	/* no autorepeat for numlock, ChN */
		chg_vc_kbd_led(kbd,VC_NUMLOCK);
}

static void lastcons(void)
{
	/* pressing alt-printscreen switches to the last used console, ChN */
	want_console = last_console;
}

static void send_intr(void)
{
	got_break = 1;
}

static void scrll_forw(void)
{
	scrollfront(0);
}

static void scrll_back(void)
{
	scrollback(0);
}

static void boot_it(void)
{
	ctrl_alt_del();
}

static void compose(void)
{
        dead_key_next = 1;
}

static void do_spec(unsigned char value, char up_flag)
{
	typedef void (*fnp)(void);
	fnp fn_table[] = {
		NULL,		enter,		show_ptregs,	show_mem,
		show_state,	send_intr,	lastcons,	caps_toggle,
		num,		hold,		scrll_forw,	scrll_back,
		boot_it,	caps_on,        compose
	};

	if (up_flag)
		return;
	if (value >= SIZE(fn_table))
		return;
	if (!fn_table[value])
		return;
	fn_table[value]();
}

static void do_lowercase(unsigned char value, char up_flag)
{
        printk("keyboard.c: do_lowercase was called - impossible\n");
}
  
static void do_self(unsigned char value, char up_flag)
{
	if (up_flag)
		return;		/* no action, if this is a key release */

        if (diacr)
                value = handle_diacr(value);

        if (dead_key_next) {
                dead_key_next = 0;
                diacr = value;
                return;
        }

	put_queue(value);
}

#define A_GRAVE  '`'
#define A_ACUTE  '\''
#define A_CFLEX  '^'
#define A_TILDE  '~'
#define A_DIAER  '"'
static unsigned char ret_diacr[] =
        {A_GRAVE, A_ACUTE, A_CFLEX, A_TILDE, A_DIAER };

/* If a dead key pressed twice, output a character corresponding to it,	*/
/* otherwise just remember the dead key.				*/

static void do_dead(unsigned char value, char up_flag)
{
	if (up_flag)
		return;

        value = ret_diacr[value];
        if (diacr == value) {   /* pressed twice */
                diacr = 0;
                put_queue(value);
                return;
        }
	diacr = value;
}


/* If space is pressed, return the character corresponding the pending	*/
/* dead key, otherwise try to combine the two.				*/

unsigned char handle_diacr(unsigned char ch)
{
        int d = diacr;
        int i;

        diacr = 0;
        if (ch == ' ')
                return d;

        for (i = 0; i < accent_table_size; i++)
          if(accent_table[i].diacr == d && accent_table[i].base == ch)
            return accent_table[i].result;

        put_queue(d);
        return ch;
}

static void do_cons(unsigned char value, char up_flag)
{
	if (up_flag)
		return;
	want_console = value;
}

static void do_fn(unsigned char value, char up_flag)
{
	if (up_flag)
		return;
	if (value < SIZE(func_table))
	        puts_queue(func_table[value]);
	else
	        printk("do_fn called with value=%d\n", value);
}

static void do_pad(unsigned char value, char up_flag)
{
	static char *pad_chars = "0123456789+-*/\015,.?";
	static char *app_map = "pqrstuvwxylSRQMnn?";

	if (up_flag)
		return;		/* no action, if this is a key release */

	/* kludge... shift forces cursor/number keys */
	if (vc_kbd_mode(kbd,VC_APPLIC) && !k_down[KG_SHIFT]) {
		applkey(app_map[value], 1);
		return;
	}

	if (!vc_kbd_led(kbd,VC_NUMLOCK))
		switch (value) {
			case KVAL(K_PCOMMA):
			case KVAL(K_PDOT):
				do_fn(KVAL(K_REMOVE), 0);
				return;
			case KVAL(K_P0):
				do_fn(KVAL(K_INSERT), 0);
				return;
			case KVAL(K_P1):
				do_fn(KVAL(K_SELECT), 0);
				return;
			case KVAL(K_P2):
				do_cur(KVAL(K_DOWN), 0);
				return;
			case KVAL(K_P3):
				do_fn(KVAL(K_PGDN), 0);
				return;
			case KVAL(K_P4):
				do_cur(KVAL(K_LEFT), 0);
				return;
			case KVAL(K_P6):
				do_cur(KVAL(K_RIGHT), 0);
				return;
			case KVAL(K_P7):
				do_fn(KVAL(K_FIND), 0);
				return;
			case KVAL(K_P8):
				do_cur(KVAL(K_UP), 0);
				return;
			case KVAL(K_P9):
				do_fn(KVAL(K_PGUP), 0);
				return;
			case KVAL(K_P5):
				applkey('G', vc_kbd_mode(kbd, VC_APPLIC));
				return;
		}

	put_queue(pad_chars[value]);
	if (value == KVAL(K_PENTER) && vc_kbd_mode(kbd, VC_CRLF))
		put_queue(10);
}

static void do_cur(unsigned char value, char up_flag)
{
	static char *cur_chars = "BDCA";
	if (up_flag)
		return;

	applkey(cur_chars[value], vc_kbd_mode(kbd,VC_CKMODE));
}

static void do_shift(unsigned char value, char up_flag)
{
	int old_state = shift_state;

	if (rep)
		return;

	/* kludge... */
	if (value == KVAL(K_CAPSSHIFT)) {
		value = KVAL(K_SHIFT);
		clr_vc_kbd_led(kbd, VC_CAPSLOCK);
	}

	if (up_flag) {
	        /* handle the case that two shift or control
		   keys are depressed simultaneously */
		if (k_down[value])
			k_down[value]--;
	} else
		k_down[value]++;

	if (k_down[value])
		shift_state |= (1 << value);
	else
		shift_state &= ~ (1 << value);

	/* kludge */
	if (up_flag && shift_state != old_state && npadch != -1) {
		put_queue(npadch);
		npadch = -1;
	}
}

/* called after returning from RAW mode or when changing consoles -
   recompute k_down[] and shift_state from key_down[] */
void compute_shiftstate(void)
{
        int i, j, k, sym, val;

        shift_state = 0;
	for(i=0; i < SIZE(k_down); i++)
	  k_down[i] = 0;

	for(i=0; i < SIZE(key_down); i++)
	  if(key_down[i]) {	/* skip this word if not a single bit on */
	    k = (i<<5);
	    for(j=0; j<32; j++,k++)
	      if(test_bit(k, key_down)) {
		sym = key_map[0][k];
		if(KTYP(sym) == KT_SHIFT) {
		  val = KVAL(sym);
		  k_down[val]++;
		  shift_state |= (1<<val);
	        }
	      }
	  }
}

static void do_meta(unsigned char value, char up_flag)
{
	if (up_flag)
		return;

	if (vc_kbd_mode(kbd, VC_META)) {
		put_queue('\033');
		put_queue(value);
	} else
		put_queue(value | 0x80);
}

static void do_ascii(unsigned char value, char up_flag)
{
	if (up_flag)
		return;

	if (npadch == -1)
	        npadch = value;
	else
	        npadch = (npadch * 10 + value) % 1000;
}

static void do_lock(unsigned char value, char up_flag)
{
	if (up_flag || rep)
		return;
	chg_vc_kbd_lock(kbd, value);
}

/*
 * send_data sends a character to the keyboard and waits
 * for a acknowledge, possibly retrying if asked to. Returns
 * the success status.
 */
static int send_data(unsigned char data)
{
	int retries = 3;
	int i;

	do {
		kb_wait();
		acknowledge = 0;
		resend = 0;
		outb_p(data, 0x60);
		for(i=0; i<0x20000; i++) {
			inb_p(0x64);		/* just as a delay */
			if (acknowledge)
				return 1;
			if (resend)
				break;
		}
		if (!resend)
			return 0;
	} while (retries-- > 0);
	return 0;
}

/*
 * This routine is the bottom half of the keyboard interrupt
 * routine, and runs with all interrupts enabled. It does
 * console changing, led setting and copy_to_cooked, which can
 * take a reasonably long time.
 *
 * Aside from timing (which isn't really that important for
 * keyboard interrupts as they happen often), using the software
 * interrupt routines for this thing allows us to easily mask
 * this when we don't want any of the above to happen. Not yet
 * used, but this allows for easy and efficient race-condition
 * prevention later on.
 */
static void kbd_bh(void * unused)
{
	static unsigned char old_leds = 0xff;
	unsigned char leds = kbd_table[fg_console].ledstate;

	if (leds != old_leds) {
		old_leds = leds;
		if (!send_data(0xed) || !send_data(leds))
			send_data(0xf4);	/* re-enable kbd if any errors */
	}
	if (want_console >= 0) {
		if (want_console != fg_console) {
			last_console = fg_console;
			change_console(want_console);
		}
		want_console = -1;
	}
	if (got_break) {
		if (tty && !I_IGNBRK(tty)) {
			if (I_BRKINT(tty)) {
				flush_input(tty);
				flush_output(tty);
				if (tty->pgrp > 0)
					kill_pg(tty->pgrp, SIGINT, 1);
			} else {
				cli();
				if (LEFT(&tty->read_q) >= 2) {
					set_bit(tty->read_q.head,
						&tty->readq_flags);
					put_queue(TTY_BREAK);
					put_queue(0);
				}
				sti();
			}
		}
		got_break = 0;
	}
	do_keyboard_interrupt();
	cli();
	if ((inb_p(0x64) & kbd_read_mask) == 0x01)
		fake_keyboard_interrupt();
	sti();
}

long no_idt[2] = {0, 0};

/*
 * This routine reboots the machine by asking the keyboard
 * controller to pulse the reset-line low. We try that for a while,
 * and if it doesn't work, we do some other stupid things.
 */
void hard_reset_now(void)
{
	int i, j;
	extern unsigned long pg0[1024];

	sti();
/* rebooting needs to touch the page at absolute addr 0 */
	pg0[0] = 7;
	*((unsigned short *)0x472) = 0x1234;
	for (;;) {
		for (i=0; i<100; i++) {
			kb_wait();
			for(j = 0; j < 100000 ; j++)
				/* nothing */;
			outb(0xfe,0x64);	 /* pulse reset low */
		}
		__asm__("\tlidt _no_idt");
	}
}

unsigned long kbd_init(unsigned long kmem_start)
{
	int i;
	struct kbd_struct * kbd;

	kbd = kbd_table + 0;
	for (i = 0 ; i < NR_CONSOLES ; i++,kbd++) {
		kbd->ledstate = KBD_DEFLEDS;
		kbd->default_ledstate = KBD_DEFLEDS;
		kbd->lockstate = KBD_DEFLOCK;
		kbd->modeflags = KBD_DEFMODE;
	}

	bh_base[KEYBOARD_BH].routine = kbd_bh;
	request_irq(KEYBOARD_IRQ,keyboard_interrupt);
	mark_bh(KEYBOARD_BH);
	return kmem_start;
}
