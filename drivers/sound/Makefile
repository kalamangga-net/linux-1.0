# Makefile for the Linux sound card driver
#
# Note 2! The CFLAGS definitions are now inherited from the
# parent makes. (hopefully)
#
#

VERSION		= 2.4
TARGET_OS	= linux

.c.s:
	$(CC) $(CFLAGS) -S $<
.s.o:
	$(AS) -c -o $*.o $<
.c.o:
	$(CC) $(CFLAGS) -c $<

OBJS   = soundcard.o audio.o dmabuf.o sb_dsp.o dev_table.o \
	 opl3.o sequencer.o midibuf.o sb_card.o pas2_card.o adlib_card.o \
	 pas2_pcm.o pas2_mixer.o pas2_midi.o gus_card.o gus_wave.o mpu401.o \
	 gus_midi.o gus_vol.o patmgr.o sb_mixer.o sb16_dsp.o sb_midi.o \
	 sb16_midi.o sound_switch.o

all:	local.h sound.a

/usr/include/sys/soundcard.h:
	@echo "WARNING! Your /usr/include/sys/soundcard.h not found."
	@echo "Please make a new /usr/include/sys/soundcard.h containing"
	@echo "just a line #include <linux/soundcard.h>"

sound.a: $(OBJS) 
	-rm -f sound.a
	$(AR) rcs sound.a $(OBJS)
	sync

clean:
	rm -f core core.* *.o *.a tmp_make *~ x z *%
	rm -f configure sound_stub.c
	for i in *.c;do rm -f `basename $$i .c`.s;done

indent:
	for n in *.c;do echo indent $$n;indent $$n;done

local.h:
	$(MAKE) clean
	$(MAKE) config
	$(MAKE) dep

config: configure /usr/include/sys/soundcard.h
	@echo Compiling Sound Driver v $(VERSION) for Linux
	@./configure > local.h
	@echo \#define SOUND_VERSION_STRING \"$(VERSION)\" >> local.h
	@echo \#define SOUND_CONFIG_DATE \"`date`\" >> local.h
	@echo \#define SOUND_CONFIG_BY \"`whoami`\" >> local.h
	@echo \#define SOUND_CONFIG_HOST \"`hostname`\" >> local.h
	@echo \#define SOUND_CONFIG_DOMAIN \"`domainname`\" >> local.h

clrconf:
	rm -f local.h .depend

configure: configure.c /usr/include/sys/soundcard.h
	$(HOSTCC) -o configure configure.c
	@cat .blurb

dep:
	$(MAKE) /usr/include/sys/soundcard.h
	$(CPP) -M *.c > .depend

#
# include a dependency file if one exists
#
ifeq (.depend,$(wildcard .depend))
include .depend
endif
