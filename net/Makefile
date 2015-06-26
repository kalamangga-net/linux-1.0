#
# Makefile for the linux networking.
#
# Note! Dependencies are done automagically by 'make dep', which also
# removes any old dependencies. DON'T put your own dependencies here
# unless it's something special (ie not a .c file).
#
# Note 2! The CFLAGS definition is now in the main makefile...

# only these two lines should need to be changed to remove inet sockets.
# (and the inet/tcpip.o in net.o)

SUBDIRS     := unix inet

SUBOBJS     := $(foreach f,$(SUBDIRS),$f/$f.o)

.c.o:
	$(CC) $(CFLAGS) -c $<
.s.o:
	$(AS) -o $*.o $<
.c.s:
	$(CC) $(CFLAGS) -S $<

OBJS	=  Space.o ddi.o socket.o

all:		subdirs net.o

net.o:		$(OBJS) network.a
		$(LD) -r -o net.o $(OBJS) network.a

network.a:	$(SUBOBJS)
		rm -f $@
		ar rc $@ $(SUBOBJS)
		ranlib $@

subdirs:	dummy
		set -e; for i in $(SUBDIRS); do $(MAKE) -C $$i; done

dep:
		$(CPP) -M *.c > .depend
		set -e; for i in $(SUBDIRS); do $(MAKE) -C $$i dep; done

dummy:

#
# include a dependency file if one exists
#
ifeq (.depend,$(wildcard .depend))
include .depend
endif

