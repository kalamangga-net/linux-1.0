#
#               Makefile for wm-FPU-emu
#

#DEBUG	= -DDEBUGGING
DEBUG	=
PARANOID = -DPARANOID
REENTRANT	= -DREENTRANT_FPU
CFLAGS	:= $(CFLAGS) $(PARANOID) $(DEBUG) -fno-builtin

.c.o:
	$(CC) $(CFLAGS) $(MATH_EMULATION) -c $<

.S.o:
	$(CC) -D__ASSEMBLER__ $(PARANOID) $(REENTRANT) -c $<

.s.o:
	$(CC) -c $<

OBJS =	fpu_entry.o div_small.o errors.o \
	fpu_arith.o fpu_aux.o fpu_etc.o fpu_trig.o \
	load_store.o get_address.o \
	poly_atan.o poly_l2.o poly_2xm1.o poly_sin.o poly_tan.o \
	poly_div.o poly_mul64.o polynomial.o \
	reg_add_sub.o reg_compare.o reg_constant.o reg_ld_str.o \
	reg_div.o reg_mul.o reg_norm.o \
	reg_u_add.o reg_u_div.o reg_u_mul.o reg_u_sub.o \
	reg_round.o \
	wm_shrx.o wm_sqrt.o

math.a: $(OBJS)
	rm -f math.a
	$(AR) rcs math.a $(OBJS)
	sync

dep:
	$(CPP) -M *.c > .depend
	$(CPP) -D__ASSEMBLER__ -M *.S >> .depend

proto:
	cproto -e -DMAKING_PROTO *.c >fpu_proto.h

dummy:

#
# include a dependency file if one exists
#
ifeq (.depend,$(wildcard .depend))
include .depend
endif
