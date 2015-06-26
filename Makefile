VERSION = 1
PATCHLEVEL = 0
ALPHA =

all:	Version zImage

.EXPORT_ALL_VARIABLES:

CONFIG_SHELL := $(shell if [ -x "$$BASH" ]; then echo $$BASH; \
	  else if [ -x /bin/bash ]; then echo /bin/bash; \
	  else echo sh; fi ; fi)

#
# Make "config" the default target if there is no configuration file or
# "depend" the target if there is no top-level dependency information.
#
ifeq (.config,$(wildcard .config))
include .config
ifeq (.depend,$(wildcard .depend))
include .depend
else
CONFIGURATION = depend
endif
else
CONFIGURATION = config
endif

ifdef CONFIGURATION
CONFIGURE = dummy
endif

#
# ROOT_DEV specifies the default root-device when making the image.
# This can be either FLOPPY, CURRENT, /dev/xxxx or empty, in which case
# the default of FLOPPY is used by 'build'.
#

ROOT_DEV = CURRENT

#
# If you want to preset the SVGA mode, uncomment the next line and
# set SVGA_MODE to whatever number you want.
# Set it to -DSVGA_MODE=NORMAL_VGA if you just want the EGA/VGA mode.
# The number is the same as you would ordinarily press at bootup.
#

SVGA_MODE=	-DSVGA_MODE=NORMAL_VGA

#
# standard CFLAGS
#

CFLAGS = -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe

ifdef CONFIG_CPP
CFLAGS := $(CFLAGS) -x c++
endif

ifdef CONFIG_M486
CFLAGS := $(CFLAGS) -m486
else
CFLAGS := $(CFLAGS) -m386
endif

#
# if you want the ram-disk device, define this to be the
# size in blocks.
#

#RAMDISK = -DRAMDISK=512

AS86	=as86 -0 -a
LD86	=ld86 -0

AS	=as
LD	=ld
HOSTCC	=gcc
CC	=gcc -D__KERNEL__
MAKE	=make
CPP	=$(CC) -E
AR	=ar
STRIP	=strip

ARCHIVES	=kernel/kernel.o mm/mm.o fs/fs.o net/net.o ipc/ipc.o
FILESYSTEMS	=fs/filesystems.a
DRIVERS		=drivers/block/block.a \
		 drivers/char/char.a \
		 drivers/net/net.a \
		 ibcs/ibcs.o
LIBS		=lib/lib.a
SUBDIRS		=kernel drivers mm fs net ipc ibcs lib

KERNELHDRS	=/usr/src/linux/include

ifdef CONFIG_SCSI
DRIVERS := $(DRIVERS) drivers/scsi/scsi.a
endif

ifdef CONFIG_SOUND
DRIVERS := $(DRIVERS) drivers/sound/sound.a
endif

ifdef CONFIG_MATH_EMULATION
DRIVERS := $(DRIVERS) drivers/FPU-emu/math.a
endif

.c.s:
	$(CC) $(CFLAGS) -S -o $*.s $<
.s.o:
	$(AS) -c -o $*.o $<
.c.o:
	$(CC) $(CFLAGS) -c -o $*.o $<

Version: dummy
	rm -f tools/version.h

config:
	$(CONFIG_SHELL) Configure $(OPTS) < config.in
	@if grep -s '^CONFIG_SOUND' .tmpconfig ; then \
		$(MAKE) -C drivers/sound config; \
		else : ; fi
	mv .tmpconfig .config

linuxsubdirs: dummy
	set -e; for i in $(SUBDIRS); do $(MAKE) -C $$i; done

tools/./version.h: tools/version.h

tools/version.h: $(CONFIGURE) Makefile
	@./makever.sh
	@echo \#define UTS_RELEASE \"$(VERSION).$(PATCHLEVEL)$(ALPHA)\" > tools/version.h
	@echo \#define UTS_VERSION \"\#`cat .version` `date`\" >> tools/version.h
	@echo \#define LINUX_COMPILE_TIME \"`date +%T`\" >> tools/version.h
	@echo \#define LINUX_COMPILE_BY \"`whoami`\" >> tools/version.h
	@echo \#define LINUX_COMPILE_HOST \"`hostname`\" >> tools/version.h
	@echo \#define LINUX_COMPILE_DOMAIN \"`domainname`\" >> tools/version.h

tools/build: tools/build.c $(CONFIGURE)
	$(HOSTCC) $(CFLAGS) -o $@ $<

boot/head.o: $(CONFIGURE) boot/head.s

boot/head.s: boot/head.S $(CONFIGURE) include/linux/tasks.h
	$(CPP) -traditional $< -o $@

tools/version.o: tools/version.c tools/version.h

init/main.o: $(CONFIGURE) init/main.c
	$(CC) $(CFLAGS) $(PROFILING) -c -o $*.o $<

tools/system:	boot/head.o init/main.o tools/version.o linuxsubdirs
	$(LD) $(LDFLAGS) -Ttext 1000 boot/head.o init/main.o tools/version.o \
		$(ARCHIVES) \
		$(FILESYSTEMS) \
		$(DRIVERS) \
		$(LIBS) \
		-o tools/system
	nm tools/zSystem | grep -v '\(compiled\)\|\(\.o$$\)\|\( a \)' | \
		sort > System.map

boot/setup: boot/setup.o
	$(LD86) -s -o $@ $<

boot/setup.o: boot/setup.s
	$(AS86) -o $@ $<

boot/setup.s: boot/setup.S $(CONFIGURE) include/linux/config.h Makefile
	$(CPP) -traditional $(SVGA_MODE) $(RAMDISK) $< -o $@

boot/bootsect: boot/bootsect.o
	$(LD86) -s -o $@ $<

boot/bootsect.o: boot/bootsect.s
	$(AS86) -o $@ $<

boot/bootsect.s: boot/bootsect.S $(CONFIGURE) include/linux/config.h Makefile
	$(CPP) -traditional $(SVGA_MODE) $(RAMDISK) $< -o $@

zBoot/zSystem: zBoot/*.c zBoot/*.S tools/zSystem
	$(MAKE) -C zBoot

zImage: $(CONFIGURE) boot/bootsect boot/setup zBoot/zSystem tools/build
	tools/build boot/bootsect boot/setup zBoot/zSystem $(ROOT_DEV) > zImage
	sync

zdisk: zImage
	dd bs=8192 if=zImage of=/dev/fd0

zlilo: $(CONFIGURE) zImage
	if [ -f /vmlinuz ]; then mv /vmlinuz /vmlinuz.old; fi
	if [ -f /zSystem.map ]; then mv /zSystem.map /zSystem.old; fi
	cat zImage > /vmlinuz
	cp zSystem.map /
	if [ -x /sbin/lilo ]; then /sbin/lilo; else /etc/lilo/install; fi

tools/zSystem:	boot/head.o init/main.o tools/version.o linuxsubdirs
	$(LD) $(LDFLAGS) -Ttext 100000 boot/head.o init/main.o tools/version.o \
		$(ARCHIVES) \
		$(FILESYSTEMS) \
		$(DRIVERS) \
		$(LIBS) \
		-o tools/zSystem
	nm tools/zSystem | grep -v '\(compiled\)\|\(\.o$$\)\|\( a \)' | \
		sort > zSystem.map

fs: dummy
	$(MAKE) linuxsubdirs SUBDIRS=fs

lib: dummy
	$(MAKE) linuxsubdirs SUBDIRS=lib

mm: dummy
	$(MAKE) linuxsubdirs SUBDIRS=mm

ipc: dummy
	$(MAKE) linuxsubdirs SUBDIRS=ipc

kernel: dummy
	$(MAKE) linuxsubdirs SUBDIRS=kernel

drivers: dummy
	$(MAKE) linuxsubdirs SUBDIRS=drivers

net: dummy
	$(MAKE) linuxsubdirs SUBDIRS=net

clean:
	rm -f kernel/ksyms.lst
	rm -f core `find . -name '*.[oas]' -print`
	rm -f core `find . -name 'core' -print`
	rm -f zImage zSystem.map tools/zSystem tools/system
	rm -f Image System.map boot/bootsect boot/setup
	rm -f zBoot/zSystem zBoot/xtract zBoot/piggyback
	rm -f .tmp* drivers/sound/configure
	rm -f init/*.o tools/build boot/*.o tools/*.o

mrproper: clean
	rm -f include/linux/autoconf.h tools/version.h
	rm -f drivers/sound/local.h
	rm -f .version .config* config.old
	rm -f .depend `find . -name .depend -print`

distclean: mrproper

backup: mrproper
	cd .. && tar cf - linux | gzip -9 > backup.gz
	sync

depend dep:
	touch tools/version.h
	for i in init/*.c;do echo -n "init/";$(CPP) -M $$i;done > .tmpdepend
	for i in tools/*.c;do echo -n "tools/";$(CPP) -M $$i;done >> .tmpdepend
	set -e; for i in $(SUBDIRS); do $(MAKE) -C $$i dep; done
	rm -f tools/version.h
	mv .tmpdepend .depend

ifdef CONFIGURATION
..$(CONFIGURATION):
	@echo
	@echo "You have a bad or nonexistent" .$(CONFIGURATION) ": running 'make" $(CONFIGURATION)"'"
	@echo
	$(MAKE) $(CONFIGURATION)
	@echo
	@echo "Successful. Try re-making (ignore the error that follows)"
	@echo
	exit 1

dummy: ..$(CONFIGURATION)

else

dummy:

endif

#
# Leave these dummy entries for now to tell people that they are going away..
#
lilo:
	@echo
	@echo Uncompressed kernel images no longer supported. Use
	@echo \"make zlilo\" instead.
	@echo
	@exit 1

Image:
	@echo
	@echo Uncompressed kernel images no longer supported. Use
	@echo \"make zImage\" instead.
	@echo
	@exit 1

disk:
	@echo
	@echo Uncompressed kernel images no longer supported. Use
	@echo \"make zdisk\" instead.
	@echo
	@exit 1
