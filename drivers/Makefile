#
# Makefile for the linux kernel device drivers.
#
# Note! Dependencies are done automagically by 'make dep', which also
# removes any old dependencies. DON'T put your own dependencies here
# unless it's something special (ie not a .c file).
#
# Note 2! The CFLAGS definitions are now in the main makefile...

.S.s:
	$(CPP) -traditional $< -o $*.s
.c.s:
	$(CC) $(CFLAGS) -S $<
.s.o:
	$(AS) -c -o $*.o $<
.c.o:
	$(CC) $(CFLAGS) -c $<

SUBDIRS	= block char net

ifdef CONFIG_MATH_EMULATION
SUBDIRS := $(SUBDIRS) FPU-emu
endif

ifdef CONFIG_SCSI
SUBDIRS := $(SUBDIRS) scsi
endif

ifdef CONFIG_SOUND
SUBDIRS := $(SUBDIRS) sound
endif

all: driversubdirs

driversubdirs: dummy
	set -e; for i in $(SUBDIRS); do $(MAKE) -C $$i; done

dep:
	set -e; for i in $(SUBDIRS); do $(MAKE) -C $$i dep; done

dummy:

#
# include a dependency file if one exists
#
ifeq (.depend,$(wildcard .depend))
include .depend
endif

