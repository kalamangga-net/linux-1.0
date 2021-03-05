
HEAD = head.o
SYSTEM = ../tools/zSystem
#LD = gcc
#TEST = -DTEST_DRIVER

zOBJECTS = $(HEAD) inflate.o unzip.o misc.o

CFLAGS = -O2 -DSTDC_HEADERS $(TEST) -I../include/ \
         -w -m32 -std=gnu89 \
         -fno-stack-protector -fno-builtin -mmanual-endbr \
         -fno-reorder-blocks-and-partition


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
	$(CPP) -w -traditional head.S -o head.s

piggy.o:	$(SYSTEM) xtract piggyback
		objcopy -O binary -j.text -j.data -j.rodata -j.bss $(SYSTEM) a.out
		rm -f a.out.gz
		gzip -9 a.out
		$(CC) -c piggyback2.S -m32 -o piggy.o


$(SYSTEM):
		$(MAKE) -C .. tools/zSystem
