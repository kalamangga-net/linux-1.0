/*
 * Dynamic loading of modules into the kernel.
 */

#ifndef _LINUX_MODULE_H
#define _LINUX_MODULE_H

/* values of module.state */
#define MOD_UNINITIALIZED 0
#define MOD_RUNNING 1
#define MOD_DELETED 2

/* maximum length of module name */
#define MOD_MAX_NAME 64

/* maximum length of symbol name */
#define SYM_MAX_NAME 60


struct module {
	struct module *next;
	char *name;
	int size;			/* size of module in pages */
	void* addr;			/* address of module */
	int state;
	void (*cleanup)(void);		/* cleanup routine */
};


struct mod_routines {
	int (*init)(void);		/* initialization routine */
	void (*cleanup)(void);		/* cleanup routine */
};


struct kernel_sym {
	unsigned long value;		/* value of symbol */
	char name[SYM_MAX_NAME];	/* name of symbol */
};

extern struct module *module_list;


/*
 * The first word of the module contains the use count.
 */
#define GET_USE_COUNT(module)	(* (int *) (module)->addr)
/*
 * define the count variable, and usage macros.
 */

extern int mod_use_count_;

#define MOD_INC_USE_COUNT      mod_use_count_++
#define MOD_DEC_USE_COUNT      mod_use_count_--
#define MOD_IN_USE	       (mod_use_count_ != 0)

#endif
