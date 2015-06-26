# File: drivers/net/Makefile
#
# Makefile for the Linux network (ethercard) device drivers.
#

# This will go away in some future future: hidden configuration files
# are difficult for users to deal with.
include CONFIG

NETDRV_OBJS := net.a(Space.o) net.a(auto_irq.o) net.a(net_init.o)
CFLAGS := $(CFLAGS) -I../../net/inet
CPP := $(CPP) -I../../net/inet

# The point of the makefile...
all: net.a

Space.o: Space.c ../../include/linux/autoconf.h
	$(CC) $(CFLAGS) $(OPTS) $(DL_OPTS) -c $< -o $@

net_init.o: ../../include/linux/autoconf.h

ifdef CONFIG_WD80x3
NETDRV_OBJS := $(NETDRV_OBJS) net.a(wd.o)
CONFIG_8390 = CONFIG_8390
wd.o:	wd.c CONFIG
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WD_OPTS) -c $<
endif

ifdef CONFIG_EL2
NETDRV_OBJS := $(NETDRV_OBJS) net.a(3c503.o)
CONFIG_8390 = CONFIG_8390
3c503.o:	3c503.c CONFIG
	$(CC) $(CPPFLAGS) $(CFLAGS) $(EL2_OPTS) -c $<
endif

ifdef CONFIG_NE2000
NETDRV_OBJS := $(NETDRV_OBJS) net.a(ne.o)
CONFIG_8390 = CONFIG_8390
ne.o:	ne.c CONFIG
	$(CC) $(CPPFLAGS) $(CFLAGS) $(NE_OPTS) -c $<
endif

ifdef CONFIG_HPLAN
NETDRV_OBJS := $(NETDRV_OBJS) net.a(hp.o)
CONFIG_8390 = CONFIG_8390
hp.o:	hp.c CONFIG
	$(CC) $(CPPFLAGS) $(CFLAGS) $(HP_OPTS) -c $<
endif

ifdef CONFIG_ULTRA
NETDRV_OBJS := $(NETDRV_OBJS) net.a(smc-ultra.o)
CONFIG_8390 = CONFIG_8390
endif

ifdef CONFIG_E2100
NETDRV_OBJS := $(NETDRV_OBJS) net.a(e2100.o)
CONFIG_8390 = CONFIG_8390
endif

ifdef CONFIG_PLIP
NETDRV_OBJS := $(NETDRV_OBJS) net.a(plip.o)
plip.o:	plip.c CONFIG
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PLIP_OPTS) -c $<
endif

ifdef CONFIG_PPP
NETDRV_OBJS := $(NETDRV_OBJS) net.a(ppp.o) net.a(slhc.o)
endif

ifdef CONFIG_SLIP
NETDRV_OBJS := $(NETDRV_OBJS) net.a(slip.o) net.a(slhc.o)
slip.o:	slip.c CONFIG
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $<
endif

ifdef CONFIG_DE600
NETDRV_OBJS := $(NETDRV_OBJS) net.a(d_link.o)
d_link.o: d_link.c CONFIG
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DL_OPTS) -c $<
endif

ifdef CONFIG_AT1500
NETDRV_OBJS := $(NETDRV_OBJS) net.a(lance.o)
endif
ifdef CONFIG_LANCE
NETDRV_OBJS := $(NETDRV_OBJS) net.a(lance.o)
endif
ifdef CONFIG_AT1700
NETDRV_OBJS := $(NETDRV_OBJS) net.a(at1700.o)
endif
ifdef CONFIG_EL1
NETDRV_OBJS := $(NETDRV_OBJS) net.a(3c501.o)
endif
ifdef CONFIG_EL16
NETDRV_OBJS := $(NETDRV_OBJS) net.a(3c507.o)
endif
ifdef CONFIG_EL3
NETDRV_OBJS := $(NETDRV_OBJS) net.a(3c509.o)
endif
ifdef CONFIG_EEXPRESS
NETDRV_OBJS := $(NETDRV_OBJS) net.a(eexpress.o)
endif
ifdef CONFIG_ZNET
NETDRV_OBJS := $(NETDRV_OBJS) net.a(znet.o)
endif
ifdef CONFIG_DEPCA
NETDRV_OBJS := $(NETDRV_OBJS) net.a(depca.o)
depca.o: depca.c CONFIG
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPCA_OPTS) -c $<
endif
ifdef CONFIG_ATP
NETDRV_OBJS := $(NETDRV_OBJS) net.a(atp.o)
endif
ifdef CONFIG_NI52
NETDRV_OBJS := $(NETDRV_OBJS) net.a(ni52.o)
endif
ifdef CONFIG_NI65
NETDRV_OBJS := $(NETDRV_OBJS) net.a(ni65.o)
endif
ifdef CONFIG_ELPLUS
NETDRV_OBJS := $(NETDRV_OBJS) net.a(3c505.o)
endif
ifdef CONFIG_AC3200
NETDRV_OBJS := $(NETDRV_OBJS) net.a(ac3200.o)
CONFIG_8390 = CONFIG_8390
endif

ifdef CONFIG_8390
NETDRV_OBJS := $(NETDRV_OBJS) net.a(8390.o)
endif

ifdef CONFIG_IP_DEFRAG
NETDRV_OBJS := $(NETDRV_OBJS) net.a(ip-frag.o)
endif

net.a: $(NETDRV_OBJS)
	ranlib net.a

clean:
	rm -f core *.o *.a *.s

dep:
	$(CPP) -M *.c > .depend

tar:


# include a dependency file if one exists

ifeq (.depend,$(wildcard .depend))
include .depend
endif
