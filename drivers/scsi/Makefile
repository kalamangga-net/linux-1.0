#
# Makefile for kernel/blk_drv/scsi
#
# Note! Dependencies are done automagically by 'make dep', which also
# removes any old dependencies. DONT put your own dependencies here
# unless its something special (ie not a .c file).
#

#AHA152X	= -DDEBUG -DAUTOCONF -DIRQ=11 -DSCSI_ID=7 -DRECONNECT=0 \
#          -DPORTBASE=0x340 -DSKIP_BIOSTEST -DDONT_SNARF

AHA152X	= -DDEBUG -DAUTOCONF

SCSI_OBJS =
SCSI_SRCS =

ifdef CONFIG_SCSI

SCSI_OBJS := hosts.o scsi.o scsi_ioctl.o constants.o
SCSI_SRCS := hosts.c scsi.c scsi_ioctl.c constants.c

ifdef CONFIG_CHR_DEV_ST
SCSI_OBJS := $(SCSI_OBJS) st.o
SCSI_SRCS := $(SCSI_SRCS) st.c
endif

ifdef CONFIG_BLK_DEV_SD
SCSI_OBJS := $(SCSI_OBJS) sd.o sd_ioctl.o
SCSI_SRCS := $(SCSI_SRCS) sd.c sd_ioctl.c
endif

ifdef CONFIG_BLK_DEV_SR
SCSI_OBJS := $(SCSI_OBJS) sr.o sr_ioctl.o
SCSI_SRCS := $(SCSI_SRCS) sr.c sr_ioctl.c
endif

ifdef CONFIG_CHR_DEV_SG
SCSI_OBJS := $(SCSI_OBJS) sg.o
SCSI_SRCS := $(SCSI_SRCS) sg.c
endif

ifdef CONFIG_SCSI_AHA152X
SCSI_OBJS := $(SCSI_OBJS) aha152x.o
SCSI_SRCS := $(SCSI_SRCS) aha152x.c
endif

ifdef CONFIG_SCSI_AHA1542
SCSI_OBJS := $(SCSI_OBJS) aha1542.o
SCSI_SRCS := $(SCSI_SRCS) aha1542.c
endif

ifdef CONFIG_SCSI_AHA1740
SCSI_OBJS := $(SCSI_OBJS) aha1740.o
SCSI_SRCS := $(SCSI_SRCS) aha1740.c
endif

ifdef CONFIG_SCSI_DEBUG
SCSI_OBJS := $(SCSI_OBJS) scsi_debug.o
SCSI_SRCS := $(SCSI_SRCS) scsi_debug.c
endif

ifdef CONFIG_SCSI_FUTURE_DOMAIN
SCSI_OBJS := $(SCSI_OBJS) fdomain.o
SCSI_SRCS := $(SCSI_SRCS) fdomain.c
endif

ifdef CONFIG_SCSI_GENERIC_NCR5380
SCSI_OBJS := $(SCSI_OBJS) g_NCR5380.o
SCSI_SRCS := $(SCSI_SRCS) g_NCR5380.c
endif

ifdef CONFIG_SCSI_PAS16
SCSI_OBJS := $(SCSI_OBJS) pas16.o
SCSI_SRCS := $(SCSI_SRCS) pas16.c
endif

ifdef CONFIG_SCSI_SEAGATE
SCSI_OBJS := $(SCSI_OBJS) seagate.o
SCSI_SRCS := $(SCSI_SRCS) seagate.c
else
ifdef CONFIG_SCSI_FD_8xx
SCSI_OBJS := $(SCSI_OBJS) seagate.o
SCSI_SRCS := $(SCSI_SRCS) seagate.c
endif
endif

ifdef CONFIG_SCSI_7000FASST
SCSI_OBJS := $(SCSI_OBJS) wd7000.o
SCSI_SRCS := $(SCSI_SRCS) wd7000.c
endif

ifdef CONFIG_SCSI_T128
SCSI_OBJS := $(SCSI_OBJS) t128.o
SCSI_SRCS := $(SCSI_SRCS) t128.c
endif

ifdef CONFIG_SCSI_ULTRASTOR
SCSI_OBJS := $(SCSI_OBJS) ultrastor.o
SCSI_SRCS := $(SCSI_SRCS) ultrastor.c
endif



scsi.a: $(SCSI_OBJS)
	rm -f scsi.a
	$(AR) rcs scsi.a $(SCSI_OBJS)
	sync

aha152x.o: aha152x.c
	$(CC) $(CFLAGS) $(AHA152X) -c aha152x.c 


seagate.o: seagate.c
	$(CC) $(CFLAGS) -DARBITRATE -DSLOW_HANDSHAKE -DFAST32 -c seagate.c 

dep:
	$(CPP) -M $(AHA152X) $(SCSI_SRCS) > .depend

else

scsi.a:
	rm -f scsi.a
	@echo No SCSI drivers configured
	$(AR) rcs scsi.a

dep:

endif


#
# include a dependency file if one exists
#
ifeq (.depend,$(wildcard .depend))
include .depend
endif
