
HEAD = head.o
SYSTEM = ../tools/zSystem
#LD = gcc
#TEST = -DTEST_DRIVER

zOBJECTS = $(HEAD) inflate.o unzip.o misc.o

CFLAGS = -O2 -DSTDC_HEADERS $(TEST)

.c.s:
	$(CC) $(CFLAGS) -S -o $*.s $<
.s.o:
	$(AS) -c -o $*.o $<
.c.o:
	$(CC) $(CFLAGS) -c -o $*.o $<

all:	zSystem

zSystem:	piggy.o $(zOBJECTS)
		$(LD) $(LDFLAGS) -o zSystem -Ttext 1000 $(zOBJECTS) piggy.o

head.o:	head.s

head.s: head.S ../include/linux/tasks.h
	$(CPP) -traditional head.S -o head.s

piggy.o:	$(SYSTEM) xtract piggyback
		./xtract $(SYSTEM) | gzip -9 | ./piggyback > piggy.o

$(SYSTEM):
		$(MAKE) -C .. tools/zSystem
