#
# Makefile for the kernel block device drivers.
#
# Note! Dependencies are done automagically by 'make dep', which also
# removes any old dependencies. DON'T put your own dependencies here
# unless it's something special (ie not a .c file).
#
# Note 2! The CFLAGS definition is now inherited from the
# parent makefile.
#

.c.s:
	$(CC) $(CFLAGS) -S $<
.s.o:
	$(AS) -c -o $*.o $<
.c.o:
	$(CC) $(CFLAGS) -c $<

#
# Note : at this point, these files are compiled on all systems. 
# In the future, some of these should be built conditionally.
#

OBJS := ll_rw_blk.o floppy.o ramdisk.o genhd.o 
SRCS := ll_rw_blk.c floppy.c ramdisk.c genhd.c 

ifdef CONFIG_CDU31A
OBJS := $(OBJS) cdu31a.o
SRCS := $(SRCS) cdu31a.c
endif

ifdef CONFIG_MCD
OBJS := $(OBJS) mcd.o
SRCS := $(SRCS) mcd.c
endif

ifdef CONFIG_SBPCD
OBJS := $(OBJS) sbpcd.o
SRCS := $(SRCS) sbpcd.c
endif #CONFIG_SBPCD

ifdef CONFIG_BLK_DEV_HD
OBJS := $(OBJS) hd.o
SRCS := $(SRCS) hd.c
endif

ifdef CONFIG_BLK_DEV_XD
OBJS := $(OBJS) xd.o
SRCS := $(SRCS) xd.c
endif

all: block.a

block.a: $(OBJS)
	rm -f block.a
	$(AR) rcs block.a $(OBJS)
	sync

dep:
	$(CPP) -M $(SRCS) > .depend

dummy:

#
# include a dependency file if one exists
#
ifeq (.depend,$(wildcard .depend))
include .depend
endif
