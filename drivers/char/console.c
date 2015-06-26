/*
 *  linux/kernel/console.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 *	console.c
 *
 * This module exports the console io functions:
 * 
 *	'long con_init(long)'
 *	'int con_open(struct tty_struct *tty, struct file * filp)'
 * 	'void update_screen(int new_console)'
 * 	'void blank_screen(void)'
 * 	'void unblank_screen(void)'
 *
 *      'int  con_get_font(char *)' 
 *      'int  con_set_font(char *)' 
 *      'int  con_get_trans(char *)'
 *      'int  con_set_trans(char *)'
 * 
 * Hopefully this will be a rather complete VT102 implementation.
 *
 * Beeping thanks to John T Kohl.
 * 
 * Virtual Consoles, Screen Blanking, Screen Dumping, Color, Graphics
 *   Chars, and VT100 enhancements by Peter MacDonald.
 *
 * Copy and paste function by Andrew Haylett.
 *
 * User definable mapping table and font loading by Eugene G. Crosser,
 * <crosser@pccross.msk.su>
 *
 * Code to check for different video-cards mostly by Galen Hunt,
 * <g-hunt@ee.utah.edu>
 *
 */

#define CAN_LOAD_EGA_FONTS    /* undefine if the user must not do this */

/*
 *  NOTE!!! We sometimes disable and enable interrupts for a short while
 * (to put a word in video IO), but this will work even for keyboard
 * interrupts. We know interrupts aren't enabled when getting a keyboard
 * interrupt, as we use trap-gates. Hopefully all is well.
 */

#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/tty.h>
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/kd.h>

#include <asm/io.h>
#include <asm/system.h>
#include <asm/segment.h>

#include "kbd_kern.h"
#include "vt_kern.h"

#ifdef CONFIG_SELECTION
#include <linux/ctype.h>

/* Routines for selection control. */
int set_selection(const int arg);
int paste_selection(struct tty_struct *tty);
static void clear_selection(void);

/* Variables for selection control. */
#define SEL_BUFFER_SIZE TTY_BUF_SIZE
static int sel_cons;
static int sel_start = -1;
static int sel_end;
static char sel_buffer[SEL_BUFFER_SIZE] = { '\0' };
#endif /* CONFIG_SELECTION */

#define NPAR 16

extern void vt_init(void);
extern void register_console(void (*proc)(const char *));
extern void compute_shiftstate(void);

unsigned long	video_num_columns;		/* Number of text columns	*/
unsigned long	video_num_lines;		/* Number of text lines		*/

static unsigned char	video_type;		/* Type of display being used	*/
static unsigned long	video_mem_base;		/* Base of video memory		*/
static unsigned long	video_mem_term;		/* End of video memory		*/
static unsigned long	video_size_row;		/* Bytes per row		*/
static unsigned char	video_page;		/* Initial video page		*/
static unsigned short	video_port_reg;		/* Video register select port	*/
static unsigned short	video_port_val;		/* Video register value port	*/
static int can_do_color = 0;
static int printable = 0;

static struct {
	unsigned short	vc_video_erase_char;	/* Background erase character */
	unsigned char	vc_attr;		/* Current attributes */
	unsigned char	vc_def_color;		/* Default colors */
	unsigned char	vc_color;		/* Foreground & background */
	unsigned char	vc_s_color;		/* Saved foreground & background */
	unsigned char	vc_ulcolor;		/* Colour for underline mode */
	unsigned char	vc_halfcolor;		/* Colour for half intensity mode */
	unsigned long	vc_origin;		/* Used for EGA/VGA fast scroll	*/
	unsigned long	vc_scr_end;		/* Used for EGA/VGA fast scroll	*/
	unsigned long	vc_pos;
	unsigned long	vc_x,vc_y;
	unsigned long	vc_top,vc_bottom;
	unsigned long	vc_state;
	unsigned long	vc_npar,vc_par[NPAR];
	unsigned long	vc_video_mem_start;	/* Start of video RAM		*/
	unsigned long	vc_video_mem_end;	/* End of video RAM (sort of)	*/
	unsigned long	vc_saved_x;
	unsigned long	vc_saved_y;
	/* mode flags */
	unsigned long	vc_charset	: 1;	/* Character set G0 / G1 */
	unsigned long	vc_s_charset	: 1;	/* Saved character set */
	unsigned long	vc_decscnm	: 1;	/* Screen Mode */
	unsigned long	vc_decom	: 1;	/* Origin Mode */
	unsigned long	vc_decawm	: 1;	/* Autowrap Mode */
	unsigned long	vc_deccm	: 1;	/* Cursor Visible */
	unsigned long	vc_decim	: 1;	/* Insert Mode */
	/* attribute flags */
	unsigned long	vc_intensity	: 2;	/* 0=half-bright, 1=normal, 2=bold */
	unsigned long	vc_underline	: 1;
	unsigned long	vc_blink	: 1;
	unsigned long	vc_reverse	: 1;
	unsigned long	vc_s_intensity	: 2;	/* saved rendition */
	unsigned long	vc_s_underline	: 1;
	unsigned long	vc_s_blink	: 1;
	unsigned long	vc_s_reverse	: 1;
	/* misc */
	unsigned long	vc_ques		: 1;
	unsigned long	vc_need_wrap	: 1;
	unsigned long	vc_tab_stop[5];		/* Tab stops. 160 columns. */
	unsigned char * vc_translate;
	unsigned char *	vc_G0_charset;
	unsigned char *	vc_G1_charset;
	unsigned char *	vc_saved_G0;
	unsigned char *	vc_saved_G1;
	/* additional information is in vt_kern.h */
} vc_cons [NR_CONSOLES];

unsigned short *vc_scrbuf[NR_CONSOLES];
static unsigned short * vc_scrmembuf;
static int console_blanked = 0;

#define origin		(vc_cons[currcons].vc_origin)
#define scr_end		(vc_cons[currcons].vc_scr_end)
#define pos		(vc_cons[currcons].vc_pos)
#define top		(vc_cons[currcons].vc_top)
#define bottom		(vc_cons[currcons].vc_bottom)
#define x		(vc_cons[currcons].vc_x)
#define y		(vc_cons[currcons].vc_y)
#define state		(vc_cons[currcons].vc_state)
#define npar		(vc_cons[currcons].vc_npar)
#define par		(vc_cons[currcons].vc_par)
#define ques		(vc_cons[currcons].vc_ques)
#define attr		(vc_cons[currcons].vc_attr)
#define saved_x		(vc_cons[currcons].vc_saved_x)
#define saved_y		(vc_cons[currcons].vc_saved_y)
#define translate	(vc_cons[currcons].vc_translate)
#define G0_charset	(vc_cons[currcons].vc_G0_charset)
#define G1_charset	(vc_cons[currcons].vc_G1_charset)
#define saved_G0	(vc_cons[currcons].vc_saved_G0)
#define saved_G1	(vc_cons[currcons].vc_saved_G1)
#define video_mem_start	(vc_cons[currcons].vc_video_mem_start)
#define video_mem_end	(vc_cons[currcons].vc_video_mem_end)
#define video_erase_char (vc_cons[currcons].vc_video_erase_char)	
#define decscnm		(vc_cons[currcons].vc_decscnm)
#define decom		(vc_cons[currcons].vc_decom)
#define decawm		(vc_cons[currcons].vc_decawm)
#define deccm		(vc_cons[currcons].vc_deccm)
#define decim		(vc_cons[currcons].vc_decim)
#define need_wrap	(vc_cons[currcons].vc_need_wrap)
#define color		(vc_cons[currcons].vc_color)
#define s_color		(vc_cons[currcons].vc_s_color)
#define def_color	(vc_cons[currcons].vc_def_color)
#define	foreground	(color & 0x0f)
#define background	(color & 0xf0)
#define charset		(vc_cons[currcons].vc_charset)
#define s_charset	(vc_cons[currcons].vc_s_charset)
#define	intensity	(vc_cons[currcons].vc_intensity)
#define	underline	(vc_cons[currcons].vc_underline)
#define	blink		(vc_cons[currcons].vc_blink)
#define	reverse		(vc_cons[currcons].vc_reverse)
#define	s_intensity	(vc_cons[currcons].vc_s_intensity)
#define	s_underline	(vc_cons[currcons].vc_s_underline)
#define	s_blink		(vc_cons[currcons].vc_s_blink)
#define	s_reverse	(vc_cons[currcons].vc_s_reverse)
#define	ulcolor		(vc_cons[currcons].vc_ulcolor)
#define	halfcolor	(vc_cons[currcons].vc_halfcolor)
#define tab_stop	(vc_cons[currcons].vc_tab_stop)
#define vcmode		(vt_cons[currcons].vc_mode)
#define vtmode		(vt_cons[currcons].vt_mode)
#define vtpid		(vt_cons[currcons].vt_pid)
#define vtnewvt		(vt_cons[currcons].vt_newvt)

#define set_kbd(x) set_vc_kbd_mode(kbd_table+currcons,x)
#define clr_kbd(x) clr_vc_kbd_mode(kbd_table+currcons,x)
#define is_kbd(x) vc_kbd_mode(kbd_table+currcons,x)

#define decarm		VC_REPEAT
#define decckm		VC_CKMODE
#define kbdapplic	VC_APPLIC
#define kbdraw		VC_RAW
#define lnm		VC_CRLF

int blankinterval = 10*60*HZ;
static int screen_size = 0;

/*
 * this is what the terminal answers to a ESC-Z or csi0c query.
 */
#define VT100ID "\033[?1;2c"
#define VT102ID "\033[?6c"

static unsigned char * translations[] = {
/* 8-bit Latin-1 mapped to the PC character set: '\0' means non-printable */
(unsigned char *)
	"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
	"\0\0\0\0\0\0\0\0\0\0\376\0\0\0\0\0"
	" !\"#$%&'()*+,-./0123456789:;<=>?"
	"@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"
	"`abcdefghijklmnopqrstuvwxyz{|}~\0"
	"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
	"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
	"\377\255\233\234\376\235\174\025\376\376\246\256\252\055\376\376"
	"\370\361\375\376\376\346\024\371\376\376\247\257\254\253\376\250"
	"\376\376\376\376\216\217\222\200\376\220\376\376\376\376\376\376"
	"\376\245\376\376\376\376\231\376\350\376\376\376\232\376\376\341"
	"\205\240\203\376\204\206\221\207\212\202\210\211\215\241\214\213"
	"\376\244\225\242\223\376\224\366\355\227\243\226\201\376\376\230",
/* vt100 graphics */
(unsigned char *)
	"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
	"\0\0\0\0\0\0\0\0\0\0\376\0\0\0\0\0"
	" !\"#$%&'()*+,-./0123456789:;<=>?"
	"@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^ "
	"\004\261\007\007\007\007\370\361\007\007\331\277\332\300\305\304"
	"\304\304\137\137\303\264\301\302\263\363\362\343\330\234\007\0"
	"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
	"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
	"\377\255\233\234\376\235\174\025\376\376\246\256\252\055\376\376"
	"\370\361\375\376\376\346\024\371\376\376\247\257\254\253\376\250"
	"\376\376\376\376\216\217\222\200\376\220\376\376\376\376\376\376"
	"\376\245\376\376\376\376\231\376\376\376\376\376\232\376\376\341"
	"\205\240\203\376\204\206\221\207\212\202\210\211\215\241\214\213"
	"\376\244\225\242\223\376\224\366\376\227\243\226\201\376\376\230",
/* IBM graphics: minimal translations (BS, CR, LF, LL, SO, SI and ESC) */
(unsigned char *)
	"\000\001\002\003\004\005\006\007\000\011\000\013\000\000\000\000"
	"\020\021\022\023\024\025\026\027\030\031\032\000\034\035\036\037"
	"\040\041\042\043\044\045\046\047\050\051\052\053\054\055\056\057"
	"\060\061\062\063\064\065\066\067\070\071\072\073\074\075\076\077"
	"\100\101\102\103\104\105\106\107\110\111\112\113\114\115\116\117"
	"\120\121\122\123\124\125\126\127\130\131\132\133\134\135\136\137"
	"\140\141\142\143\144\145\146\147\150\151\152\153\154\155\156\157"
	"\160\161\162\163\164\165\166\167\170\171\172\173\174\175\176\177"
	"\200\201\202\203\204\205\206\207\210\211\212\213\214\215\216\217"
	"\220\221\222\223\224\225\226\227\230\231\232\233\234\235\236\237"
	"\240\241\242\243\244\245\246\247\250\251\252\253\254\255\256\257"
	"\260\261\262\263\264\265\266\267\270\271\272\273\274\275\276\277"
	"\300\301\302\303\304\305\306\307\310\311\312\313\314\315\316\317"
	"\320\321\322\323\324\325\326\327\330\331\332\333\334\335\336\337"
	"\340\341\342\343\344\345\346\347\350\351\352\353\354\355\356\357"
	"\360\361\362\363\364\365\366\367\370\371\372\373\374\375\376\377",
 /* USER: customizable mappings, initialized as the previous one (IBM) */
(unsigned char *)
	"\000\001\002\003\004\005\006\007\010\011\000\013\000\000\016\017"
	"\020\021\022\023\024\025\026\027\030\031\032\000\034\035\036\037"
	"\040\041\042\043\044\045\046\047\050\051\052\053\054\055\056\057"
	"\060\061\062\063\064\065\066\067\070\071\072\073\074\075\076\077"
	"\100\101\102\103\104\105\106\107\110\111\112\113\114\115\116\117"
	"\120\121\122\123\124\125\126\127\130\131\132\133\134\135\136\137"
	"\140\141\142\143\144\145\146\147\150\151\152\153\154\155\156\157"
	"\160\161\162\163\164\165\166\167\170\171\172\173\174\175\176\177"
	"\200\201\202\203\204\205\206\207\210\211\212\213\214\215\216\217"
	"\220\221\222\223\224\225\226\227\230\231\232\233\234\235\236\237"
	"\240\241\242\243\244\245\246\247\250\251\252\253\254\255\256\257"
	"\260\261\262\263\264\265\266\267\270\271\272\273\274\275\276\277"
	"\300\301\302\303\304\305\306\307\310\311\312\313\314\315\316\317"
	"\320\321\322\323\324\325\326\327\330\331\332\333\334\335\336\337"
	"\340\341\342\343\344\345\346\347\350\351\352\353\354\355\356\357"
	"\360\361\362\363\364\365\366\367\370\371\372\373\374\375\376\377"
};

#define NORM_TRANS (translations[0])
#define GRAF_TRANS (translations[1])
#define NULL_TRANS (translations[2])
#define USER_TRANS (translations[3])

static unsigned char color_table[] = { 0, 4, 2, 6, 1, 5, 3, 7,
				       8,12,10,14, 9,13,11,15 };

/*
 * gotoxy() must verify all boundaries, because the arguments
 * might also be negative. If the given position is out of
 * bounds, the cursor is placed at the nearest margin.
 */
static void gotoxy(int currcons, int new_x, int new_y)
{
	int max_y;

	if (new_x < 0)
		x = 0;
	else
		if (new_x >= video_num_columns)
			x = video_num_columns - 1;
		else
			x = new_x;
 	if (decom) {
		new_y += top;
		max_y = bottom;
	} else
		max_y = video_num_lines;
	if (new_y < 0)
		y = 0;
	else
		if (new_y >= max_y)
			y = max_y - 1;
		else
			y = new_y;
	pos = origin + y*video_size_row + (x<<1);
	need_wrap = 0;
}

/*
 * *Very* limited hardware scrollback support..
 */
static unsigned short __real_origin;
static unsigned short __origin;

static inline void __set_origin(unsigned short offset)
{
	unsigned long flags;
#ifdef CONFIG_SELECTION
	clear_selection();
#endif /* CONFIG_SELECTION */
	save_flags(flags); cli();
	__origin = offset;
	outb_p(12, video_port_reg);
	outb_p(offset >> 8, video_port_val);
	outb_p(13, video_port_reg);
	outb_p(offset, video_port_val);
	restore_flags(flags);
}

void scrollback(int lines)
{
	if (!lines)
		lines = video_num_lines/2;
	lines *= video_num_columns;
	lines = __origin - lines;
	if (lines < 0)
		lines = 0;
	__set_origin(lines);
}

void scrollfront(int lines)
{
	if (!lines)
		lines = video_num_lines/2;
	lines *= video_num_columns;
	lines = __origin + lines;
	if (lines > __real_origin)
		lines = __real_origin;
	__set_origin(lines);
}

static void set_origin(int currcons)
{
	if (video_type != VIDEO_TYPE_EGAC && video_type != VIDEO_TYPE_EGAM)
		return;
	if (currcons != fg_console || console_blanked || vcmode == KD_GRAPHICS)
		return;
	__real_origin = (origin-video_mem_base) >> 1;
	__set_origin(__real_origin);
}

/*
 * Put the cursor just beyond the end of the display adaptor memory.
 */
static inline void hide_cursor(void)
{
  /* This is inefficient, we could just put the cursor at 0xffff,
     but perhaps the delays due to the inefficiency are useful for
     some hardware... */
	outb_p(14, video_port_reg);
	outb_p(0xff&((video_mem_term-video_mem_base)>>9), video_port_val);
	outb_p(15, video_port_reg);
	outb_p(0xff&((video_mem_term-video_mem_base)>>1), video_port_val);
}

static inline void set_cursor(int currcons)
{
	unsigned long flags;

	if (currcons != fg_console || console_blanked || vcmode == KD_GRAPHICS)
		return;
	if (__real_origin != __origin)
		set_origin(__real_origin);
	save_flags(flags); cli();
	if (deccm) {
		outb_p(14, video_port_reg);
		outb_p(0xff&((pos-video_mem_base)>>9), video_port_val);
		outb_p(15, video_port_reg);
		outb_p(0xff&((pos-video_mem_base)>>1), video_port_val);
	} else
		hide_cursor();
	restore_flags(flags);
}

static void scrup(int currcons, unsigned int t, unsigned int b)
{
	int hardscroll = 1;

	if (b > video_num_lines || t >= b)
		return;
	if (video_type != VIDEO_TYPE_EGAC && video_type != VIDEO_TYPE_EGAM)
		hardscroll = 0;
	else if (t || b != video_num_lines)
		hardscroll = 0;
	if (hardscroll) {
		origin += video_size_row;
		pos += video_size_row;
		scr_end += video_size_row;
		if (scr_end > video_mem_end) {
			__asm__("cld\n\t"
				"rep\n\t"
				"movsl\n\t"
				"movl _video_num_columns,%1\n\t"
				"rep\n\t"
				"stosw"
				: /* no output */
				:"a" (video_erase_char),
				"c" ((video_num_lines-1)*video_num_columns>>1),
				"D" (video_mem_start),
				"S" (origin)
				:"cx","di","si");
			scr_end -= origin-video_mem_start;
			pos -= origin-video_mem_start;
			origin = video_mem_start;
		} else {
			__asm__("cld\n\t"
				"rep\n\t"
				"stosw"
				: /* no output */
				:"a" (video_erase_char),
				"c" (video_num_columns),
				"D" (scr_end-video_size_row)
				:"cx","di");
		}
		set_origin(currcons);
	} else {
		__asm__("cld\n\t"
			"rep\n\t"
			"movsl\n\t"
			"movl _video_num_columns,%%ecx\n\t"
			"rep\n\t"
			"stosw"
			: /* no output */
			:"a" (video_erase_char),
			"c" ((b-t-1)*video_num_columns>>1),
			"D" (origin+video_size_row*t),
			"S" (origin+video_size_row*(t+1))
			:"cx","di","si");
	}
}

static void scrdown(int currcons, unsigned int t, unsigned int b)
{
	if (b > video_num_lines || t >= b)
		return;
	__asm__("std\n\t"
		"rep\n\t"
		"movsl\n\t"
		"addl $2,%%edi\n\t"	/* %edi has been decremented by 4 */
		"movl _video_num_columns,%%ecx\n\t"
		"rep\n\t"
		"stosw\n\t"
		"cld"
		: /* no output */
		:"a" (video_erase_char),
		"c" ((b-t-1)*video_num_columns>>1),
		"D" (origin+video_size_row*b-4),
		"S" (origin+video_size_row*(b-1)-4)
		:"ax","cx","di","si");
}

static void lf(int currcons)
{
	if (y+1<bottom) {
		y++;
		pos += video_size_row;
		return;
	} else 
		scrup(currcons,top,bottom);
	need_wrap = 0;
}

static void ri(int currcons)
{
	if (y>top) {
		y--;
		pos -= video_size_row;
		return;
	} else
		scrdown(currcons,top,bottom);
	need_wrap = 0;
}

static inline void cr(int currcons)
{
	pos -= x<<1;
	need_wrap = x = 0;
}

static inline void bs(int currcons)
{
	if (x) {
		pos -= 2;
		x--;
		need_wrap = 0;
	}
}

static inline void del(int currcons)
{
#if 0
	if (x) {
		if (!need_wrap) {    /* this is not the right condition */
		        pos -= 2;
			x--;
		}
		*(unsigned short *)pos = video_erase_char;
		need_wrap = 0;
	}
#endif
}

static void csi_J(int currcons, int vpar)
{
	unsigned long count;
	unsigned long start;

	switch (vpar) {
		case 0:	/* erase from cursor to end of display */
			count = (scr_end-pos)>>1;
			start = pos;
			break;
		case 1:	/* erase from start to cursor */
			count = ((pos-origin)>>1)+1;
			start = origin;
			break;
		case 2: /* erase whole display */
			count = video_num_columns * video_num_lines;
			start = origin;
			break;
		default:
			return;
	}
	__asm__("cld\n\t"
		"rep\n\t"
		"stosw\n\t"
		: /* no output */
		:"c" (count),
		"D" (start),"a" (video_erase_char)
		:"cx","di");
	need_wrap = 0;
}

static void csi_K(int currcons, int vpar)
{
	long count;
	long start;

	switch (vpar) {
		case 0:	/* erase from cursor to end of line */
			count = video_num_columns-x;
			start = pos;
			break;
		case 1:	/* erase from start of line to cursor */
			start = pos - (x<<1);
			count = x+1;
			break;
		case 2: /* erase whole line */
			start = pos - (x<<1);
			count = video_num_columns;
			break;
		default:
			return;
	}
	__asm__("cld\n\t"
		"rep\n\t"
		"stosw\n\t"
		: /* no output */
		:"c" (count),
		"D" (start),"a" (video_erase_char)
		:"cx","di");
	need_wrap = 0;
}

/*
 *  I hope this works. The monochrome part is untested.
 */
static void update_attr(int currcons)
{
	attr = color;
	if (can_do_color) {
		if (underline)
			attr = (attr & 0xf0) | ulcolor;
		else if (intensity == 0)
			attr = (attr & 0xf0) | halfcolor;
	}
	if (reverse ^ decscnm)
		attr = (attr & 0x88) | (((attr >> 4) | (attr << 4)) & 0x77);
	if (blink)
		attr ^= 0x80;
	if (intensity == 2)
		attr ^= 0x08;
	if (!can_do_color) {
		if (underline)
			attr = (attr & 0xf8) | 0x01;
		else if (intensity == 0)
			attr = (attr & 0xf0) | 0x08;
	}
	if (decscnm)
		video_erase_char = (((color & 0x88) | (((color >> 4) | (color << 4)) & 0x77)) << 8) | ' ';
	else
		video_erase_char = (color << 8) | ' ';
}

static void default_attr(int currcons)
{
	intensity = 1;
	underline = 0;
	reverse = 0;
	blink = 0;
	color = def_color;
}

static void csi_m(int currcons)
{
	int i;

	for (i=0;i<=npar;i++)
		switch (par[i]) {
			case 0:	/* all attributes off */
				default_attr(currcons);
				break;
			case 1:
				intensity = 2;
				break;
			case 2:
				intensity = 0;
				break;
			case 4:
				underline = 1;
				break;
			case 5:
				blink = 1;
				break;
			case 7:
				reverse = 1;
				break;
			case 21:
			case 22:
				intensity = 1;
				break;
			case 24:
				underline = 0;
				break;
			case 25:
				blink = 0;
				break;
			case 27:
				reverse = 0;
				break;
			case 39:
				color = (def_color & 0x0f) | background;
				break;
			case 49:
				color = (def_color & 0xf0) | foreground;
				break;
			default:
				if (par[i] >= 30 && par[i] <= 37)
					color = color_table[par[i]-30]
						| background; 
				else if (par[i] >= 40 && par[i] <= 47)
					color = (color_table[par[i]-40]<<4)
						| foreground;
				break;
		}
	update_attr(currcons);
}

static void respond_string(char * p, int currcons, struct tty_struct * tty)
{
	while (*p) {
		put_tty_queue(*p, &tty->read_q);
		p++;
	}
	TTY_READ_FLUSH(tty);
}

static void respond_num(unsigned int n, int currcons, struct tty_struct * tty)
{
	char buff[3];
	int i = 0;

	do {
		buff[i++] = (n%10)+'0';
		n /= 10;
	} while(n && i < 3);	/* We'll take no chances */
	while (i--) {
		put_tty_queue(buff[i], &tty->read_q);
	}
	/* caller must flush */
}

static void cursor_report(int currcons, struct tty_struct * tty)
{
	put_tty_queue('\033', &tty->read_q);
	put_tty_queue('[', &tty->read_q);
	respond_num(y + (decom ? top+1 : 1), currcons, tty);
	put_tty_queue(';', &tty->read_q);
	respond_num(x+1, currcons, tty);
	put_tty_queue('R', &tty->read_q);
	TTY_READ_FLUSH(tty);
}

static inline void status_report(int currcons, struct tty_struct * tty)
{
	respond_string("\033[0n", currcons, tty);	/* Terminal ok */
}

static inline void respond_ID(int currcons, struct tty_struct * tty)
{
	respond_string(VT102ID, currcons, tty);
}

static void invert_screen(int currcons) {
	unsigned char *p;

	if (can_do_color)
		for (p = (unsigned char *)origin+1; p < (unsigned char *)scr_end; p+=2)
			*p = (*p & 0x88) | (((*p >> 4) | (*p << 4)) & 0x77);
	else
		for (p = (unsigned char *)origin+1; p < (unsigned char *)scr_end; p+=2)
			*p ^= *p & 0x07 == 1 ? 0x70 : 0x77;
}

static void set_mode(int currcons, int on_off)
{
	int i;

	for (i=0; i<=npar; i++)
		if (ques) switch(par[i]) {	/* DEC private modes set/reset */
			case 1:			/* Cursor keys send ^[Ox/^[[x */
				if (on_off)
					set_kbd(decckm);
				else
					clr_kbd(decckm);
				break;
			case 3:	/* 80/132 mode switch unimplemented */
				csi_J(currcons,2);
				gotoxy(currcons,0,0);
				break;
			case 5:			/* Inverted screen on/off */
				if (decscnm != on_off) {
					decscnm = on_off;
					invert_screen(currcons);
					update_attr(currcons);
				}
				break;
			case 6:			/* Origin relative/absolute */
				decom = on_off;
				gotoxy(currcons,0,0);
				break;
			case 7:			/* Autowrap on/off */
				decawm = on_off;
				break;
			case 8:			/* Autorepeat on/off */
				if (on_off)
					set_kbd(decarm);
				else
					clr_kbd(decarm);
				break;
			case 25:		/* Cursor on/off */
				deccm = on_off;
				set_cursor(currcons);
				break;
		} else switch(par[i]) {		/* ANSI modes set/reset */
			case 4:			/* Insert Mode on/off */
				decim = on_off;
				break;
			case 20:		/* Lf, Enter == CrLf/Lf */
				if (on_off)
					set_kbd(lnm);
				else
					clr_kbd(lnm);
				break;
		}
}

static void setterm_command(int currcons)
{
	switch(par[0]) {
		case 1:	/* set color for underline mode */
			if (can_do_color && par[1] < 16) {
				ulcolor = color_table[par[1]];
				if (underline)
					update_attr(currcons);
			}
			break;
		case 2:	/* set color for half intensity mode */
			if (can_do_color && par[1] < 16) {
				halfcolor = color_table[par[1]];
				if (intensity == 0)
					update_attr(currcons);
			}
			break;
		case 8:	/* store colors as defaults */
			def_color = attr;
			default_attr(currcons);
			update_attr(currcons);
			break;
		case 9:	/* set blanking interval */
			blankinterval = ((par[1] < 60) ? par[1] : 60) * 60 * HZ;
			break;
	}
}

static void insert_char(int currcons)
{
	unsigned int i = x;
	unsigned short tmp, old = video_erase_char;
	unsigned short * p = (unsigned short *) pos;

	while (i++ < video_num_columns) {
		tmp = *p;
		*p = old;
		old = tmp;
		p++;
	}
	need_wrap = 0;
}

static void insert_line(int currcons)
{
	scrdown(currcons,y,bottom);
	need_wrap = 0;
}

static void delete_char(int currcons)
{
	unsigned int i = x;
	unsigned short * p = (unsigned short *) pos;

	while (++i < video_num_columns) {
		*p = *(p+1);
		p++;
	}
	*p = video_erase_char;
	need_wrap = 0;
}

static void delete_line(int currcons)
{
	scrup(currcons,y,bottom);
	need_wrap = 0;
}

static void csi_at(int currcons, unsigned int nr)
{
	if (nr > video_num_columns)
		nr = video_num_columns;
	else if (!nr)
		nr = 1;
	while (nr--)
		insert_char(currcons);
}

static void csi_L(int currcons, unsigned int nr)
{
	if (nr > video_num_lines)
		nr = video_num_lines;
	else if (!nr)
		nr = 1;
	while (nr--)
		insert_line(currcons);
}

static void csi_P(int currcons, unsigned int nr)
{
	if (nr > video_num_columns)
		nr = video_num_columns;
	else if (!nr)
		nr = 1;
	while (nr--)
		delete_char(currcons);
}

static void csi_M(int currcons, unsigned int nr)
{
	if (nr > video_num_lines)
		nr = video_num_lines;
	else if (!nr)
		nr=1;
	while (nr--)
		delete_line(currcons);
}

static void save_cur(int currcons)
{
	saved_x		= x;
	saved_y		= y;
	s_intensity	= intensity;
	s_underline	= underline;
	s_blink		= blink;
	s_reverse	= reverse;
	s_charset	= charset;
	s_color		= color;
	saved_G0	= G0_charset;
	saved_G1	= G1_charset;
}

static void restore_cur(int currcons)
{
	gotoxy(currcons,saved_x,saved_y);
	intensity	= s_intensity;
	underline	= s_underline;
	blink		= s_blink;
	reverse		= s_reverse;
	charset		= s_charset;
	color		= s_color;
	G0_charset	= saved_G0;
	G1_charset	= saved_G1;
	translate	= charset ? G1_charset : G0_charset;
	update_attr(currcons);
	need_wrap = 0;
}

enum { ESnormal, ESesc, ESsquare, ESgetpars, ESgotpars, ESfunckey, 
	EShash, ESsetG0, ESsetG1, ESignore };

static void reset_terminal(int currcons, int do_clear)
{
	top		= 0;
	bottom		= video_num_lines;
	state		= ESnormal;
	ques		= 0;
	translate	= NORM_TRANS;
	G0_charset	= NORM_TRANS;
	G1_charset	= GRAF_TRANS;
	charset		= 0;
	need_wrap	= 0;

	decscnm		= 0;
	decom		= 0;
	decawm		= 1;
	deccm		= 1;
	decim		= 0;

	set_kbd(decarm);
	clr_kbd(decckm);
	clr_kbd(kbdapplic);
	clr_kbd(lnm);
	kbd_table[currcons].lockstate = 0;
	kbd_table[currcons].ledstate = kbd_table[currcons].default_ledstate;
	set_leds();

	default_attr(currcons);
	update_attr(currcons);

	tab_stop[0]	= 0x01010100;
	tab_stop[1]	=
	tab_stop[2]	=
	tab_stop[3]	=
	tab_stop[4]	= 0x01010101;

	if (do_clear) {
		gotoxy(currcons,0,0);
		csi_J(currcons,2);
		save_cur(currcons);
	}
}

void con_write(struct tty_struct * tty)
{
	int c;
	unsigned int currcons;

	currcons = tty->line - 1;
	if (currcons >= NR_CONSOLES) {
		printk("con_write: illegal tty (%d)\n", currcons);
		return;
	}
#ifdef CONFIG_SELECTION
	/* clear the selection as soon as any characters are to be written
	   out on the console holding the selection. */
	if (!EMPTY(&tty->write_q) && currcons == sel_cons)
		clear_selection();
#endif /* CONFIG_SELECTION */
	disable_bh(KEYBOARD_BH);
	while (!tty->stopped &&	(c = get_tty_queue(&tty->write_q)) >= 0) {
		if (state == ESnormal && translate[c]) {
			if (need_wrap) {
				cr(currcons);
				lf(currcons);
			}
			if (decim)
				insert_char(currcons);
			c = translate[c];
			*(unsigned short *) pos = (attr << 8) + c;
			if (x == video_num_columns - 1)
				need_wrap = decawm;
			else {
				x++;
				pos+=2;
			}
			continue;
		}

		/*
		 *  Control characters can be used in the _middle_
		 *  of an escape sequence.
		 */
		switch (c) {
			case 7:
				kd_mksound(0x637, HZ/8);
				continue;
			case 8:
				bs(currcons);
				continue;
			case 9:
				pos -= (x << 1);
				while (x < video_num_columns - 1) {
					x++;
					if (tab_stop[x >> 5] & (1 << (x & 31)))
						break;
				}
				pos += (x << 1);
				continue;
			case 10: case 11: case 12:
				lf(currcons);
				if (!is_kbd(lnm))
					continue;
			case 13:
				cr(currcons);
				continue;
			case 14:
				charset = 1;
				translate = G1_charset;
				continue;
			case 15:
				charset = 0;
				translate = G0_charset;
				continue;
			case 24: case 26:
				state = ESnormal;
				continue;
			case 27:
				state = ESesc;
				continue;
			case 127:
				del(currcons);
				continue;
			case 128+27:
				state = ESsquare;
				continue;
		}
		switch(state) {
			case ESesc:
				state = ESnormal;
				switch (c) {
				  case '[':
					state = ESsquare;
					continue;
				  case 'E':
					cr(currcons);
					lf(currcons);
					continue;
				  case 'M':
					ri(currcons);
					continue;
				  case 'D':
					lf(currcons);
					continue;
				  case 'H':
					tab_stop[x >> 5] |= (1 << (x & 31));
					continue;
				  case 'Z':
					respond_ID(currcons,tty);
					continue;
				  case '7':
					save_cur(currcons);
					continue;
				  case '8':
					restore_cur(currcons);
					continue;
				  case '(':
					state = ESsetG0;
					continue;
				  case ')':
					state = ESsetG1;
					continue;
				  case '#':
					state = EShash;
					continue;
				  case 'c':
					reset_terminal(currcons,1);
					continue;
				  case '>':  /* Numeric keypad */
					clr_kbd(kbdapplic);
					continue;
				  case '=':  /* Appl. keypad */
					set_kbd(kbdapplic);
				 	continue;
				}	
				continue;
			case ESsquare:
				for(npar = 0 ; npar < NPAR ; npar++)
					par[npar] = 0;
				npar = 0;
				state = ESgetpars;
				if (c == '[') { /* Function key */
					state=ESfunckey;
					continue;
				}
				ques = (c=='?');
				if (ques)
					continue;
			case ESgetpars:
				if (c==';' && npar<NPAR-1) {
					npar++;
					continue;
				} else if (c>='0' && c<='9') {
					par[npar] *= 10;
					par[npar] += c-'0';
					continue;
				} else state=ESgotpars;
			case ESgotpars:
				state = ESnormal;
				switch(c) {
					case 'h':
						set_mode(currcons,1);
						continue;
					case 'l':
						set_mode(currcons,0);
						continue;
					case 'n':
						if (!ques)
							if (par[0] == 5)
								status_report(currcons,tty);
							else if (par[0] == 6)
								cursor_report(currcons,tty);
						continue;
				}
				if (ques) {
					ques = 0;
					continue;
				}
				switch(c) {
					case 'G': case '`':
						if (par[0]) par[0]--;
						gotoxy(currcons,par[0],y);
						continue;
					case 'A':
						if (!par[0]) par[0]++;
						gotoxy(currcons,x,y-par[0]);
						continue;
					case 'B': case 'e':
						if (!par[0]) par[0]++;
						gotoxy(currcons,x,y+par[0]);
						continue;
					case 'C': case 'a':
						if (!par[0]) par[0]++;
						gotoxy(currcons,x+par[0],y);
						continue;
					case 'D':
						if (!par[0]) par[0]++;
						gotoxy(currcons,x-par[0],y);
						continue;
					case 'E':
						if (!par[0]) par[0]++;
						gotoxy(currcons,0,y+par[0]);
						continue;
					case 'F':
						if (!par[0]) par[0]++;
						gotoxy(currcons,0,y-par[0]);
						continue;
					case 'd':
						if (par[0]) par[0]--;
						gotoxy(currcons,x,par[0]);
						continue;
					case 'H': case 'f':
						if (par[0]) par[0]--;
						if (par[1]) par[1]--;
						gotoxy(currcons,par[1],par[0]);
						continue;
					case 'J':
						csi_J(currcons,par[0]);
						continue;
					case 'K':
						csi_K(currcons,par[0]);
						continue;
					case 'L':
						csi_L(currcons,par[0]);
						continue;
					case 'M':
						csi_M(currcons,par[0]);
						continue;
					case 'P':
						csi_P(currcons,par[0]);
						continue;
					case 'c':
						if (!par[0])
							respond_ID(currcons,tty);
						continue;
					case 'g':
						if (!par[0])
							tab_stop[x >> 5] &= ~(1 << (x & 31));
						else if (par[0] == 3) {
							tab_stop[0] =
							tab_stop[1] =
							tab_stop[2] =
							tab_stop[3] =
							tab_stop[4] = 0;
						}
						continue;
					case 'm':
						csi_m(currcons);
						continue;
					case 'r':
						if (!par[0])
							par[0]++;
						if (!par[1])
							par[1] = video_num_lines;
						/* Minimum allowed region is 2 lines */
						if (par[0] < par[1] &&
						    par[1] <= video_num_lines) {
							top=par[0]-1;
							bottom=par[1];
							gotoxy(currcons,0,0);
						}
						continue;
					case 's':
						save_cur(currcons);
						continue;
					case 'u':
						restore_cur(currcons);
						continue;
					case '@':
						csi_at(currcons,par[0]);
						continue;
					case ']': /* setterm functions */
						setterm_command(currcons);
						continue;
				}
				continue;
			case ESfunckey:
				state = ESnormal;
				continue;
			case EShash:
				state = ESnormal;
				if (c == '8') {
					/* DEC screen alignment test. kludge :-) */
					video_erase_char =
						(video_erase_char & 0xff00) | 'E';
					csi_J(currcons, 2);
					video_erase_char =
						(video_erase_char & 0xff00) | ' ';
				}
				continue;
			case ESsetG0:
				if (c == '0')
					G0_charset = GRAF_TRANS;
				else if (c == 'B')
					G0_charset = NORM_TRANS;
				else if (c == 'U')
					G0_charset = NULL_TRANS;
				else if (c == 'K')
					G0_charset = USER_TRANS;
				if (charset == 0)
					translate = G0_charset;
				state = ESnormal;
				continue;
			case ESsetG1:
				if (c == '0')
					G1_charset = GRAF_TRANS;
				else if (c == 'B')
					G1_charset = NORM_TRANS;
				else if (c == 'U')
					G1_charset = NULL_TRANS;
				else if (c == 'K')
					G1_charset = USER_TRANS;
				if (charset == 1)
					translate = G1_charset;
				state = ESnormal;
				continue;
			default:
				state = ESnormal;
		}
	}
	if (vcmode != KD_GRAPHICS)
		set_cursor(currcons);
	enable_bh(KEYBOARD_BH);
	if (LEFT(&tty->write_q) > WAKEUP_CHARS)
		wake_up_interruptible(&tty->write_q.proc_list);
}

void do_keyboard_interrupt(void)
{
	TTY_READ_FLUSH(TTY_TABLE(0));
	timer_active &= ~(1<<BLANK_TIMER);
	if (vt_cons[fg_console].vc_mode == KD_GRAPHICS)
		return;
	if (console_blanked) {
		timer_table[BLANK_TIMER].expires = 0;
		timer_active |= 1<<BLANK_TIMER;
	} else if (blankinterval) {
		timer_table[BLANK_TIMER].expires = jiffies + blankinterval;
		timer_active |= 1<<BLANK_TIMER;
	}
}

void * memsetw(void * s,unsigned short c,int count)
{
__asm__("cld\n\t"
	"rep\n\t"
	"stosw"
	: /* no output */
	:"a" (c),"D" (s),"c" (count)
	:"cx","di");
return s;
}

void console_print(const char * b)
{
	int currcons = fg_console;
	unsigned char c;

	if (!printable || currcons<0 || currcons>=NR_CONSOLES)
		return;
	while ((c = *(b++)) != 0) {
		if (c == 10 || c == 13 || need_wrap) {
			if (c != 13)
				lf(currcons);
			cr(currcons);
			if (c == 10 || c == 13)
				continue;
		}
		*(unsigned short *) pos = (attr << 8) + c;
		if (x == video_num_columns - 1) {
			need_wrap = 1;
			continue;
		}
		x++;
		pos+=2;
	}
	set_cursor(currcons);
	if (vt_cons[fg_console].vc_mode == KD_GRAPHICS)
		return;
	timer_active &= ~(1<<BLANK_TIMER);
	if (console_blanked) {
		timer_table[BLANK_TIMER].expires = 0;
		timer_active |= 1<<BLANK_TIMER;
	} else if (blankinterval) {
		timer_table[BLANK_TIMER].expires = jiffies + blankinterval;
		timer_active |= 1<<BLANK_TIMER;
	}
}

/*
 *  long con_init(long);
 *
 * This routine initalizes console interrupts, and does nothing
 * else. If you want the screen to clear, call tty_write with
 * the appropriate escape-sequece.
 *
 * Reads the information preserved by setup.s to determine the current display
 * type and sets everything accordingly.
 */
long con_init(long kmem_start)
{
	char *display_desc = "????";
	int currcons = 0;
	long base;
	int orig_x = ORIG_X;
	int orig_y = ORIG_Y;

	vc_scrmembuf = (unsigned short *) kmem_start;
	video_num_columns = ORIG_VIDEO_COLS;
	video_size_row = video_num_columns * 2;
	video_num_lines = ORIG_VIDEO_LINES;
	video_page = ORIG_VIDEO_PAGE;
	screen_size = (video_num_lines * video_size_row);
	kmem_start += NR_CONSOLES * screen_size;
	timer_table[BLANK_TIMER].fn = blank_screen;
	timer_table[BLANK_TIMER].expires = 0;
	if (blankinterval) {
		timer_table[BLANK_TIMER].expires = jiffies+blankinterval;
		timer_active |= 1<<BLANK_TIMER;
	}
	
	if (ORIG_VIDEO_MODE == 7)	/* Is this a monochrome display? */
	{
		video_mem_base = 0xb0000;
		video_port_reg = 0x3b4;
		video_port_val = 0x3b5;
		if ((ORIG_VIDEO_EGA_BX & 0xff) != 0x10)
		{
			video_type = VIDEO_TYPE_EGAM;
			video_mem_term = 0xb8000;
			display_desc = "EGA+";
		}
		else
		{
			video_type = VIDEO_TYPE_MDA;
			video_mem_term = 0xb2000;
			display_desc = "*MDA";
		}
	}
	else				/* If not, it is color. */
	{
		can_do_color = 1;
		video_mem_base = 0xb8000;
		video_port_reg	= 0x3d4;
		video_port_val	= 0x3d5;
		if ((ORIG_VIDEO_EGA_BX & 0xff) != 0x10)
		{
			video_type = VIDEO_TYPE_EGAC;
			video_mem_term = 0xc0000;
			display_desc = "EGA+";
		}
		else
		{
			video_type = VIDEO_TYPE_CGA;
			video_mem_term = 0xba000;
			display_desc = "*CGA";
		}
	}
	
	/* Initialize the variables used for scrolling (mostly EGA/VGA)	*/

	base = (long)vc_scrmembuf;
	for (currcons = 0; currcons<NR_CONSOLES; currcons++) {
		pos = origin = video_mem_start = base;
		scr_end = video_mem_end = (base += screen_size);
		vc_scrbuf[currcons] = (unsigned short *) origin;
		vcmode		= KD_TEXT;
		vtmode.mode	= VT_AUTO;
		vtmode.waitv	= 0;
		vtmode.relsig	= 0;
		vtmode.acqsig	= 0;
		vtmode.frsig	= 0;
		vtpid		= -1;
		vtnewvt		= -1;
		clr_kbd(kbdraw);
		def_color	= 0x07;   /* white */
		ulcolor		= 0x0f;   /* bold white */
		halfcolor	= 0x08;   /* grey */
		reset_terminal(currcons, currcons);
	}
	currcons = fg_console = 0;

	video_mem_start = video_mem_base;
	video_mem_end = video_mem_term;
	origin = video_mem_start;
	scr_end	= video_mem_start + video_num_lines * video_size_row;
	gotoxy(currcons,0,0);
	save_cur(currcons);
	gotoxy(currcons,orig_x,orig_y);
	update_screen(fg_console);
	printable = 1;
	printk("Console: %s %s %ldx%ld, %d virtual consoles\n",
		can_do_color?"colour":"mono",
		display_desc,
		video_num_columns,video_num_lines,
		NR_CONSOLES);
	register_console(console_print);
	return kmem_start;
}

/*
 * kbdsave doesn't need to do anything: it's all handled automatically
 * with the new data structures..
 */
void kbdsave(int new_console)
{
}

static void get_scrmem(int currcons)
{
	memcpy((void *)vc_scrbuf[currcons],(void *)origin, screen_size);
	video_mem_start = (unsigned long)vc_scrbuf[currcons];
	origin 	= video_mem_start;
	scr_end = video_mem_end = video_mem_start+screen_size;
	pos = origin + y*video_size_row + (x<<1);
}

static void set_scrmem(int currcons)
{
#ifdef CONFIG_HGA
  /* This works with XFree86 1.2, 1.3 and 2.0
     This code could be extended and made more generally useful if we could
     determine the actual video mode. It appears that this should be
     possible on a genuine Hercules card, but I (WM) haven't been able to
     read from any of the required registers on my clone card.
     */
	/* This code should work with Hercules and MDA cards. */
	if (video_type == VIDEO_TYPE_MDA)
	  {
	    if (vcmode == KD_TEXT)
	      {
		/* Ensure that the card is in text mode. */
		int	i;
		static char herc_txt_tbl[12] = {
		  0x61,0x50,0x52,0x0f,0x19,6,0x19,0x19,2,0x0d,0x0b,0x0c };
		outb_p(0, 0x3bf);  /* Back to power-on defaults */
		outb_p(0, 0x3b8);  /* Blank the screen, select page 0, etc */
		for ( i = 0 ; i < 12 ; i++ )
		  {
		    outb_p(i, 0x3b4);
		    outb_p(herc_txt_tbl[i], 0x3b5);
		  }
	      }
#define HGA_BLINKER_ON 0x20
#define HGA_SCREEN_ON  8
	    /* Make sure that the hardware is not blanked */
	    outb_p(HGA_BLINKER_ON | HGA_SCREEN_ON, 0x3b8);
	  }
#endif CONFIG_HGA

	video_mem_start = video_mem_base;
	video_mem_end = video_mem_term;
	origin	= video_mem_start;
	scr_end	= video_mem_start + screen_size;
	pos = origin + y*video_size_row + (x<<1);
	memcpy((void *)video_mem_base, (void *)vc_scrbuf[fg_console], screen_size);
}

void blank_screen(void)
{
	if (console_blanked)
		return;
	timer_table[BLANK_TIMER].fn = unblank_screen;
	get_scrmem(fg_console);
	hide_cursor();
	console_blanked = 1;
	memsetw((void *)video_mem_base, 0x0020, video_mem_term-video_mem_base );
}

void unblank_screen(void)
{
	if (!console_blanked)
		return;
	timer_table[BLANK_TIMER].fn = blank_screen;
	if (blankinterval) {
		timer_table[BLANK_TIMER].expires = jiffies + blankinterval;
		timer_active |= 1<<BLANK_TIMER;
	}
	console_blanked = 0;
	set_scrmem(fg_console);
	set_origin(fg_console);
	set_cursor(fg_console);
}

void update_screen(int new_console)
{
	static int lock = 0;

	if (new_console == fg_console || lock)
		return;
	lock = 1;
	kbdsave(new_console);
	get_scrmem(fg_console); 
	fg_console = new_console;
	set_scrmem(fg_console); 
	set_origin(fg_console);
	set_cursor(new_console);
	set_leds();
	compute_shiftstate();
	lock = 0;
}

int do_screendump(int arg)
{
	char *sptr, *buf = (char *)arg;
	int currcons, l;

	if (!suser())
		return -EPERM;
	l = verify_area(VERIFY_WRITE, buf,2+video_num_columns*video_num_lines);
	if (l)
		return l;
	currcons = get_fs_byte(buf+1);
	if ((currcons<0) || (currcons>NR_CONSOLES))
		return -EIO;
	put_fs_byte((char)(video_num_lines),buf++);	
	put_fs_byte((char)(video_num_columns),buf++);
	currcons = (currcons ? currcons-1 : fg_console);
	sptr = (char *) origin;
	for (l=video_num_lines*video_num_columns; l>0 ; l--, sptr++)
		put_fs_byte(*sptr++,buf++);	
	return(0);
}

/*
 * All we do is set the write and ioctl subroutines; later on maybe we'll
 * dynamically allocate the console screen memory.
 */
int con_open(struct tty_struct *tty, struct file * filp)
{
	tty->write = con_write;
	tty->ioctl = vt_ioctl;
	if (tty->line > NR_CONSOLES)
		return -ENODEV;
	return 0;
}

#ifdef CONFIG_SELECTION
/* correction factor for when screen is hardware-scrolled */
#define	hwscroll_offset (currcons == fg_console ? ((__real_origin - __origin) << 1) : 0)

/* set reverse video on characters s-e of console with selection. */
static void highlight(const int currcons, const int s, const int e)
{
	unsigned char *p, *p1, *p2;

	p1 = (unsigned char *)origin - hwscroll_offset + s + 1;
	p2 = (unsigned char *)origin - hwscroll_offset + e + 1;
	if (p1 > p2)
	{
		p = p1;
		p1 = p2;
		p2 = p;
	}
	for (p = p1; p <= p2; p += 2)
		*p = (*p & 0x88) | ((*p << 4) & 0x70) | ((*p >> 4) & 0x07);
}

/* is c in range [a-zA-Z0-9_]? */
static inline int inword(const char c) { return (isalnum(c) || c == '_'); }

/* does screen address p correspond to character at LH/RH edge of screen? */
static inline int atedge(const int p)
{
	return (!(p % video_size_row) || !((p + 2) % video_size_row));
}

/* constrain v such that l <= v <= u */
static inline short limit(const int v, const int l, const int u)
{
	return (v < l) ? l : ((v > u) ? u : v);
}

/* set the current selection. Invoked by ioctl(). */
int set_selection(const int arg)
{
	unsigned short *args, xs, ys, xe, ye;
	int currcons = fg_console;
	int sel_mode, new_sel_start, new_sel_end, spc;
	char *bp, *obp, *spos;
	int i, ps, pe;
	char *off = (char *)origin - hwscroll_offset;

	unblank_screen();
	args = (unsigned short *)(arg + 1);
	xs = get_fs_word(args++) - 1;
	ys = get_fs_word(args++) - 1;
	xe = get_fs_word(args++) - 1;
	ye = get_fs_word(args++) - 1;
	sel_mode = get_fs_word(args);

	xs = limit(xs, 0, video_num_columns - 1);
	ys = limit(ys, 0, video_num_lines - 1);
	xe = limit(xe, 0, video_num_columns - 1);
	ye = limit(ye, 0, video_num_lines - 1);
	ps = ys * video_size_row + (xs << 1);
	pe = ye * video_size_row + (xe << 1);

	if (ps > pe)	/* make sel_start <= sel_end */
	{
		int tmp = ps;
		ps = pe;
		pe = tmp;
	}

	switch (sel_mode)
	{
		case 0:	/* character-by-character selection */
		default:
			new_sel_start = ps;
			new_sel_end = pe;
			break;
		case 1:	/* word-by-word selection */
			spc = isspace(*(off + ps));
			for (new_sel_start = ps; ; ps -= 2)
			{
				if ((spc && !isspace(*(off + ps))) ||
				    (!spc && !inword(*(off + ps))))
					break;
				new_sel_start = ps;
				if (!(ps % video_size_row))
					break;
			}
			spc = isspace(*(off + pe));
			for (new_sel_end = pe; ; pe += 2)
			{
				if ((spc && !isspace(*(off + pe))) ||
				    (!spc && !inword(*(off + pe))))
					break;
				new_sel_end = pe;
				if (!((pe + 2) % video_size_row))
					break;
			}
			break;
		case 2:	/* line-by-line selection */
			new_sel_start = ps - ps % video_size_row;
			new_sel_end = pe + video_size_row
				    - pe % video_size_row - 2;
			break;
	}
	/* select to end of line if on trailing space */
	if (new_sel_end > new_sel_start &&
		!atedge(new_sel_end) && isspace(*(off + new_sel_end)))
	{
		for (pe = new_sel_end + 2; ; pe += 2)
		{
			if (!isspace(*(off + pe)) || atedge(pe))
				break;
		}
		if (isspace(*(off + pe)))
			new_sel_end = pe;
	}
	if (sel_cons != currcons)
	{
		clear_selection();
		sel_cons = currcons;
	}
	if (sel_start == -1)	/* no current selection */
		highlight(sel_cons, new_sel_start, new_sel_end);
	else if (new_sel_start == sel_start)
	{
		if (new_sel_end == sel_end)	/* no action required */
			return 0;
		else if (new_sel_end > sel_end)	/* extend to right */
			highlight(sel_cons, sel_end + 2, new_sel_end);
		else				/* contract from right */
			highlight(sel_cons, new_sel_end + 2, sel_end);
	}
	else if (new_sel_end == sel_end)
	{
		if (new_sel_start < sel_start)	/* extend to left */
			highlight(sel_cons, new_sel_start, sel_start - 2);
		else				/* contract from left */
			highlight(sel_cons, sel_start, new_sel_start - 2);
	}
	else	/* some other case; start selection from scratch */
	{
		clear_selection();
		highlight(sel_cons, new_sel_start, new_sel_end);
	}
	sel_start = new_sel_start;
	sel_end = new_sel_end;
	obp = bp = sel_buffer;
	for (i = sel_start; i <= sel_end; i += 2)
	{
		spos = (char *)off + i;
		*bp++ = *spos;
		if (!isspace(*spos))
			obp = bp;
		if (! ((i + 2) % video_size_row))
		{
			/* strip trailing blanks from line and add newline,
			   unless non-space at end of line. */
			if (obp != bp)
			{
				bp = obp;
				*bp++ = '\r';
			}
			obp = bp;
		}
		/* check for space, leaving room for next character, possible
		   newline, and null at end. */
		if (bp - sel_buffer > SEL_BUFFER_SIZE - 3)
			break;
	}
	*bp = '\0';
	return 0;
}

/* insert the contents of the selection buffer into the queue of the
   tty associated with the current console. Invoked by ioctl(). */
int paste_selection(struct tty_struct *tty)
{
	char *bp = sel_buffer;

	if (! *bp)
		return 0;
	unblank_screen();
	while (*bp) {
		put_tty_queue(*bp, &tty->read_q);
		bp++;
		TTY_READ_FLUSH(tty);
	}
	return 0;
}

/* remove the current selection highlight, if any, from the console holding
   the selection. */
static void clear_selection()
{
	if (sel_start != -1)
	{
		highlight(sel_cons, sel_start, sel_end);
		sel_start = -1;
	}
}
#endif /* CONFIG_SELECTION */

/*
 * PIO_FONT support.
 *
 * The font loading code goes back to the codepage package by
 * Joel Hoffman (joel@wam.umd.edu). (He reports that the original
 * reference is: "From: p. 307 of _Programmer's Guide to PC & PS/2
 * Video Systems_ by Richard Wilton. 1987.  Microsoft Press".)
 *
 * Change for certain monochrome monitors by Yury Shevchuck
 * (sizif@botik.yaroslavl.su).
 */

#define colourmap ((char *)0xa0000)
#define blackwmap ((char *)0xb0000)
#define cmapsz 8192
#define seq_port_reg (0x3c4)
#define seq_port_val (0x3c5)
#define gr_port_reg (0x3ce)
#define gr_port_val (0x3cf)

static int set_get_font(char * arg, int set)
{
#ifdef CAN_LOAD_EGA_FONTS
	int i;
	char *charmap;
	int beg;

	/* no use to "load" CGA... */

	if (video_type == VIDEO_TYPE_EGAC) {
		charmap = colourmap;
		beg = 0x0e;
	} else if (video_type == VIDEO_TYPE_EGAM) {
		charmap = blackwmap;
		beg = 0x0a;
	} else
		return -EINVAL;

	i = verify_area(set ? VERIFY_READ : VERIFY_WRITE, (void *)arg, cmapsz);
	if (i)
	        return i;

	cli();
	outb_p( 0x00, seq_port_reg );   /* First, the sequencer */
	outb_p( 0x01, seq_port_val );   /* Synchronous reset */
	outb_p( 0x02, seq_port_reg );
	outb_p( 0x04, seq_port_val );   /* CPU writes only to map 2 */
	outb_p( 0x04, seq_port_reg );
	outb_p( 0x07, seq_port_val );   /* Sequential addressing */
	outb_p( 0x00, seq_port_reg );
	outb_p( 0x03, seq_port_val );   /* Clear synchronous reset */

	outb_p( 0x04, gr_port_reg );    /* Now, the graphics controller */
	outb_p( 0x02, gr_port_val );    /* select map 2 */
	outb_p( 0x05, gr_port_reg );
	outb_p( 0x00, gr_port_val );    /* disable odd-even addressing */
	outb_p( 0x06, gr_port_reg );
	outb_p( 0x00, gr_port_val );    /* map start at A000:0000 */
	sti();

	if (set)
		for (i=0; i<cmapsz ; i++)
			*(charmap+i) = get_fs_byte(arg+i);
	else
		for (i=0; i<cmapsz ; i++)
			put_fs_byte(*(charmap+i), arg+i);

	cli();
	outb_p( 0x00, seq_port_reg );   /* Frist, the sequencer */
	outb_p( 0x01, seq_port_val );   /* Synchronous reset */
	outb_p( 0x02, seq_port_reg );
	outb_p( 0x03, seq_port_val );   /* CPU writes to maps 0 and 1 */
	outb_p( 0x04, seq_port_reg );
	outb_p( 0x03, seq_port_val );   /* odd-even addressing */
	outb_p( 0x00, seq_port_reg );
	outb_p( 0x03, seq_port_val );   /* clear synchronous reset */

	outb_p( 0x04, gr_port_reg );    /* Now, the graphics controller */
	outb_p( 0x00, gr_port_val );    /* select map 0 for CPU */
	outb_p( 0x05, gr_port_reg );
	outb_p( 0x10, gr_port_val );    /* enable even-odd addressing */
	outb_p( 0x06, gr_port_reg );
	outb_p( beg, gr_port_val );     /* map starts at b800:0 or b000:0 */
	sti();

	return 0;
#else
	return -EINVAL;
#endif
}

/*
 * Load font into the EGA/VGA character generator. arg points to a 8192
 * byte map, 32 bytes per character. Only first H of them are used for
 * 8xH fonts (0 < H <= 32).
 */

int con_set_font (char *arg)
{
	return set_get_font (arg,1);
}

int con_get_font (char *arg)
{
	return set_get_font (arg,0);
}

/*
 * Load customizable translation table (USER_TRANS[]). All checks are here,
 * so we need only include 'return con_set_trans(arg)' in the ioctl handler
 * arg points to a 256 byte translation table.
 */
int con_set_trans(char * arg)
{
	int i;

	i = verify_area(VERIFY_READ, (void *)arg, E_TABSZ);
	if (i)
	        return i;

	for (i=0; i<E_TABSZ ; i++) USER_TRANS[i] = get_fs_byte(arg+i);
	USER_TRANS[012]=0;
	USER_TRANS[014]=0;
	USER_TRANS[015]=0;
	USER_TRANS[033]=0;
	return 0;
}

int con_get_trans(char * arg)
{
	int i;

	i = verify_area(VERIFY_WRITE, (void *)arg, E_TABSZ);
	if (i)
	        return i;

	for (i=0; i<E_TABSZ ; i++) put_fs_byte(USER_TRANS[i],arg+i);
	return 0;
}
