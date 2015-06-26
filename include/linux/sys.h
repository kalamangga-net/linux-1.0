#ifndef _LINUX_SYS_H
#define _LINUX_SYS_H
/*
 * system call entry points
 */

#define sys_clone sys_fork

#ifdef __cplusplus
extern "C" {
#endif

extern int sys_setup();         /* 0 */
extern int sys_exit();
extern int sys_fork();
extern int sys_read();
extern int sys_write();
extern int sys_open();          /* 5 */
extern int sys_close();
extern int sys_waitpid();
extern int sys_creat();
extern int sys_link();
extern int sys_unlink();        /* 10 */
extern int sys_execve();
extern int sys_chdir();
extern int sys_time();
extern int sys_mknod();
extern int sys_chmod();         /* 15 */
extern int sys_chown();
extern int sys_break();
extern int sys_stat();
extern int sys_lseek();
extern int sys_getpid();        /* 20 */
extern int sys_mount();
extern int sys_umount();
extern int sys_setuid();
extern int sys_getuid();
extern int sys_stime();         /* 25 */
extern int sys_ptrace();
extern int sys_alarm();
extern int sys_fstat();
extern int sys_pause();
extern int sys_utime();         /* 30 */
extern int sys_stty();
extern int sys_gtty();
extern int sys_access();
extern int sys_nice();
extern int sys_ftime();         /* 35 */
extern int sys_sync();
extern int sys_kill();
extern int sys_rename();
extern int sys_mkdir();
extern int sys_rmdir();         /* 40 */
extern int sys_dup();
extern int sys_pipe();
extern int sys_times();
extern int sys_prof();
extern int sys_brk();           /* 45 */
extern int sys_setgid();
extern int sys_getgid();
extern int sys_signal();
extern int sys_geteuid();
extern int sys_getegid();       /* 50 */
extern int sys_acct();
extern int sys_phys();
extern int sys_lock();
extern int sys_ioctl();
extern int sys_fcntl();         /* 55 */
extern int sys_mpx();
extern int sys_setpgid();
extern int sys_ulimit();
extern int sys_uname();
extern int sys_umask();         /* 60 */
extern int sys_chroot();
extern int sys_ustat();
extern int sys_dup2();
extern int sys_getppid();
extern int sys_getpgrp();       /* 65 */
extern int sys_setsid();
extern int sys_sigaction();
extern int sys_sgetmask();
extern int sys_ssetmask();
extern int sys_setreuid();      /* 70 */
extern int sys_setregid();
extern int sys_sigpending();
extern int sys_sigsuspend();
extern int sys_sethostname();
extern int sys_setrlimit();     /* 75 */
extern int sys_getrlimit();
extern int sys_getrusage();
extern int sys_gettimeofday();
extern int sys_settimeofday();
extern int sys_getgroups();     /* 80 */
extern int sys_setgroups();
extern int sys_select();
extern int sys_symlink();
extern int sys_lstat();
extern int sys_readlink();      /* 85 */
extern int sys_uselib();
extern int sys_swapon();
extern int sys_reboot();
extern int sys_readdir();
extern int sys_mmap();          /* 90 */
extern int sys_munmap();
extern int sys_truncate();
extern int sys_ftruncate();
extern int sys_fchmod();
extern int sys_fchown();        /* 95 */
extern int sys_getpriority();
extern int sys_setpriority();
extern int sys_profil();
extern int sys_statfs();
extern int sys_fstatfs();       /* 100 */
extern int sys_ioperm();
extern int sys_socketcall();
extern int sys_syslog();
extern int sys_getitimer();
extern int sys_setitimer();     /* 105 */
extern int sys_newstat();
extern int sys_newlstat();
extern int sys_newfstat();
extern int sys_newuname();
extern int sys_iopl();          /* 110 */
extern int sys_vhangup();
extern int sys_idle();
extern int sys_vm86();
extern int sys_wait4();
extern int sys_swapoff();       /* 115 */
extern int sys_sysinfo();
extern int sys_ipc();
extern int sys_fsync();
extern int sys_sigreturn();
extern int sys_setdomainname(); /* 120 */
extern int sys_olduname();
extern int sys_old_syscall();
extern int sys_modify_ldt();
extern int sys_adjtimex();
extern int sys_mprotect();      /* 125 */
extern int sys_sigprocmask();
extern int sys_create_module();
extern int sys_init_module();
extern int sys_delete_module();
extern int sys_get_kernel_syms(); /* 130 */
extern int sys_quotactl();
extern int sys_getpgid();
extern int sys_fchdir();
extern int sys_bdflush();

/*
 * These are system calls that will be removed at some time
 * due to newer versions existing..
 */
#ifdef notdef
#define sys_waitpid	sys_old_syscall	/* sys_wait4	*/
#define sys_olduname	sys_old_syscall /* sys_newuname	*/
#define sys_uname	sys_old_syscall /* sys_newuname	*/
#define sys_stat	sys_old_syscall /* sys_newstat	*/
#define sys_fstat	sys_old_syscall	/* sys_newfstat	*/
#define sys_lstat	sys_old_syscall /* sys_newlstat	*/
#define sys_signal	sys_old_syscall	/* sys_sigaction */
#define sys_sgetmask	sys_old_syscall /* sys_sigprocmask */
#define sys_ssetmask	sys_old_syscall /* sig_sigprocmask */
#endif

/*
 * These are system calls that haven't been implemented yet
 * but have an entry in the table for future expansion..
 */

#define sys_quotactl	sys_ni_syscall
#define sys_bdflush	sys_ni_syscall

typedef int (*fn_ptr)();

#ifdef __cplusplus
}
#endif

#endif
