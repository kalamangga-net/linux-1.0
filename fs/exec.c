/*
 *  linux/fs/exec.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * #!-checking implemented by tytso.
 */

/*
 * Demand-loading implemented 01.12.91 - no need to read anything but
 * the header into memory. The inode of the executable is put into
 * "current->executable", and page faults do the actual loading. Clean.
 *
 * Once more I can proudly say that linux stood up to being changed: it
 * was less than 2 hours work to get demand-loading completely implemented.
 *
 * Demand loading changed July 1993 by Eric Youngdale.   Use mmap instead,
 * current->executable is only used by the procfs.  This allows a dispatch
 * table to check for several different types  of binary formats.  We keep
 * trying until we recognize the file or we run out of supported binary
 * formats. 
 */

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/a.out.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/ptrace.h>
#include <linux/user.h>
#include <linux/segment.h>
#include <linux/malloc.h>

#include <asm/system.h>

#include <linux/binfmts.h>

#include <asm/segment.h>
#include <asm/system.h>

asmlinkage int sys_exit(int exit_code);
asmlinkage int sys_close(unsigned fd);
asmlinkage int sys_open(const char *, int, int);
asmlinkage int sys_brk(unsigned long);

extern void shm_exit (void);

int open_inode(struct inode * inode, int mode)
{
	int error, fd;
	struct file *f, **fpp;

	if (!inode->i_op || !inode->i_op->default_file_ops)
		return -EINVAL;
	f = get_empty_filp();
	if (!f)
		return -EMFILE;
	fd = 0;
	fpp = current->filp;
	for (;;) {
		if (!*fpp)
			break;
		if (++fd > NR_OPEN)
			return -ENFILE;
		fpp++;
	}
	*fpp = f;
	f->f_flags = mode;
	f->f_mode = (mode+1) & O_ACCMODE;
	f->f_inode = inode;
	f->f_pos = 0;
	f->f_reada = 0;
	f->f_op = inode->i_op->default_file_ops;
	if (f->f_op->open) {
		error = f->f_op->open(inode,f);
		if (error) {
			*fpp = NULL;
			f->f_count--;
			return error;
		}
	}
	inode->i_count++;
	return fd;
}

/*
 * These are the only things you should do on a core-file: use only these
 * macros to write out all the necessary info.
 */
#define DUMP_WRITE(addr,nr) \
while (file.f_op->write(inode,&file,(char *)(addr),(nr)) != (nr)) goto close_coredump

#define DUMP_SEEK(offset) \
if (file.f_op->lseek) { \
	if (file.f_op->lseek(inode,&file,(offset),0) != (offset)) \
 		goto close_coredump; \
} else file.f_pos = (offset)		

/*
 * Routine writes a core dump image in the current directory.
 * Currently only a stub-function.
 *
 * Note that setuid/setgid files won't make a core-dump if the uid/gid
 * changed due to the set[u|g]id. It's enforced by the "current->dumpable"
 * field, which also makes sure the core-dumps won't be recursive if the
 * dumping of the process results in another error..
 */
int core_dump(long signr, struct pt_regs * regs)
{
	struct inode * inode = NULL;
	struct file file;
	unsigned short fs;
	int has_dumped = 0;
	char corefile[6+sizeof(current->comm)];
	int i;
	register int dump_start, dump_size;
	struct user dump;

	if (!current->dumpable)
		return 0;
	current->dumpable = 0;

/* See if we have enough room to write the upage.  */
	if (current->rlim[RLIMIT_CORE].rlim_cur < PAGE_SIZE)
		return 0;
	fs = get_fs();
	set_fs(KERNEL_DS);
	memcpy(corefile,"core.",5);
#if 0
	memcpy(corefile+5,current->comm,sizeof(current->comm));
#else
	corefile[4] = '\0';
#endif
	if (open_namei(corefile,O_CREAT | 2 | O_TRUNC,0600,&inode,NULL)) {
		inode = NULL;
		goto end_coredump;
	}
	if (!S_ISREG(inode->i_mode))
		goto end_coredump;
	if (!inode->i_op || !inode->i_op->default_file_ops)
		goto end_coredump;
	file.f_mode = 3;
	file.f_flags = 0;
	file.f_count = 1;
	file.f_inode = inode;
	file.f_pos = 0;
	file.f_reada = 0;
	file.f_op = inode->i_op->default_file_ops;
	if (file.f_op->open)
		if (file.f_op->open(inode,&file))
			goto end_coredump;
	if (!file.f_op->write)
		goto close_coredump;
	has_dumped = 1;
/* changed the size calculations - should hopefully work better. lbt */
	dump.magic = CMAGIC;
	dump.start_code = 0;
	dump.start_stack = regs->esp & ~(PAGE_SIZE - 1);
	dump.u_tsize = ((unsigned long) current->end_code) >> 12;
	dump.u_dsize = ((unsigned long) (current->brk + (PAGE_SIZE-1))) >> 12;
	dump.u_dsize -= dump.u_tsize;
	dump.u_ssize = 0;
	for(i=0; i<8; i++) dump.u_debugreg[i] = current->debugreg[i];  
	if (dump.start_stack < TASK_SIZE)
		dump.u_ssize = ((unsigned long) (TASK_SIZE - dump.start_stack)) >> 12;
/* If the size of the dump file exceeds the rlimit, then see what would happen
   if we wrote the stack, but not the data area.  */
	if ((dump.u_dsize+dump.u_ssize+1) * PAGE_SIZE >
	    current->rlim[RLIMIT_CORE].rlim_cur)
		dump.u_dsize = 0;
/* Make sure we have enough room to write the stack and data areas. */
	if ((dump.u_ssize+1) * PAGE_SIZE >
	    current->rlim[RLIMIT_CORE].rlim_cur)
		dump.u_ssize = 0;
       	strncpy(dump.u_comm, current->comm, sizeof(current->comm));
	dump.u_ar0 = (struct pt_regs *)(((int)(&dump.regs)) -((int)(&dump)));
	dump.signal = signr;
	dump.regs = *regs;
/* Flag indicating the math stuff is valid. We don't support this for the
   soft-float routines yet */
	if (hard_math) {
		if ((dump.u_fpvalid = current->used_math) != 0) {
			if (last_task_used_math == current)
				__asm__("clts ; fnsave %0": :"m" (dump.i387));
			else
				memcpy(&dump.i387,&current->tss.i387.hard,sizeof(dump.i387));
		}
	} else {
		/* we should dump the emulator state here, but we need to
		   convert it into standard 387 format first.. */
		dump.u_fpvalid = 0;
	}
	set_fs(KERNEL_DS);
/* struct user */
	DUMP_WRITE(&dump,sizeof(dump));
/* Now dump all of the user data.  Include malloced stuff as well */
	DUMP_SEEK(PAGE_SIZE);
/* now we start writing out the user space info */
	set_fs(USER_DS);
/* Dump the data area */
	if (dump.u_dsize != 0) {
		dump_start = dump.u_tsize << 12;
		dump_size = dump.u_dsize << 12;
		DUMP_WRITE(dump_start,dump_size);
	};
/* Now prepare to dump the stack area */
	if (dump.u_ssize != 0) {
		dump_start = dump.start_stack;
		dump_size = dump.u_ssize << 12;
		DUMP_WRITE(dump_start,dump_size);
	};
/* Finally dump the task struct.  Not be used by gdb, but could be useful */
	set_fs(KERNEL_DS);
	DUMP_WRITE(current,sizeof(*current));
close_coredump:
	if (file.f_op->release)
		file.f_op->release(inode,&file);
end_coredump:
	set_fs(fs);
	iput(inode);
	return has_dumped;
}

/*
 * Note that a shared library must be both readable and executable due to
 * security reasons.
 *
 * Also note that we take the address to load from from the file itself.
 */
asmlinkage int sys_uselib(const char * library)
{
	int fd, retval;
	struct file * file;
	struct linux_binfmt * fmt;

	fd = sys_open(library, 0, 0);
	if (fd < 0)
		return fd;
	file = current->filp[fd];
	retval = -ENOEXEC;
	if (file && file->f_inode && file->f_op && file->f_op->read) {
		fmt = formats;
		do {
			int (*fn)(int) = fmt->load_shlib;
			if (!fn)
				break;
			retval = fn(fd);
			fmt++;
		} while (retval == -ENOEXEC);
	}
	sys_close(fd);
  	return retval;
}

/*
 * create_tables() parses the env- and arg-strings in new user
 * memory and creates the pointer tables from them, and puts their
 * addresses on the "stack", returning the new stack pointer value.
 */
unsigned long * create_tables(char * p,int argc,int envc,int ibcs)
{
	unsigned long *argv,*envp;
	unsigned long * sp;
	struct vm_area_struct *mpnt;

	mpnt = (struct vm_area_struct *)kmalloc(sizeof(*mpnt), GFP_KERNEL);
	if (mpnt) {
		mpnt->vm_task = current;
		mpnt->vm_start = PAGE_MASK & (unsigned long) p;
		mpnt->vm_end = TASK_SIZE;
		mpnt->vm_page_prot = PAGE_PRIVATE|PAGE_DIRTY;
		mpnt->vm_share = NULL;
		mpnt->vm_inode = NULL;
		mpnt->vm_offset = 0;
		mpnt->vm_ops = NULL;
		insert_vm_struct(current, mpnt);
		current->stk_vma = mpnt;
	}
	sp = (unsigned long *) (0xfffffffc & (unsigned long) p);
	sp -= envc+1;
	envp = sp;
	sp -= argc+1;
	argv = sp;
	if (!ibcs) {
		put_fs_long((unsigned long)envp,--sp);
		put_fs_long((unsigned long)argv,--sp);
	}
	put_fs_long((unsigned long)argc,--sp);
	current->arg_start = (unsigned long) p;
	while (argc-->0) {
		put_fs_long((unsigned long) p,argv++);
		while (get_fs_byte(p++)) /* nothing */ ;
	}
	put_fs_long(0,argv);
	current->arg_end = current->env_start = (unsigned long) p;
	while (envc-->0) {
		put_fs_long((unsigned long) p,envp++);
		while (get_fs_byte(p++)) /* nothing */ ;
	}
	put_fs_long(0,envp);
	current->env_end = (unsigned long) p;
	return sp;
}

/*
 * count() counts the number of arguments/envelopes
 */
static int count(char ** argv)
{
	int i=0;
	char ** tmp;

	if ((tmp = argv) != 0)
		while (get_fs_long((unsigned long *) (tmp++)))
			i++;

	return i;
}

/*
 * 'copy_string()' copies argument/envelope strings from user
 * memory to free pages in kernel mem. These are in a format ready
 * to be put directly into the top of new user memory.
 *
 * Modified by TYT, 11/24/91 to add the from_kmem argument, which specifies
 * whether the string and the string array are from user or kernel segments:
 * 
 * from_kmem     argv *        argv **
 *    0          user space    user space
 *    1          kernel space  user space
 *    2          kernel space  kernel space
 * 
 * We do this by playing games with the fs segment register.  Since it
 * it is expensive to load a segment register, we try to avoid calling
 * set_fs() unless we absolutely have to.
 */
unsigned long copy_strings(int argc,char ** argv,unsigned long *page,
		unsigned long p, int from_kmem)
{
	char *tmp, *pag = NULL;
	int len, offset = 0;
	unsigned long old_fs, new_fs;

	if (!p)
		return 0;	/* bullet-proofing */
	new_fs = get_ds();
	old_fs = get_fs();
	if (from_kmem==2)
		set_fs(new_fs);
	while (argc-- > 0) {
		if (from_kmem == 1)
			set_fs(new_fs);
		if (!(tmp = (char *)get_fs_long(((unsigned long *)argv)+argc)))
			panic("VFS: argc is wrong");
		if (from_kmem == 1)
			set_fs(old_fs);
		len=0;		/* remember zero-padding */
		do {
			len++;
		} while (get_fs_byte(tmp++));
		if (p < len) {	/* this shouldn't happen - 128kB */
			set_fs(old_fs);
			return 0;
		}
		while (len) {
			--p; --tmp; --len;
			if (--offset < 0) {
				offset = p % PAGE_SIZE;
				if (from_kmem==2)
					set_fs(old_fs);
				if (!(pag = (char *) page[p/PAGE_SIZE]) &&
				    !(pag = (char *) page[p/PAGE_SIZE] =
				      (unsigned long *) get_free_page(GFP_USER))) 
					return 0;
				if (from_kmem==2)
					set_fs(new_fs);

			}
			*(pag + offset) = get_fs_byte(tmp);
		}
	}
	if (from_kmem==2)
		set_fs(old_fs);
	return p;
}

unsigned long change_ldt(unsigned long text_size,unsigned long * page)
{
	unsigned long code_limit,data_limit,code_base,data_base;
	int i;

	code_limit = TASK_SIZE;
	data_limit = TASK_SIZE;
	code_base = data_base = 0;
	current->start_code = code_base;
	data_base += data_limit;
	for (i=MAX_ARG_PAGES-1 ; i>=0 ; i--) {
		data_base -= PAGE_SIZE;
		if (page[i]) {
			current->rss++;
			put_dirty_page(current,page[i],data_base);
		}
	}
	return data_limit;
}

/*
 * Read in the complete executable. This is used for "-N" files
 * that aren't on a block boundary, and for files on filesystems
 * without bmap support.
 */
int read_exec(struct inode *inode, unsigned long offset,
	char * addr, unsigned long count)
{
	struct file file;
	int result = -ENOEXEC;

	if (!inode->i_op || !inode->i_op->default_file_ops)
		goto end_readexec;
	file.f_mode = 1;
	file.f_flags = 0;
	file.f_count = 1;
	file.f_inode = inode;
	file.f_pos = 0;
	file.f_reada = 0;
	file.f_op = inode->i_op->default_file_ops;
	if (file.f_op->open)
		if (file.f_op->open(inode,&file))
			goto end_readexec;
	if (!file.f_op || !file.f_op->read)
		goto close_readexec;
	if (file.f_op->lseek) {
		if (file.f_op->lseek(inode,&file,offset,0) != offset)
 			goto close_readexec;
	} else
		file.f_pos = offset;
	if (get_fs() == USER_DS) {
		result = verify_area(VERIFY_WRITE, addr, count);
		if (result)
			goto close_readexec;
	}
	result = file.f_op->read(inode, &file, addr, count);
close_readexec:
	if (file.f_op->release)
		file.f_op->release(inode,&file);
end_readexec:
	return result;
}


/*
 * This function flushes out all traces of the currently running executable so
 * that a new one can be started
 */

void flush_old_exec(struct linux_binprm * bprm)
{
	int i;
	int ch;
	char * name;
	struct vm_area_struct * mpnt, *mpnt1;

	current->dumpable = 1;
	name = bprm->filename;
	for (i=0; (ch = *(name++)) != '\0';) {
		if (ch == '/')
			i = 0;
		else
			if (i < 15)
				current->comm[i++] = ch;
	}
	current->comm[i] = '\0';
	if (current->shm)
		shm_exit();
	if (current->executable) {
		iput(current->executable);
		current->executable = NULL;
	}
	/* Release all of the old mmap stuff. */

	mpnt = current->mmap;
	current->mmap = NULL;
	current->stk_vma = NULL;
	while (mpnt) {
		mpnt1 = mpnt->vm_next;
		if (mpnt->vm_ops && mpnt->vm_ops->close)
			mpnt->vm_ops->close(mpnt);
		kfree(mpnt);
		mpnt = mpnt1;
	}

	/* Flush the old ldt stuff... */
	if (current->ldt) {
		free_page((unsigned long) current->ldt);
		current->ldt = NULL;
		for (i=1 ; i<NR_TASKS ; i++) {
			if (task[i] == current)  {
				set_ldt_desc(gdt+(i<<1)+
					     FIRST_LDT_ENTRY,&default_ldt, 1);
				load_ldt(i);
			}
		}	
	}

	for (i=0 ; i<8 ; i++) current->debugreg[i] = 0;

	if (bprm->e_uid != current->euid || bprm->e_gid != current->egid || 
	    !permission(bprm->inode,MAY_READ))
		current->dumpable = 0;
	current->signal = 0;
	for (i=0 ; i<32 ; i++) {
		current->sigaction[i].sa_mask = 0;
		current->sigaction[i].sa_flags = 0;
		if (current->sigaction[i].sa_handler != SIG_IGN)
			current->sigaction[i].sa_handler = NULL;
	}
	for (i=0 ; i<NR_OPEN ; i++)
		if (FD_ISSET(i,&current->close_on_exec))
			sys_close(i);
	FD_ZERO(&current->close_on_exec);
	clear_page_tables(current);
	if (last_task_used_math == current)
		last_task_used_math = NULL;
	current->used_math = 0;
	current->elf_executable = 0;
}

/*
 * sys_execve() executes a new program.
 */
static int do_execve(char * filename, char ** argv, char ** envp, struct pt_regs * regs)
{
	struct linux_binprm bprm;
	struct linux_binfmt * fmt;
	unsigned long old_fs;
	int i;
	int retval;
	int sh_bang = 0;

	if (regs->cs != USER_CS)
		return -EINVAL;
	bprm.p = PAGE_SIZE*MAX_ARG_PAGES-4;
	for (i=0 ; i<MAX_ARG_PAGES ; i++)	/* clear page-table */
		bprm.page[i] = 0;
	retval = open_namei(filename, 0, 0, &bprm.inode, NULL);
	if (retval)
		return retval;
	bprm.filename = filename;
	bprm.argc = count(argv);
	bprm.envc = count(envp);
	
restart_interp:
	if (!S_ISREG(bprm.inode->i_mode)) {	/* must be regular file */
		retval = -EACCES;
		goto exec_error2;
	}
	if (IS_NOEXEC(bprm.inode)) {		/* FS mustn't be mounted noexec */
		retval = -EPERM;
		goto exec_error2;
	}
	if (!bprm.inode->i_sb) {
		retval = -EACCES;
		goto exec_error2;
	}
	i = bprm.inode->i_mode;
	if (IS_NOSUID(bprm.inode) && (((i & S_ISUID) && bprm.inode->i_uid != current->
	    euid) || ((i & S_ISGID) && !in_group_p(bprm.inode->i_gid))) &&
	    !suser()) {
		retval = -EPERM;
		goto exec_error2;
	}
	/* make sure we don't let suid, sgid files be ptraced. */
	if (current->flags & PF_PTRACED) {
		bprm.e_uid = current->euid;
		bprm.e_gid = current->egid;
	} else {
		bprm.e_uid = (i & S_ISUID) ? bprm.inode->i_uid : current->euid;
		bprm.e_gid = (i & S_ISGID) ? bprm.inode->i_gid : current->egid;
	}
	if (current->euid == bprm.inode->i_uid)
		i >>= 6;
	else if (in_group_p(bprm.inode->i_gid))
		i >>= 3;
	if (!(i & 1) &&
	    !((bprm.inode->i_mode & 0111) && suser())) {
		retval = -EACCES;
		goto exec_error2;
	}
	memset(bprm.buf,0,sizeof(bprm.buf));
	old_fs = get_fs();
	set_fs(get_ds());
	retval = read_exec(bprm.inode,0,bprm.buf,128);
	set_fs(old_fs);
	if (retval < 0)
		goto exec_error2;
	if ((bprm.buf[0] == '#') && (bprm.buf[1] == '!') && (!sh_bang)) {
		/*
		 * This section does the #! interpretation.
		 * Sorta complicated, but hopefully it will work.  -TYT
		 */

		char *cp, *interp, *i_name, *i_arg;

		iput(bprm.inode);
		bprm.buf[127] = '\0';
		if ((cp = strchr(bprm.buf, '\n')) == NULL)
			cp = bprm.buf+127;
		*cp = '\0';
		while (cp > bprm.buf) {
			cp--;
			if ((*cp == ' ') || (*cp == '\t'))
				*cp = '\0';
			else
				break;
		}
		for (cp = bprm.buf+2; (*cp == ' ') || (*cp == '\t'); cp++);
		if (!cp || *cp == '\0') {
			retval = -ENOEXEC; /* No interpreter name found */
			goto exec_error1;
		}
		interp = i_name = cp;
		i_arg = 0;
		for ( ; *cp && (*cp != ' ') && (*cp != '\t'); cp++) {
 			if (*cp == '/')
				i_name = cp+1;
		}
		while ((*cp == ' ') || (*cp == '\t'))
			*cp++ = '\0';
		if (*cp)
			i_arg = cp;
		/*
		 * OK, we've parsed out the interpreter name and
		 * (optional) argument.
		 */
		if (sh_bang++ == 0) {
			bprm.p = copy_strings(bprm.envc, envp, bprm.page, bprm.p, 0);
			bprm.p = copy_strings(--bprm.argc, argv+1, bprm.page, bprm.p, 0);
		}
		/*
		 * Splice in (1) the interpreter's name for argv[0]
		 *           (2) (optional) argument to interpreter
		 *           (3) filename of shell script
		 *
		 * This is done in reverse order, because of how the
		 * user environment and arguments are stored.
		 */
		bprm.p = copy_strings(1, &bprm.filename, bprm.page, bprm.p, 2);
		bprm.argc++;
		if (i_arg) {
			bprm.p = copy_strings(1, &i_arg, bprm.page, bprm.p, 2);
			bprm.argc++;
		}
		bprm.p = copy_strings(1, &i_name, bprm.page, bprm.p, 2);
		bprm.argc++;
		if (!bprm.p) {
			retval = -E2BIG;
			goto exec_error1;
		}
		/*
		 * OK, now restart the process with the interpreter's inode.
		 * Note that we use open_namei() as the name is now in kernel
		 * space, and we don't need to copy it.
		 */
		retval = open_namei(interp, 0, 0, &bprm.inode, NULL);
		if (retval)
			goto exec_error1;
		goto restart_interp;
	}
	if (!sh_bang) {
		bprm.p = copy_strings(bprm.envc,envp,bprm.page,bprm.p,0);
		bprm.p = copy_strings(bprm.argc,argv,bprm.page,bprm.p,0);
		if (!bprm.p) {
			retval = -E2BIG;
			goto exec_error2;
		}
	}

	bprm.sh_bang = sh_bang;
	fmt = formats;
	do {
		int (*fn)(struct linux_binprm *, struct pt_regs *) = fmt->load_binary;
		if (!fn)
			break;
		retval = fn(&bprm, regs);
		if (retval == 0) {
			iput(bprm.inode);
			current->did_exec = 1;
			return 0;
		}
		fmt++;
	} while (retval == -ENOEXEC);
exec_error2:
	iput(bprm.inode);
exec_error1:
	for (i=0 ; i<MAX_ARG_PAGES ; i++)
		free_page(bprm.page[i]);
	return(retval);
}

/*
 * sys_execve() executes a new program.
 */
asmlinkage int sys_execve(struct pt_regs regs)
{
	int error;
	char * filename;

	error = getname((char *) regs.ebx, &filename);
	if (error)
		return error;
	error = do_execve(filename, (char **) regs.ecx, (char **) regs.edx, &regs);
	putname(filename);
	return error;
}

/*
 * These are  the prototypes for the  functions in the  dispatch table, as
 * well as the  dispatch  table itself.
 */

extern int load_aout_binary(struct linux_binprm *,
			    struct pt_regs * regs);
extern int load_aout_library(int fd);

#ifdef CONFIG_BINFMT_ELF
extern int load_elf_binary(struct linux_binprm *,
			    struct pt_regs * regs);
extern int load_elf_library(int fd);
#endif

#ifdef CONFIG_BINFMT_COFF
extern int load_coff_binary(struct linux_binprm *,
			    struct pt_regs * regs);
extern int load_coff_library(int fd);
#endif

/* Here are the actual binaries that will be accepted  */
struct linux_binfmt formats[] = {
	{load_aout_binary, load_aout_library},
#ifdef CONFIG_BINFMT_ELF
	{load_elf_binary, load_elf_library},
#endif
#ifdef CONFIG_BINFMT_COFF
	{load_coff_binary, load_coff_library},
#endif
	{NULL, NULL}
};

/*
 * These are the functions used to load a.out style executables and shared
 * libraries.  There is no binary dependent code anywhere else.
 */

int load_aout_binary(struct linux_binprm * bprm, struct pt_regs * regs)
{
	struct exec ex;
	struct file * file;
	int fd, error;
	unsigned long p = bprm->p;

	ex = *((struct exec *) bprm->buf);		/* exec-header */
	if ((N_MAGIC(ex) != ZMAGIC && N_MAGIC(ex) != OMAGIC && 
	     N_MAGIC(ex) != QMAGIC) ||
	    ex.a_trsize || ex.a_drsize ||
	    bprm->inode->i_size < ex.a_text+ex.a_data+ex.a_syms+N_TXTOFF(ex)) {
		return -ENOEXEC;
	}

	if (N_MAGIC(ex) == ZMAGIC &&
	    (N_TXTOFF(ex) < bprm->inode->i_sb->s_blocksize)) {
		printk("N_TXTOFF < BLOCK_SIZE. Please convert binary.");
		return -ENOEXEC;
	}

	if (N_TXTOFF(ex) != BLOCK_SIZE && N_MAGIC(ex) == ZMAGIC) {
		printk("N_TXTOFF != BLOCK_SIZE. See a.out.h.");
		return -ENOEXEC;
	}
	
	/* OK, This is the point of no return */
	flush_old_exec(bprm);

	current->end_code = N_TXTADDR(ex) + ex.a_text;
	current->end_data = ex.a_data + current->end_code;
	current->start_brk = current->brk = current->end_data;
	current->start_code += N_TXTADDR(ex);
	current->rss = 0;
	current->suid = current->euid = bprm->e_uid;
	current->mmap = NULL;
	current->executable = NULL;  /* for OMAGIC files */
	current->sgid = current->egid = bprm->e_gid;
	if (N_MAGIC(ex) == OMAGIC) {
		do_mmap(NULL, 0, ex.a_text+ex.a_data,
			PROT_READ|PROT_WRITE|PROT_EXEC,
			MAP_FIXED|MAP_PRIVATE, 0);
		read_exec(bprm->inode, 32, (char *) 0, ex.a_text+ex.a_data);
	} else {
		if (ex.a_text & 0xfff || ex.a_data & 0xfff)
			printk("%s: executable not page aligned\n", current->comm);
		
		fd = open_inode(bprm->inode, O_RDONLY);
		
		if (fd < 0)
			return fd;
		file = current->filp[fd];
		if (!file->f_op || !file->f_op->mmap) {
			sys_close(fd);
			do_mmap(NULL, 0, ex.a_text+ex.a_data,
				PROT_READ|PROT_WRITE|PROT_EXEC,
				MAP_FIXED|MAP_PRIVATE, 0);
			read_exec(bprm->inode, N_TXTOFF(ex),
				  (char *) N_TXTADDR(ex), ex.a_text+ex.a_data);
			goto beyond_if;
		}
		error = do_mmap(file, N_TXTADDR(ex), ex.a_text,
				PROT_READ | PROT_EXEC,
				MAP_FIXED | MAP_SHARED, N_TXTOFF(ex));

		if (error != N_TXTADDR(ex)) {
			sys_close(fd);
			send_sig(SIGSEGV, current, 0);
			return 0;
		};
		
 		error = do_mmap(file, N_TXTADDR(ex) + ex.a_text, ex.a_data,
				PROT_READ | PROT_WRITE | PROT_EXEC,
				MAP_FIXED | MAP_PRIVATE, N_TXTOFF(ex) + ex.a_text);
		sys_close(fd);
		if (error != N_TXTADDR(ex) + ex.a_text) {
			send_sig(SIGSEGV, current, 0);
			return 0;
		};
		current->executable = bprm->inode;
		bprm->inode->i_count++;
	}
beyond_if:
	sys_brk(current->brk+ex.a_bss);
	
	p += change_ldt(ex.a_text,bprm->page);
	p -= MAX_ARG_PAGES*PAGE_SIZE;
	p = (unsigned long) create_tables((char *)p,bprm->argc,bprm->envc,0);
	current->start_stack = p;
	regs->eip = ex.a_entry;		/* eip, magic happens :-) */
	regs->esp = p;			/* stack pointer */
	if (current->flags & PF_PTRACED)
		send_sig(SIGTRAP, current, 0);
	return 0;
}


int load_aout_library(int fd)
{
        struct file * file;
	struct exec ex;
	struct  inode * inode;
	unsigned int len;
	unsigned int bss;
	unsigned int start_addr;
	int error;
	
	file = current->filp[fd];
	inode = file->f_inode;
	
	set_fs(KERNEL_DS);
	if (file->f_op->read(inode, file, (char *) &ex, sizeof(ex)) != sizeof(ex)) {
		return -EACCES;
	}
	set_fs(USER_DS);
	
	/* We come in here for the regular a.out style of shared libraries */
	if ((N_MAGIC(ex) != ZMAGIC && N_MAGIC(ex) != QMAGIC) || ex.a_trsize ||
	    ex.a_drsize || ((ex.a_entry & 0xfff) && N_MAGIC(ex) == ZMAGIC) ||
	    inode->i_size < ex.a_text+ex.a_data+ex.a_syms+N_TXTOFF(ex)) {
		return -ENOEXEC;
	}
	if (N_MAGIC(ex) == ZMAGIC && N_TXTOFF(ex) && 
	    (N_TXTOFF(ex) < inode->i_sb->s_blocksize)) {
		printk("N_TXTOFF < BLOCK_SIZE. Please convert library\n");
		return -ENOEXEC;
	}
	
	if (N_FLAGS(ex)) return -ENOEXEC;

	/* For  QMAGIC, the starting address is 0x20 into the page.  We mask
	   this off to get the starting address for the page */

	start_addr =  ex.a_entry & 0xfffff000;

	/* Now use mmap to map the library into memory. */
	error = do_mmap(file, start_addr, ex.a_text + ex.a_data,
			PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_PRIVATE,
			N_TXTOFF(ex));
	if (error != start_addr)
		return error;
	len = PAGE_ALIGN(ex.a_text + ex.a_data);
	bss = ex.a_text + ex.a_data + ex.a_bss;
	if (bss > len)
		do_mmap(NULL, start_addr + len, bss-len,
			PROT_READ|PROT_WRITE|PROT_EXEC,
			MAP_PRIVATE|MAP_FIXED, 0);
	return 0;
}
