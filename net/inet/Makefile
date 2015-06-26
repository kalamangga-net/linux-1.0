#
# Makefile for the Linix TCP/IP (INET) layer.
#
# Note! Dependencies are done automagically by 'make dep', which also
# removes any old dependencies. DON'T put your own dependencies here
# unless it's something special (ie not a .c file).
#
# Note 2! The CFLAGS definition is now in the main makefile...

.c.o:
	$(CC) $(CFLAGS) -c -o $*.o $<
.s.o:
	$(AS) -o $*.o $<
.c.s:
	$(CC) $(CFLAGS) -S -o $*.s $<


OBJS	= sock.o utils.o route.o proc.o timer.o protocol.o loopback.o \
	  eth.o packet.o arp.o dev.o ip.o raw.o icmp.o tcp.o udp.o \
	  datagram.o skbuff.o
#	  ipx.o ax25.o ax25_in.o ax25_out.o ax25_subr.o ax25_timer.o

ifdef CONFIG_INET

inet.o:		$(OBJS)
		$(LD) -r -o inet.o $(OBJS)

else

inet.o:
		echo | $(AS) -o inet.o

endif

dep:
		$(CPP) -M *.c > .depend

tar:
		tar -cvf /dev/f1 .

#
# include a dependency file if one exists
#
ifeq (.depend,$(wildcard .depend))
include .depend
endif
