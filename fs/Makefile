#
# Makefile for the linux filesystem.
#
# Note! Dependencies are done automagically by 'make dep', which also
# removes any old dependencies. DON'T put your own dependencies here
# unless it's something special (ie not a .c file).
#
# Note 2! The CFLAGS definitions are now in the main makefile...

SUBDIRS = minix ext ext2 msdos proc isofs nfs xiafs hpfs sysv

ifdef CONFIG_MINIX_FS
FS_SUBDIRS := $(FS_SUBDIRS) minix
endif
ifdef CONFIG_EXT_FS
FS_SUBDIRS := $(FS_SUBDIRS) ext
endif
ifdef CONFIG_EXT2_FS
FS_SUBDIRS := $(FS_SUBDIRS) ext2
endif
ifdef CONFIG_MSDOS_FS
FS_SUBDIRS := $(FS_SUBDIRS) msdos
endif
ifdef CONFIG_PROC_FS
FS_SUBDIRS := $(FS_SUBDIRS) proc
endif
ifdef CONFIG_ISO9660_FS
FS_SUBDIRS := $(FS_SUBDIRS) isofs
endif
ifdef CONFIG_NFS_FS
FS_SUBDIRS := $(FS_SUBDIRS) nfs
endif
ifdef CONFIG_XIA_FS
FS_SUBDIRS := $(FS_SUBDIRS) xiafs
endif
ifdef CONFIG_SYSV_FS
FS_SUBDIRS := $(FS_SUBDIRS) sysv
endif
ifdef CONFIG_HPFS_FS
FS_SUBDIRS := $(FS_SUBDIRS) hpfs
endif

ifdef CONFIG_BINFMT_ELF
BINFMTS := $(BINFMTS) binfmt_elf.o
endif
ifdef CONFIG_BINFMT_COFF
BINFMTS := $(BINFMTS) binfmt_coff.o
endif

.c.s:
	$(CC) $(CFLAGS) -S $<
.c.o:
	$(CC) $(CFLAGS) -c $<
.s.o:
	$(AS) -o $*.o $<

OBJS=	open.o read_write.o inode.o devices.o file_table.o buffer.o super.o \
	block_dev.o stat.o exec.o pipe.o namei.o fcntl.o ioctl.o \
	select.o fifo.o locks.o filesystems.o $(BINFMTS)

all: fs.o filesystems.a

fs.o: $(OBJS)
	$(LD) -r -o fs.o $(OBJS)

filesystems.a: dummy
	rm -f filesystems.a
	set -e; for i in $(FS_SUBDIRS); do \
	  test ! -d $$i || \
	    { $(MAKE) -C $$i; $(AR) rcs filesystems.a $$i/$$i.o; }; done

depend dep:
	$(CPP) -M *.c > .depend
	set -e; for i in $(SUBDIRS); do \
	  test ! -d $$i || $(MAKE) -C $$i dep; done

dummy:

#
# include a dependency file if one exists
#
ifeq (.depend,$(wildcard .depend))
include .depend
endif
