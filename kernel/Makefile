#
# Makefile for the linux kernel.
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

OBJS  = sched.o sys_call.o traps.o irq.o dma.o fork.o \
 	panic.o printk.o vsprintf.o sys.o module.o ksyms.o exit.o \
	signal.o mktime.o ptrace.o ioport.o itimer.o \
	info.o ldt.o time.o

all: kernel.o

kernel.o: $(OBJS)
	$(LD) -r -o kernel.o $(OBJS)
	sync

sys_call.s: sys_call.S

sys_call.o: sys_call.s

sched.o: sched.c
	$(CC) $(CFLAGS) $(PROFILING) -fno-omit-frame-pointer -c $<

ksyms.lst: ksyms.S ../include/linux/autoconf.h
	$(CPP) $(CFLAGS) $< > $@
 
ksyms.s: ksyms.sh ksyms.lst
	sh $< > $@

ksyms.o: ksyms.s

dep:
	$(CPP) -M *.c > .depend

dummy:

#
# include a dependency file if one exists
#
ifeq (.depend,$(wildcard .depend))
include .depend
endif

