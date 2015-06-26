#
# Makefile for the linux ipc.
#
# Note! Dependencies are done automagically by 'make dep', which also
# removes any old dependencies. DON'T put your own dependencies here
# unless it's something special (ie not a .c file).
#
# Note 2! The CFLAGS definition is now in the main makefile...

.c.o:
	$(CC) $(CFLAGS) -c $<
.s.o:
	$(AS) -o $*.o $<
.c.s:
	$(CC) $(CFLAGS) -S $<

OBJS	=  util.o 
SRCS	=  util.c

ifdef CONFIG_SYSVIPC
OBJS	:= $(OBJS) msg.o sem.o shm.o 
SRCS	:= $(SRCS) msg.c sem.c shm.c
endif

ipc.o: $(OBJS) 
	$(LD) -r -o ipc.o $(OBJS) 

dep:
	$(CPP) -M $(SRCS) > .depend

dummy:

#
# include a dependency file if one exists
#
ifeq (.depend,$(wildcard .depend))
include .depend
endif
