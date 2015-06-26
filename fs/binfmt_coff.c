/*
 * These are the functions used to load COFF IBSC style executables.
 * Information on COFF format may be obtained in either the Intel Binary
 * Compatibility Specification 2 or O'Rilley's book on COFF. The shared
 * libraries are defined only the in the Intel book.
 *
 * This file is based upon code written by Eric Youndale for the ELF object
 * file format.
 *
 * Author: Al Longyear (longyear@sii.com)
 *
 * Latest Revision:
 *    3 Feburary 1994
 *      Al Longyear (longyear@sii.com)
 *      Cleared first page of bss section using put_fs_byte.
 */

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/a.out.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/binfmts.h>
#include <asm/segment.h>
#include <linux/string.h>
#include <linux/fcntl.h>
#include <linux/ptrace.h>
#include <linux/coff.h>
#include <linux/malloc.h>

asmlinkage int sys_exit (int exit_code);
asmlinkage int sys_close (unsigned fd);
asmlinkage int sys_open (const char *, int, int);
asmlinkage int sys_uselib(const char * library);

static int preload_library (struct linux_binprm *exe_bprm,
			    COFF_SCNHDR * sect,
			    struct file *fp);

static int load_object (struct linux_binprm *bprm,
			struct pt_regs *regs,
			int lib_ok);

/*
 *  Small procedure to test for the proper file alignment.
 */

static inline int
is_properly_aligned (COFF_SCNHDR *sect)
{
    long scnptr = COFF_LONG (sect->s_scnptr);
    long vaddr  = COFF_LONG (sect->s_vaddr);
/*
 *  Print the section information if needed
 */

#ifdef COFF_DEBUG
    printk ("%s, scnptr = %d, vaddr = %d\n",
	    sect->s_name,
	    scnptr, vaddr);
#endif

/*
 *  Return the error code if the section is not properly aligned.
 */

#ifdef COFF_DEBUG
    if (((vaddr - scnptr) & ~PAGE_MASK) != 0)
	printk ("bad alignment in %s\n", sect->s_name);
#endif
    return ((((vaddr - scnptr) & ~PAGE_MASK) != 0) ? -ENOEXEC : 0);
}

/*
 *    Clear the bytes in the last page of data.
 */

static
int clear_memory (unsigned long addr, unsigned long size)
{
    int status;

    size = (PAGE_SIZE - (addr & ~PAGE_MASK)) & ~PAGE_MASK;
    if (size == 0)
        status = 0;
    else {
      
#ifdef COFF_DEBUG
        printk ("un-initialized storage in last page %d\n", size);
#endif

	status = verify_area (VERIFY_WRITE,
			      (void *) addr, size);
#ifdef COFF_DEBUG
	printk ("result from verify_area = %d\n", status);
#endif

	if (status >= 0)
	    while (size-- != 0)
	        put_fs_byte (0, addr++);
    }
    return status;
}

/*
 *  Helper function to process the load operation.
 */

static int
load_object (struct linux_binprm * bprm, struct pt_regs *regs, int lib_ok)
{
    COFF_FILHDR  *coff_hdr = (COFF_FILHDR *) bprm->buf;	/* COFF Header */
    COFF_SCNHDR  *sect_bufr;	/* Pointer to section table            */
    COFF_SCNHDR  *text_sect;	/* Pointer to the text section         */
    COFF_SCNHDR  *data_sect;	/* Pointer to the data section         */
    COFF_SCNHDR  *bss_sect;	/* Pointer to the bss section          */
    int text_count;		/* Number of text sections             */
    int data_count;		/* Number of data sections             */
    int bss_count;		/* Number of bss sections              */
    int lib_count;		/* Number of lib sections              */
    unsigned int start_addr = 0;/* Starting location for program       */
    int status = 0;		/* Result status register              */
    int fd = -1;		/* Open file descriptor                */
    struct file *fp     = NULL;	/* Pointer to the file at "fd"         */
    short int sections  = 0;	/* Number of sections in the file      */
    short int aout_size = 0;	/* Size of the a.out header area       */
    short int flags;		/* Flag bits from the COFF header      */

#ifdef COFF_DEBUG
    printk ("binfmt_coff entry: %s\n", bprm->filename);
#endif

/*
 *  Validate the magic value for the object file.
 */
    do {
	if (COFF_I386BADMAG (*coff_hdr)) {
#ifdef COFF_DEBUG
	    printk ("bad filehdr magic\n");
#endif
	    status = -ENOEXEC;
	    break;
	}
/*
 *  The object file should have 32 BIT little endian format. Do not allow
 *  it to have the 16 bit object file flag set as Linux is not able to run
 *  on the 80286/80186/8086.
 */
	flags = COFF_SHORT (coff_hdr->f_flags);
	if ((flags & (COFF_F_AR32WR | COFF_F_AR16WR)) != COFF_F_AR32WR) {
#ifdef COFF_DEBUG
	    printk ("invalid f_flags bits\n");
#endif
	    status = -ENOEXEC;
	    break;
	}
/*
 *  Extract the header information which we need.
 */
	sections  = COFF_SHORT (coff_hdr->f_nscns);   /* Number of sections */
	aout_size = COFF_SHORT (coff_hdr->f_opthdr);  /* Size of opt. headr */
/*
 *  If the file is not executable then reject the exectution. This means
 *  that there must not be external references.
 */
	if ((flags & COFF_F_EXEC) == 0) {
#ifdef COFF_DEBUG
	    printk ("not executable bit\n");
#endif
	    status = -ENOEXEC;
	    break;
	}
/*
 *  There must be atleast one section.
 */
	if (sections == 0) {
#ifdef COFF_DEBUG
	    printk ("no sections\n");
#endif
	    status = -ENOEXEC;
	    break;
	}
/*
 *  Do some additional consistency checks.
 *  The system requires mapping for this loader. If you try
 *  to use a file system with no mapping, the format is not valid.
 */
	if (!bprm->inode->i_op ||
	    !bprm->inode->i_op->default_file_ops->mmap) {
#ifdef COFF_DEBUG
	    printk ("no mmap in fs\n");
#endif
	    status = -ENOEXEC;
	}
    }
    while (0);
/*
 *  Allocate a buffer to hold the entire coff section list.
 */
    if (status >= 0) {
	int nbytes = sections * COFF_SCNHSZ;

	sect_bufr = (COFF_SCNHDR *) kmalloc (nbytes, GFP_KERNEL);
	if (0 == sect_bufr) {
#ifdef COFF_DEBUG
	    printk ("kmalloc failed\n");
#endif
	    status = -ENOEXEC;
	}
/*
 *  Read the section list from the disk file.
 */
	else {
	     int old_fs = get_fs ();
	     set_fs (get_ds ());  /* Make it point to the proper location    */
	     status = read_exec (bprm->inode,	     /* INODE for file       */
			    aout_size + COFF_FILHSZ, /* Offset in the file   */
			    (char *) sect_bufr,      /* Buffer for read      */
			    nbytes);                 /* Byte count reqd.     */
	     set_fs (old_fs);	                     /* Restore the selector */
#ifdef COFF_DEBUG
	     if (status < 0)
	        printk ("read aout hdr, status = %d\n", status);
#endif
	 }
    }
    else
	sect_bufr = NULL;	/* Errors do not have a section buffer */
/*
 *  Count the number of sections for the required types and store the location
 *  of the last section for the three primary types.
 */
    text_count = 0;
    data_count = 0;
    bss_count  = 0;
    lib_count  = 0;

    text_sect = NULL;
    data_sect = NULL;
    bss_sect  = NULL;
/*
 *  Loop through the sections and find the various types
 */
    if (status >= 0) {
	int nIndex;
	COFF_SCNHDR *sect_ptr = sect_bufr;

	for (nIndex = 0; nIndex < sections; ++nIndex) {
	    long int sect_flags = COFF_LONG (sect_ptr->s_flags);

	    switch (sect_flags) {
	    case COFF_STYP_TEXT:
		text_sect = sect_ptr;
		++text_count;
		status = is_properly_aligned (sect_ptr);
		break;

	    case COFF_STYP_DATA:
		data_sect = sect_ptr;
		++data_count;
		status = is_properly_aligned (sect_ptr);
		break;

	    case COFF_STYP_BSS:
		bss_sect = sect_ptr;
		++bss_count;
		break;

	    case COFF_STYP_LIB:
#ifdef COFF_DEBUG
		printk (".lib section found\n");
#endif
		++lib_count;
		break;

	    default:
		break;
	    }
	    sect_ptr = (COFF_SCNHDR *) & ((char *) sect_ptr)[COFF_SCNHSZ];
	}
/*
 *  Ensure that there are the required sections. There must be one text
 *  sections and one each of the data and bss sections for an executable.
 *  A library may or may not have a data / bss section.
 */
	if (text_count != 1) {
	    status = -ENOEXEC;
#ifdef COFF_DEBUG
	    printk ("no text sections\n");
#endif
	}
	else {
	    if (lib_ok) {
		if (data_count != 1 || bss_count != 1) {
		    status = -ENOEXEC;
#ifdef COFF_DEBUG
		    printk ("no .data nor .bss sections\n");
#endif
		}
	    }
	}
    }
/*
 *  If there is no additional header then assume the file starts at
 *  the first byte of the text section. This may not be the proper place,
 *  so the best solution is to include the optional header. A shared library
 *  __MUST__ have an optional header to indicate that it is a shared library.
 */
    if (status >= 0) {
	if (aout_size == 0) {
	    if (!lib_ok) {
		status = -ENOEXEC;
#ifdef COFF_DEBUG
		printk ("no header in library\n");
#endif
	    }
	    start_addr = COFF_LONG (text_sect->s_vaddr);
	}
/*
 *  There is some header. Ensure that it is sufficient.
 */
	else {
	    if (aout_size < COFF_AOUTSZ) {
		status = -ENOEXEC;
#ifdef COFF_DEBUG
		printk ("header too small\n");
#endif
	    }
	    else {
		COFF_AOUTHDR *aout_hdr =	/* Pointer to a.out header */
		(COFF_AOUTHDR *) & ((char *) coff_hdr)[COFF_FILHSZ];
		short int aout_magic = COFF_SHORT (aout_hdr->magic); /* id */
/*
 *  Validate the magic number in the a.out header. If it is valid then
 *  update the starting symbol location. Do not accept these file formats
 *  when loading a shared library.
 */
		switch (aout_magic) {
		case COFF_OMAGIC:
		case COFF_ZMAGIC:
		case COFF_STMAGIC:
		    if (!lib_ok) {
			status = -ENOEXEC;
#ifdef COFF_DEBUG
			printk ("wrong a.out header magic\n");
#endif
		    }
		    start_addr = (unsigned int) COFF_LONG (aout_hdr->entry);
		    break;
/*
 *  Magic value for a shared library. This is valid only when loading a
 *  shared library. (There is no need for a start_addr. It won't be used.)
 */
		case COFF_SHMAGIC:
		    if (lib_ok) {
#ifdef COFF_DEBUG
			printk ("wrong a.out header magic\n");
#endif
			status = -ENOEXEC;
		    }
		    break;

		default:
#ifdef COFF_DEBUG
		    printk ("wrong a.out header magic\n");
#endif
		    status = -ENOEXEC;
		    break;
		}
	    }
	}
    }
/*
 *  Fetch a file pointer to the executable.
 */
    if (status >= 0) {
	fd = open_inode (bprm->inode, O_RDONLY);
	if (fd < 0) {
#ifdef COFF_DEBUG
	    printk ("can not open inode, result = %d\n", fd);
#endif
	    status = fd;
	}
	else
	    fp = current->filp[fd];
    }
    else
	fd = -1;		/* Invalidate the open file descriptor */
/*
 *  Generate the proper values for the text fields
 *
 *  THIS IS THE POINT OF NO RETURN. THE NEW PROCESS WILL TRAP OUT SHOULD
 *  SOMETHING FAIL IN THE LOAD SEQUENCE FROM THIS POINT ONWARD.
 */
    if (status >= 0) {
	long text_scnptr = COFF_LONG (text_sect->s_scnptr);
	long text_size   = COFF_LONG (text_sect->s_size);
	long text_vaddr  = COFF_LONG (text_sect->s_vaddr);

	long data_scnptr;
	long data_size;
	long data_vaddr;

	long bss_size;
	long bss_vaddr;
/*
 *  Generate the proper values for the data fields
 */
	if (data_sect != NULL) {
	    data_scnptr = COFF_LONG (data_sect->s_scnptr);
	    data_size   = COFF_LONG (data_sect->s_size);
	    data_vaddr  = COFF_LONG (data_sect->s_vaddr);
	}
	else {
	    data_scnptr = 0;
	    data_size   = 0;
	    data_vaddr  = 0;
	}
/*
 *  Generate the proper values for the bss fields
 */
	if (bss_sect != NULL) {
	    bss_size  = COFF_LONG (bss_sect->s_size);
	    bss_vaddr = COFF_LONG (bss_sect->s_vaddr);
	}
	else {
	    bss_size  = 0;
	    bss_vaddr = 0;
	}
/*
 *  Flush the executable from memory. At this point the executable is
 *  committed to being defined or a segmentation violation will occur.
 */
	if (lib_ok) {
#ifdef COFF_DEBUG
	    printk ("flushing executable\n");
#endif
	    flush_old_exec (bprm);
/*
 *  Define the initial locations for the various items in the new process
 */
	    current->mmap        = NULL;
	    current->rss         = 0;
/*
 *  Construct the parameter and environment string table entries.
 */
	    bprm->p += change_ldt (0, bprm->page);
	    bprm->p -= MAX_ARG_PAGES*PAGE_SIZE;
	    bprm->p  = (unsigned long) create_tables ((char *) bprm->p,
						      bprm->argc,
						      bprm->envc,
						      1);
/*
 *  Do the end processing once the stack has been constructed
 */
	    current->start_code  = text_vaddr & PAGE_MASK;
	    current->end_code    = text_vaddr + text_size;
	    current->end_data    = data_vaddr + data_size;
	    current->start_brk   =
	    current->brk         = bss_vaddr + bss_size;
	    current->suid        =
	    current->euid        = bprm->e_uid;
	    current->sgid        =
	    current->egid        = bprm->e_gid;
	    current->executable  = bprm->inode; /* Store inode for file  */
	    ++bprm->inode->i_count;             /* Count the open inode  */
	    regs->eip            = start_addr;  /* Current EIP register  */
	    regs->esp            =
	    current->start_stack = bprm->p;
	}
/*
 *   Map the text pages
 */

#ifdef COFF_DEBUG
	printk (".text: vaddr = %d, size = %d, scnptr = %d\n",
		 text_vaddr,
		 text_size,
		 text_scnptr);
#endif
	status = do_mmap (fp,
			  text_vaddr & PAGE_MASK,
			  text_size + (text_vaddr & ~PAGE_MASK),
			  PROT_READ | PROT_EXEC,
			  MAP_FIXED | MAP_SHARED,
			  text_scnptr & PAGE_MASK);

	status = (status == (text_vaddr & PAGE_MASK)) ? 0 : -ENOEXEC;
/*
 *   Map the data pages
 */
	if (status >= 0 && data_size != 0) {
#ifdef COFF_DEBUG
	    printk (".data: vaddr = %d, size = %d, scnptr = %d\n",
		     data_vaddr,
		     data_size,
		     data_scnptr);
#endif
	    status = do_mmap (fp,
			      data_vaddr & PAGE_MASK,
			      data_size + (data_vaddr & ~PAGE_MASK),
			      PROT_READ | PROT_WRITE | PROT_EXEC,
			      MAP_FIXED | MAP_PRIVATE,
			      data_scnptr & PAGE_MASK);

	    status = (status == (data_vaddr & PAGE_MASK)) ? 0 : -ENOEXEC;
	}
/*
 *   Construct the bss data for the process. The bss ranges from the
 *   end of the data (which may not be on a page boundry) to the end
 *   of the bss section. Allocate any necessary pages for the data.
 */
	if (status >= 0 && bss_size != 0) {
#ifdef COFF_DEBUG
	    printk (".bss: vaddr = %d, size = %d\n",
		     bss_vaddr,
		     bss_size);
#endif
	    zeromap_page_range (PAGE_ALIGN (bss_vaddr),
				PAGE_ALIGN (bss_size),
				PAGE_COPY);

	    status = clear_memory (bss_vaddr, bss_size);
	}
/*
 *  Load any shared library for the executable.
 */
	if (status >= 0 && lib_ok && lib_count != 0) {
	    int nIndex;
	    COFF_SCNHDR *sect_ptr = sect_bufr;
/*
 *  Find the library sections. (There should be atleast one. It was counted
 *  earlier.) This will evenutally recurse to our code and load the shared
 *  library with our own procedures.
 */
	    for (nIndex = 0; nIndex < sections; ++nIndex) {
		long int sect_flags = COFF_LONG (sect_ptr->s_flags);
		if (sect_flags == COFF_STYP_LIB) {
		    status = preload_library (bprm, sect_ptr, fp);
		    if (status != 0)
			break;
		}
	    sect_ptr = (COFF_SCNHDR *) &((char *) sect_ptr) [COFF_SCNHSZ];
	    }
	}
/*
 *   Generate any needed trap for this process. If an error occured then
 *   generate a segmentation violation. If the process is being debugged
 *   then generate the load trap. (Note: If this is a library load then
 *   do not generate the trap here. Pass the error to the caller who
 *   will do it for the process in the outer lay of this procedure call.)
 */
	if (lib_ok) {
	    if (status < 0)
		send_sig (SIGSEGV, current, 0);	/* Generate the error trap  */
	    else {
		if (current->flags & PF_PTRACED)
		    send_sig (SIGTRAP, current, 0);
	    }
	    status = 0;		/* We are committed. It can't fail */
	}
    }
/*
 *  Do any cleanup processing
 */
    if (fd >= 0)
	sys_close (fd);		/* Close unused code file      */

    if (sect_bufr != NULL)
	kfree (sect_bufr);	/* Release section list buffer */
/*
 *  Return the completion status.
 */
#ifdef COFF_DEBUG
    printk ("binfmt_coff: result = %d\n", status);
#endif
    return (status);
}

/*
 *  This procedure will load the library listed in the file name given
 *  as the paramter. The result will be non-zero should something fail
 *  to load.
 */

static int
preload_this_library (struct linux_binprm *exe_bprm, char *lib_name)
{
    int   status;
    int   old_fs = get_fs();
/*
 *  If debugging then print "we have arrived"
 */
#ifdef COFF_DEBUG
    printk ("%s loading shared library %s\n",
	    exe_bprm->filename,
	    lib_name);
#endif
/*
 *  Change the FS register to the proper kernel address space and attempt
 *  to load the library. The library name is allocated from the kernel
 *  pool.
 */
    set_fs (get_ds ());
    status = sys_uselib (lib_name);
    set_fs (old_fs);
/*
 *  Return the success/failure to the caller.
 */
    return (status);
}

/*
 *  This procedure is called to load a library section. The various
 *  libraries are loaded from the list given in the section data.
 */

static int
preload_library (struct linux_binprm *exe_bprm,
		 COFF_SCNHDR * sect, struct file *fp)
{
    int status = 0;		/* Completion status                  */
    long nbytes;		/* Count of bytes in the header area  */
/*
 *  Fetch the size of the section. There must be enough room for atleast
 *  one entry.
 */
    nbytes = COFF_LONG (sect->s_size);
    if (nbytes < COFF_SLIBSZ) {
	status = -ENOEXEC;
#ifdef COFF_DEBUG
	printk ("library section too small\n");
#endif
    }
/*
 *  Allocate a buffer to hold the section data
 */
    else {
	COFF_SLIBHD *phdr;
	char *buffer = (char *) kmalloc (nbytes, GFP_KERNEL);

        if (0 == buffer) {
	    status = -ENOEXEC;
#ifdef COFF_DEBUG
	    printk ("kmalloc failed\n");
#endif
	}
	else {
	    int old_fs   = get_fs ();
/*
 *  Read the section data from the disk file.
 */
	    set_fs (get_ds ());   /* Make it point to the proper location    */
	    status = read_exec (exe_bprm->inode,     /* INODE for file       */
			    COFF_LONG (sect->s_scnptr), /* Disk location     */
			    buffer,                     /* Buffer for read   */
			    nbytes);                    /* Byte count reqd.  */
	    set_fs (old_fs);                         /* Restore the selector */
/*
 *  Check the result. The value returned is the byte count actaully read.
 */
	    if (status >= 0 && status != nbytes) {
#ifdef COFF_DEBUG
		printk ("read of lib section was short\n");
#endif
		status = -ENOEXEC;
	    }
	}
/*
 *  At this point, go through the list of libraries in the data area.
 */
	phdr = (COFF_SLIBHD *) buffer;
	while (status >= 0 && nbytes > COFF_SLIBSZ) {
	    int entry_size  = COFF_LONG (phdr->sl_entsz)   * sizeof (long);
	    int header_size = COFF_LONG (phdr->sl_pathndx) * sizeof (long);
/*
 *  Validate the sizes of the various items. I don't trust the linker!!
 */
	    if ((unsigned) header_size >= (unsigned) nbytes ||
		entry_size <= 0 ||
		(unsigned) entry_size <= (unsigned) header_size) {
		status = -ENOEXEC;
#ifdef COFF_DEBUG
		printk ("header count is invalid\n");
#endif
	    }
/*
 *  Load the library. Stop the load process on the first error.
 */
	    else {
		status = preload_this_library (exe_bprm,
					       &((char *) phdr)[header_size]);
#ifdef COFF_DEBUG
		printk ("preload_this_library result = %d\n", status);
#endif
	    }
/*
 *  Point to the next library in the section data.
 */
	    nbytes -= entry_size;
	    phdr = (COFF_SLIBHD *) &((char *) phdr)[entry_size];
	}
/*
 *  Release the space for the library list.
 */
	if (buffer != NULL)
	    kfree (buffer);
    }
/*
 *  Return the resulting status to the caller.
 */
    return (status);
}

/*
 *  This procedure is called by the main load sequence. It will load
 *  the executable and prepare it for execution. It provides the additional
 *  parameters used by the recursive coff loader and tells the loader that
 *  this is the main executable. How simple it is . . . .
 */

int
load_coff_binary (struct linux_binprm *bprm, struct pt_regs *regs)
{
    return (load_object (bprm, regs, 1));
}

/*
 *   Load the image for any shared library.
 *
 *   This is called when we need to load a library based upon a file name.
 */

int
load_coff_library (int fd)
{
    struct linux_binprm *bprm;  /* Parameters for the load operation   */
    int    status;              /* Status of the request               */
/*
 *  Read the first portion of the file.
 */
    bprm = (struct linux_binprm *) kmalloc (sizeof (struct linux_binprm),
					    GFP_KERNEL);
    if (0 == bprm) {
#ifdef COFF_DEBUG
	printk ("kmalloc failed\n");
#endif
        status = -ENOEXEC;
    }
    else {
        struct file         *file;  /* Pointer to the file table           */
        struct pt_regs       regs;  /* Register work area                  */
        int old_fs = get_fs ();     /* Previous FS register value          */

        memset (bprm, '\0', sizeof (struct linux_binprm));

	file           = current->filp[fd];
	bprm->inode    = file->f_inode;   /* The only item _really_ needed */
	bprm->filename = "";              /* Make it a legal string        */
/*
 *  Read the section list from the disk file.
 */
	set_fs (get_ds ());   /* Make it point to the proper location    */
	status = read_exec (bprm->inode,	 /* INODE for file       */
			    0L,                  /* Offset in the file   */
			    bprm->buf,           /* Buffer for read      */
			    sizeof (bprm->buf)); /* Size of the buffer   */
	set_fs (old_fs);	                 /* Restore the selector */
/*
 *  Try to load the library.
 */
	status = load_object (bprm, &regs, 0);
/*
 *  Release the work buffer and return the result.
 */
	kfree (bprm);                 /* Release the buffer area */
    }
/*
 *  Return the result of the load operation
 */
    return (status);
}
