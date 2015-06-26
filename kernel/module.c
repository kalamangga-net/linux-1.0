#include <linux/errno.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <linux/mm.h>		/* defines GFP_KERNEL */
#include <linux/string.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/malloc.h>

struct module *module_list = NULL;
int freeing_modules;		/* true if some modules are marked for deletion */

struct module *find_module( const char *name);
int get_mod_name( char *user_name, char *buf);
int free_modules( void);

/*
 * Allocate space for a module.
 */
asmlinkage int
sys_create_module(char *module_name, unsigned long size)
{
	int npages;
	void* addr;
	int len;
	char name[MOD_MAX_NAME];
	char *savename;
	struct module *mp;
	int error;

	if (!suser())
		return -EPERM;
	if (module_name == NULL || size == 0)
		return -EINVAL;
	if ((error = get_mod_name(module_name, name)) != 0)
		return error;
	if (find_module(name) != NULL) {
		return -EEXIST;
	}
	len = strlen(name) + 1;
	if ((savename = (char*) kmalloc(len, GFP_KERNEL)) == NULL)
		return -ENOMEM;
	memcpy(savename, name, len);
	if ((mp = (struct module*) kmalloc(sizeof *mp, GFP_KERNEL)) == NULL) {
		kfree(savename);
		return -ENOMEM;
	}
	npages = (size + sizeof (int) + 4095) / 4096;
	if ((addr = vmalloc(npages * 4096)) == 0) {
		kfree_s(mp, sizeof *mp);
		kfree(savename);
		return -ENOMEM;
	}
	mp->name = savename;
	mp->size = npages;
	mp->addr = addr;
	mp->state = MOD_UNINITIALIZED;
	* (int *) addr = 0;		/* set use count to zero */
	mp->cleanup = NULL;
	mp->next = module_list;
	module_list = mp;
	printk("module `%s' (%lu pages @ 0x%08lx) created\n",
		mp->name, (unsigned long) mp->size, (unsigned long) mp->addr);
	return (int) addr;
}

/*
 * Initialize a module.
 */
asmlinkage int
sys_init_module(char *module_name, char *code, unsigned codesize,
		struct mod_routines *routines)
{
	struct module *mp;
	char name[MOD_MAX_NAME];
	int error;
	struct mod_routines rt;

	if (!suser())
		return -EPERM;
	/*
	 * First reclaim any memory from dead modules that where not
	 * freed when deleted. Should I think be done by timers when
	 * the module was deleted - Jon.
	 */
	free_modules();

	if ((error = get_mod_name(module_name, name)) != 0)
		return error;
	printk( "initializing module `%s', %d (0x%x) bytes\n",
		name, codesize, codesize);
	memcpy_fromfs(&rt, routines, sizeof rt);
	if ((mp = find_module(name)) == NULL)
		return -ENOENT;
	if ((codesize + sizeof (int) + 4095) / 4096 > mp->size)
		return -EINVAL;
	memcpy_fromfs((char *)mp->addr + sizeof (int), code, codesize);
	memset((char *)mp->addr + sizeof (int) + codesize, 0,
		mp->size * 4096 - (codesize + sizeof (int)));
	printk( "  init entry @ 0x%08lx, cleanup entry @ 0x%08lx\n",
		(unsigned long) rt.init, (unsigned long) rt.cleanup);
	mp->cleanup = rt.cleanup;
	if ((*rt.init)() != 0)
		return -EBUSY;
	mp->state = MOD_RUNNING;
	return 0;
}

asmlinkage int
sys_delete_module(char *module_name)
{
	struct module *mp;
	char name[MOD_MAX_NAME];
	int error;

	if (!suser())
		return -EPERM;
	if (module_name != NULL) {
		if ((error = get_mod_name(module_name, name)) != 0)
			return error;
		if ((mp = find_module(name)) == NULL)
			return -ENOENT;
		if (mp->state == MOD_RUNNING)
			(*mp->cleanup)();
		mp->state = MOD_DELETED;
	}
	free_modules();
	return 0;
}

/*
 * Copy the kernel symbol table to user space.  If the argument is null,
 * just return the size of the table.
 */
asmlinkage int
sys_get_kernel_syms(struct kernel_sym *table)
{
	struct symbol {
		unsigned long addr;
		char *name;
	};
	extern int symbol_table_size;
	extern struct symbol symbol_table[];
	int i;
	struct symbol *from;
	struct kernel_sym *to;
	struct kernel_sym sym;

	if (table != NULL) {
		from = symbol_table;
		to = table;
		i = verify_area(VERIFY_WRITE, to, symbol_table_size * sizeof *table);
		if (i)
			return i;
		for (i = symbol_table_size ; --i >= 0 ; ) {
			sym.value = from->addr;
			strncpy(sym.name, from->name, sizeof sym.name);
			memcpy_tofs(to, &sym, sizeof sym);
			from++, to++;
		}
	}
	return symbol_table_size;
}


/*
 * Copy the name of a module from user space.
 */
int
get_mod_name(char *user_name, char *buf)
{
	int i;

	i = 0;
	for (i = 0 ; (buf[i] = get_fs_byte(user_name + i)) != '\0' ; ) {
		if (++i >= MOD_MAX_NAME)
			return -E2BIG;
	}
	return 0;
}


/*
 * Look for a module by name, ignoring modules marked for deletion.
 */
struct module *
find_module( const char *name)
{
	struct module *mp;

	for (mp = module_list ; mp ; mp = mp->next) {
		if (mp->state == MOD_DELETED)
			continue;
		if (!strcmp(mp->name, name))
			break;
	}
	return mp;
}


/*
 * Try to free modules which have been marked for deletion.  Returns nonzero
 * if a module was actually freed.
 */
int
free_modules( void)
{
	struct module *mp;
	struct module **mpp;
	int did_deletion;

	did_deletion = 0;
	freeing_modules = 0;
	mpp = &module_list;
	while ((mp = *mpp) != NULL) {
		if (mp->state != MOD_DELETED) {
			mpp = &mp->next;
		} else if (GET_USE_COUNT(mp) != 0) {
			freeing_modules = 1;
			mpp = &mp->next;
		} else {	/* delete it */
			*mpp = mp->next;
			vfree(mp->addr);
			kfree(mp->name);
			kfree_s(mp, sizeof *mp);
			did_deletion = 1;
		}
	}
	return did_deletion;
}


/*
 * Called by the /proc file system to return a current list of modules.
 */
int get_module_list(char *buf)
{
	char *p;
	char *q;
	int i;
	struct module *mp;
	char size[32];

	p = buf;
	for (mp = module_list ; mp ; mp = mp->next) {
		if (p - buf > 4096 - 100)
			break;			/* avoid overflowing buffer */
		q = mp->name;
		i = 20;
		while (*q) {
			*p++ = *q++;
			i--;
		}
		sprintf(size, "%d", mp->size);
		i -= strlen(size);
		if (i <= 0)
			i = 1;
		while (--i >= 0)
			*p++ = ' ';
		q = size;
		while (*q)
			*p++ = *q++;
		if (mp->state == MOD_UNINITIALIZED)
			q = "  (uninitialized)";
		else if (mp->state == MOD_RUNNING)
			q = "";
		else if (mp->state == MOD_DELETED)
			q = "  (deleted)";
		else
			q = "  (bad state)";
		while (*q)
			*p++ = *q++;
		*p++ = '\n';
	}
	return p - buf;
}
