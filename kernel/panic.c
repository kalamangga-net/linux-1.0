/*
 *  linux/kernel/panic.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * This function is used through-out the kernel (includeinh mm and fs)
 * to indicate a major problem.
 */
#include <stdarg.h>

#include <linux/kernel.h>
#include <linux/sched.h>

asmlinkage void sys_sync(void);	/* it's really int */

extern int vsprintf(char * buf, const char * fmt, va_list args);

NORET_TYPE void panic(const char * fmt, ...)
{
	static char buf[1024];
	va_list args;

	va_start(args, fmt);
	vsprintf(buf, fmt, args);
	va_end(args);
	printk(KERN_EMERG "Kernel panic: %s\n",buf);
	if (current == task[0])
		printk(KERN_EMERG "In swapper task - not syncing\n");
	else
		sys_sync();
	for(;;);
}
