#ifndef _ASM_SEGMENT_H
#define _ASM_SEGMENT_H

static inline unsigned char get_user_byte(const char * addr)
{
	register unsigned char _v;

	__asm__ ("movb %%fs:%1,%0":"=q" (_v):"m" (*addr));
	return _v;
}

#define get_fs_byte(addr) get_user_byte((char *)(addr))

static inline unsigned short get_user_word(const short *addr)
{
	unsigned short _v;

	__asm__ ("movw %%fs:%1,%0":"=r" (_v):"m" (*addr));
	return _v;
}

#define get_fs_word(addr) get_user_word((short *)(addr))

static inline unsigned long get_user_long(const int *addr)
{
	unsigned long _v;

	__asm__ ("movl %%fs:%1,%0":"=r" (_v):"m" (*addr)); \
	return _v;
}

#define get_fs_long(addr) get_user_long((int *)(addr))

static inline void put_user_byte(char val,char *addr)
{
__asm__ ("movb %0,%%fs:%1": /* no outputs */ :"iq" (val),"m" (*addr));
}

#define put_fs_byte(x,addr) put_user_byte((x),(char *)(addr))

static inline void put_user_word(short val,short * addr)
{
__asm__ ("movw %0,%%fs:%1": /* no outputs */ :"ir" (val),"m" (*addr));
}

#define put_fs_word(x,addr) put_user_word((x),(short *)(addr))

static inline void put_user_long(unsigned long val,int * addr)
{
__asm__ ("movl %0,%%fs:%1": /* no outputs */ :"ir" (val),"m" (*addr));
}

#define put_fs_long(x,addr) put_user_long((x),(int *)(addr))

static inline void __generic_memcpy_tofs(void * to, const void * from, unsigned long n)
{
__asm__("cld\n\t"
	"push %%es\n\t"
	"push %%fs\n\t"
	"pop %%es\n\t"
	"testb $1,%%cl\n\t"
	"je 1f\n\t"
	"movsb\n"
	"1:\ttestb $2,%%cl\n\t"
	"je 2f\n\t"
	"movsw\n"
	"2:\tshrl $2,%%ecx\n\t"
	"rep ; movsl\n\t"
	"pop %%es"
	: /* no outputs */
	:"c" (n),"D" ((long) to),"S" ((long) from)
	:"cx","di","si");
}

static inline void __constant_memcpy_tofs(void * to, const void * from, unsigned long n)
{
	switch (n) {
		case 0:
			return;
		case 1:
			put_user_byte(*(const char *) from, (char *) to);
			return;
		case 2:
			put_user_word(*(const short *) from, (short *) to);
			return;
		case 3:
			put_user_word(*(const short *) from, (short *) to);
			put_user_byte(*(2+(const char *) from), 2+(char *) to);
			return;
		case 4:
			put_user_long(*(const int *) from, (int *) to);
			return;
	}
#define COMMON(x) \
__asm__("cld\n\t" \
	"push %%es\n\t" \
	"push %%fs\n\t" \
	"pop %%es\n\t" \
	"rep ; movsl\n\t" \
	x \
	"pop %%es" \
	: /* no outputs */ \
	:"c" (n/4),"D" ((long) to),"S" ((long) from) \
	:"cx","di","si")

	switch (n % 4) {
		case 0:
			COMMON("");
			return;
		case 1:
			COMMON("movsb\n\t");
			return;
		case 2:
			COMMON("movsw\n\t");
			return;
		case 3:
			COMMON("movsw\n\tmovsb\n\t");
			return;
	}
#undef COMMON
}

static inline void __generic_memcpy_fromfs(void * to, const void * from, unsigned long n)
{
__asm__("cld\n\t"
	"testb $1,%%cl\n\t"
	"je 1f\n\t"
	"fs ; movsb\n"
	"1:\ttestb $2,%%cl\n\t"
	"je 2f\n\t"
	"fs ; movsw\n"
	"2:\tshrl $2,%%ecx\n\t"
	"rep ; fs ; movsl"
	: /* no outputs */
	:"c" (n),"D" ((long) to),"S" ((long) from)
	:"cx","di","si","memory");
}

static inline void __constant_memcpy_fromfs(void * to, const void * from, unsigned long n)
{
	switch (n) {
		case 0:
			return;
		case 1:
			*(char *)to = get_user_byte((const char *) from);
			return;
		case 2:
			*(short *)to = get_user_word((const short *) from);
			return;
		case 3:
			*(short *) to = get_user_word((const short *) from);
			*(char *) to = get_user_byte(2+(const char *) from);
			return;
		case 4:
			*(int *) to = get_user_long((const int *) from);
			return;
	}
#define COMMON(x) \
__asm__("cld\n\t" \
	"rep ; fs ; movsl\n\t" \
	x \
	: /* no outputs */ \
	:"c" (n/4),"D" ((long) to),"S" ((long) from) \
	:"cx","di","si","memory")

	switch (n % 4) {
		case 0:
			COMMON("");
			return;
		case 1:
			COMMON("fs ; movsb");
			return;
		case 2:
			COMMON("fs ; movsw");
			return;
		case 3:
			COMMON("fs ; movsw\n\tfs ; movsb");
			return;
	}
#undef COMMON
}

#define memcpy_fromfs(to, from, n) \
(__builtin_constant_p(n) ? \
 __constant_memcpy_fromfs((to),(from),(n)) : \
 __generic_memcpy_fromfs((to),(from),(n)))

#define memcpy_tofs(to, from, n) \
(__builtin_constant_p(n) ? \
 __constant_memcpy_tofs((to),(from),(n)) : \
 __generic_memcpy_tofs((to),(from),(n)))

/*
 * Someone who knows GNU asm better than I should double check the followig.
 * It seems to work, but I don't know if I'm doing something subtly wrong.
 * --- TYT, 11/24/91
 * [ nothing wrong here, Linus: I just changed the ax to be any reg ]
 */

static inline unsigned long get_fs(void)
{
	unsigned long _v;
	__asm__("mov %%fs,%w0":"=r" (_v):"0" (0));
	return _v;
}

static inline unsigned long get_ds(void)
{
	unsigned long _v;
	__asm__("mov %%ds,%w0":"=r" (_v):"0" (0));
	return _v;
}

static inline void set_fs(unsigned long val)
{
	__asm__ __volatile__("mov %w0,%%fs": /* no output */ :"r" (val));
}

#endif /* _ASM_SEGMENT_H */
