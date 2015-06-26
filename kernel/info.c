/*
 * linux/kernel/info.c
 *
 * Copyright (C) 1992 Darren Senn
 */

/* This implements the sysinfo() system call */

#include <asm/segment.h>

#include <linux/sched.h>
#include <linux/string.h>
#include <linux/unistd.h>
#include <linux/types.h>
#include <linux/mm.h>

asmlinkage int sys_sysinfo(struct sysinfo *info)
{
	int error;
	struct sysinfo val;
	struct task_struct **p;

	error = verify_area(VERIFY_WRITE, info, sizeof(struct sysinfo));
	if (error)
		return error;
	memset((char *)&val, 0, sizeof(struct sysinfo));

	val.uptime = jiffies / HZ;

	val.loads[0] = avenrun[0] << (SI_LOAD_SHIFT - FSHIFT);
	val.loads[1] = avenrun[1] << (SI_LOAD_SHIFT - FSHIFT);
	val.loads[2] = avenrun[2] << (SI_LOAD_SHIFT - FSHIFT);

	for (p = &LAST_TASK; p > &FIRST_TASK; p--)
		if (*p) val.procs++;

	si_meminfo(&val);
	si_swapinfo(&val);

	memcpy_tofs(info, &val, sizeof(struct sysinfo));
	return 0;
}
