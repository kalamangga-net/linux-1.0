#ifndef _KBD_KERN_H
#define _KBD_KERN_H

#include <linux/interrupt.h>
#define set_leds() mark_bh(KEYBOARD_BH)

#include <linux/keyboard.h>

/*
 * kbd->xxx contains the VC-local things (flag settings etc..)
 *
 * Note: externally visible are LED_SCR, LED_NUM, LED_CAP defined in kd.h
 *       The code in KDGETLED / KDSETLED depends on the internal and
 *       external order being the same.
 *
 * Note: lockstate is used as index in the array key_map.
 */
struct kbd_struct {
        unsigned char ledstate;		/* 3 bits */
	unsigned char default_ledstate;
#define VC_SCROLLOCK	0	/* scroll-lock mode */
#define VC_NUMLOCK	1	/* numeric lock mode */
#define VC_CAPSLOCK	2	/* capslock mode */

	unsigned char lockstate;	/* 4 bits - must be in 0..15 */
#define VC_SHIFTLOCK	KG_SHIFT	/* shift lock mode */
#define VC_ALTGRLOCK	KG_ALTGR	/* altgr lock mode */
#define VC_CTRLLOCK	KG_CTRL 	/* control lock mode */
#define VC_ALTLOCK	KG_ALT  	/* alt lock mode */

	unsigned char modeflags;
#define VC_APPLIC	0	/* application key mode */
#define VC_CKMODE	1	/* cursor key mode */
#define VC_REPEAT	2	/* keyboard repeat */
#define VC_CRLF		3	/* 0 - enter sends CR, 1 - enter sends CRLF */
#define VC_META		4	/* 0 - meta, 1 - meta=prefix with ESC */
#define VC_PAUSE	5	/* pause key pressed - unused */
#define VC_RAW		6	/* raw (scancode) mode */
#define VC_MEDIUMRAW	7	/* medium raw (keycode) mode */
};

extern struct kbd_struct kbd_table[];


extern unsigned long kbd_init(unsigned long);

extern inline int vc_kbd_led(struct kbd_struct * kbd, int flag)
{
	return ((kbd->ledstate >> flag) & 1);
}

extern inline int vc_kbd_lock(struct kbd_struct * kbd, int flag)
{
	return ((kbd->lockstate >> flag) & 1);
}

extern inline int vc_kbd_mode(struct kbd_struct * kbd, int flag)
{
	return ((kbd->modeflags >> flag) & 1);
}

extern inline void set_vc_kbd_led(struct kbd_struct * kbd, int flag)
{
	kbd->ledstate |= 1 << flag;
}

extern inline void set_vc_kbd_lock(struct kbd_struct * kbd, int flag)
{
	kbd->lockstate |= 1 << flag;
}

extern inline void set_vc_kbd_mode(struct kbd_struct * kbd, int flag)
{
	kbd->modeflags |= 1 << flag;
}

extern inline void clr_vc_kbd_led(struct kbd_struct * kbd, int flag)
{
	kbd->ledstate &= ~(1 << flag);
}

extern inline void clr_vc_kbd_lock(struct kbd_struct * kbd, int flag)
{
	kbd->lockstate &= ~(1 << flag);
}

extern inline void clr_vc_kbd_mode(struct kbd_struct * kbd, int flag)
{
	kbd->modeflags &= ~(1 << flag);
}

extern inline void chg_vc_kbd_led(struct kbd_struct * kbd, int flag)
{
	kbd->ledstate ^= 1 << flag;
}

extern inline void chg_vc_kbd_lock(struct kbd_struct * kbd, int flag)
{
	kbd->lockstate ^= 1 << flag;
}

extern inline void chg_vc_kbd_mode(struct kbd_struct * kbd, int flag)
{
	kbd->modeflags ^= 1 << flag;
}

#endif
