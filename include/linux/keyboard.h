#ifndef __LINUX_KEYBOARD_H
#define __LINUX_KEYBOARD_H

#define KG_SHIFT	0
#define KG_CTRL		2
#define KG_ALT		3
#define KG_ALTGR	1
#define KG_SHIFTL       4
#define KG_SHIFTR       5
#define KG_CTRLL        6
#define KG_CTRLR        7

#define NR_KEYS 128
#define NR_KEYMAPS 16
extern const int NR_TYPES;
extern const int max_vals[];
extern unsigned short key_map[NR_KEYMAPS][NR_KEYS];

#define NR_FUNC 36
#define FUNC_BUFSIZE 512
extern char func_buf[FUNC_BUFSIZE];
extern char *func_table[NR_FUNC];

#define KT_LATIN	0	/* we depend on this being zero */
#define KT_LETTER      11	/* symbol that can be acted upon by CapsLock */
#define KT_FN		1
#define KT_SPEC		2
#define KT_PAD		3
#define KT_DEAD		4
#define KT_CONS		5
#define KT_CUR		6
#define KT_SHIFT	7
#define KT_META		8
#define KT_ASCII	9
#define KT_LOCK		10

#define K(t,v)		(((t)<<8)|(v))
#define KTYP(x)		((x) >> 8)
#define KVAL(x)		((x) & 0xff)

#define K_F1		K(KT_FN,0)
#define K_F2		K(KT_FN,1)
#define K_F3		K(KT_FN,2)
#define K_F4		K(KT_FN,3)
#define K_F5		K(KT_FN,4)
#define K_F6		K(KT_FN,5)
#define K_F7		K(KT_FN,6)
#define K_F8		K(KT_FN,7)
#define K_F9		K(KT_FN,8)
#define K_F10		K(KT_FN,9)
#define K_F11		K(KT_FN,10)
#define K_F12		K(KT_FN,11)
#define K_F13		K(KT_FN,12)
#define K_F14		K(KT_FN,13)
#define K_F15		K(KT_FN,14)
#define K_F16		K(KT_FN,15)
#define K_F17		K(KT_FN,16)
#define K_F18		K(KT_FN,17)
#define K_F19		K(KT_FN,18)
#define K_F20		K(KT_FN,19)
#define K_FIND		K(KT_FN,20)
#define K_INSERT	K(KT_FN,21)
#define K_REMOVE	K(KT_FN,22)
#define K_SELECT	K(KT_FN,23)
#define K_PGUP		K(KT_FN,24)
#define K_PGDN		K(KT_FN,25)
#define K_MACRO         K(KT_FN,26)
#define K_HELP          K(KT_FN,27)
#define K_DO            K(KT_FN,28)
#define K_PAUSE         K(KT_FN,29)

#define K_HOLE		K(KT_SPEC,0)
#define K_ENTER		K(KT_SPEC,1)
#define K_SH_REGS	K(KT_SPEC,2)
#define K_SH_MEM	K(KT_SPEC,3)
#define K_SH_STAT	K(KT_SPEC,4)
#define K_BREAK		K(KT_SPEC,5)
#define K_CONS		K(KT_SPEC,6)
#define K_CAPS		K(KT_SPEC,7)
#define K_NUM		K(KT_SPEC,8)
#define K_HOLD		K(KT_SPEC,9)
#define K_SCROLLFORW	K(KT_SPEC,10)
#define K_SCROLLBACK	K(KT_SPEC,11)
#define K_BOOT		K(KT_SPEC,12)
#define K_CAPSON	K(KT_SPEC,13)
#define K_COMPOSE       K(KT_SPEC,14)

#define K_P0		K(KT_PAD,0)
#define K_P1		K(KT_PAD,1)
#define K_P2		K(KT_PAD,2)
#define K_P3		K(KT_PAD,3)
#define K_P4		K(KT_PAD,4)
#define K_P5		K(KT_PAD,5)
#define K_P6		K(KT_PAD,6)
#define K_P7		K(KT_PAD,7)
#define K_P8		K(KT_PAD,8)
#define K_P9		K(KT_PAD,9)
#define K_PPLUS		K(KT_PAD,10)	/* key-pad plus			   */
#define K_PMINUS	K(KT_PAD,11)	/* key-pad minus		   */
#define K_PSTAR		K(KT_PAD,12)	/* key-pad asterisk (star)	   */
#define K_PSLASH	K(KT_PAD,13)	/* key-pad slash		   */
#define K_PENTER	K(KT_PAD,14)	/* key-pad enter		   */
#define K_PCOMMA	K(KT_PAD,15)	/* key-pad comma: kludge...	   */
#define K_PDOT		K(KT_PAD,16)	/* key-pad dot (period): kludge... */
#define K_PPLUSMINUS    K(KT_PAD,17)    /* key-pad plus/minus              */

#define K_DGRAVE	K(KT_DEAD,0)
#define K_DACUTE	K(KT_DEAD,1)
#define K_DCIRCM	K(KT_DEAD,2)
#define K_DTILDE	K(KT_DEAD,3)
#define K_DDIERE	K(KT_DEAD,4)

#define K_DOWN		K(KT_CUR,0)
#define K_LEFT		K(KT_CUR,1)
#define K_RIGHT		K(KT_CUR,2)
#define K_UP		K(KT_CUR,3)

#define K_SHIFT		K(KT_SHIFT,KG_SHIFT)
#define K_CTRL		K(KT_SHIFT,KG_CTRL)
#define K_ALT		K(KT_SHIFT,KG_ALT)
#define K_ALTGR		K(KT_SHIFT,KG_ALTGR)
#define K_SHIFTL        K(KT_SHIFT,KG_SHIFTL)
#define K_SHIFTR        K(KT_SHIFT,KG_SHIFTR)
#define K_CTRLL         K(KT_SHIFT,KG_CTRLL)
#define K_CTRLR         K(KT_SHIFT,KG_CTRLR)

#define NR_SHIFT	4

#define K_CAPSSHIFT	K(KT_SHIFT,NR_SHIFT)

#define K_ASC0		K(KT_ASCII,0)
#define K_ASC1		K(KT_ASCII,1)
#define K_ASC2		K(KT_ASCII,2)
#define K_ASC3		K(KT_ASCII,3)
#define K_ASC4		K(KT_ASCII,4)
#define K_ASC5		K(KT_ASCII,5)
#define K_ASC6		K(KT_ASCII,6)
#define K_ASC7		K(KT_ASCII,7)
#define K_ASC8		K(KT_ASCII,8)
#define K_ASC9		K(KT_ASCII,9)

#define K_SHIFTLOCK	K(KT_LOCK,KG_SHIFT)
#define K_CTRLLOCK	K(KT_LOCK,KG_CTRL)
#define K_ALTLOCK	K(KT_LOCK,KG_ALT)
#define K_ALTGRLOCK	K(KT_LOCK,KG_ALTGR)

#define MAX_DIACR       256
#endif
