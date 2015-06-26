/*
 *  linux/kernel/sys_call.S
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * sys_call.S  contains the system-call and fault low-level handling routines.
 * This also contains the timer-interrupt handler, as well as all interrupts
 * and faults that can result in a task-switch.
 *
 * NOTE: This code handles signal-recognition, which happens every time
 * after a timer-interrupt and after each system call.
 *
 * I changed all the .align's to 4 (16 byte alignment), as that's faster
 * on a 486.
 *
 * Stack layout in 'ret_from_system_call':
 * 	ptrace needs to have all regs on the stack.
 *	if the order here is changed, it needs to be 
 *	updated in fork.c:copy_process, signal.c:do_signal,
 *	ptrace.c and ptrace.h
 *
 *	 0(%esp) - %ebx
 *	 4(%esp) - %ecx
 *	 8(%esp) - %edx
 *       C(%esp) - %esi
 *	10(%esp) - %edi
 *	14(%esp) - %ebp
 *	18(%esp) - %eax
 *	1C(%esp) - %ds
 *	20(%esp) - %es
 *      24(%esp) - %fs
 *	28(%esp) - %gs
 *	2C(%esp) - orig_eax
 *	30(%esp) - %eip
 *	34(%esp) - %cs
 *	38(%esp) - %eflags
 *	3C(%esp) - %oldesp
 *	40(%esp) - %oldss
 */

#include <linux/segment.h>

EBX		= 0x00
ECX		= 0x04
EDX		= 0x08
ESI		= 0x0C
EDI		= 0x10
EBP		= 0x14
EAX		= 0x18
DS		= 0x1C
ES		= 0x20
FS		= 0x24
GS		= 0x28
ORIG_EAX	= 0x2C
EIP		= 0x30
CS		= 0x34
EFLAGS		= 0x38
OLDESP		= 0x3C
OLDSS		= 0x40

CF_MASK		= 0x00000001
IF_MASK		= 0x00000200
NT_MASK		= 0x00004000
VM_MASK		= 0x00020000

/*
 * these are offsets into the task-struct.
 */
state		=  0
counter		=  4
priority	=  8
signal		= 12
blocked		= 16
flags		= 20
errno		= 24
dbgreg6		= 52
dbgreg7		= 56

ENOSYS = 38

.globl _system_call,_lcall7
.globl _device_not_available, _coprocessor_error
.globl _divide_error,_debug,_nmi,_int3,_overflow,_bounds,_invalid_op
.globl _double_fault,_coprocessor_segment_overrun
.globl _invalid_TSS,_segment_not_present,_stack_segment
.globl _general_protection,_reserved
.globl _alignment_check,_page_fault
.globl ret_from_sys_call

#define SAVE_ALL \
	cld; \
	push %gs; \
	push %fs; \
	push %es; \
	push %ds; \
	pushl %eax; \
	pushl %ebp; \
	pushl %edi; \
	pushl %esi; \
	pushl %edx; \
	pushl %ecx; \
	pushl %ebx; \
	movl $(KERNEL_DS),%edx; \
	mov %dx,%ds; \
	mov %dx,%es; \
	movl $(USER_DS),%edx; \
	mov %dx,%fs;

#define RESTORE_ALL \
	cmpw $(KERNEL_CS),CS(%esp); \
	je 1f;   \
	movl _current,%eax; \
	movl dbgreg7(%eax),%ebx; \
	movl %ebx,%db7;	\
1:	popl %ebx; \
	popl %ecx; \
	popl %edx; \
	popl %esi; \
	popl %edi; \
	popl %ebp; \
	popl %eax; \
	pop %ds; \
	pop %es; \
	pop %fs; \
	pop %gs; \
	addl $4,%esp; \
	iret

.align 4
_lcall7:
	pushfl			# We get a different stack layout with call gates,
	pushl %eax		# which has to be cleaned up later..
	SAVE_ALL
	movl EIP(%esp),%eax	# due to call gates, this is eflags, not eip..
	movl CS(%esp),%edx	# this is eip..
	movl EFLAGS(%esp),%ecx	# and this is cs..
	movl %eax,EFLAGS(%esp)	#
	movl %edx,EIP(%esp)	# Now we move them to their "normal" places
	movl %ecx,CS(%esp)	#
	movl %esp,%eax
	pushl %eax
	call _iABI_emulate
	popl %eax
	jmp ret_from_sys_call

.align 4
handle_bottom_half:
	pushfl
	incl _intr_count
	sti
	call _do_bottom_half
	popfl
	decl _intr_count
	jmp 9f
.align 4
reschedule:
	pushl $ret_from_sys_call
	jmp _schedule
.align 4
_system_call:
	pushl %eax			# save orig_eax
	SAVE_ALL
	movl $-ENOSYS,EAX(%esp)
	cmpl _NR_syscalls,%eax
	jae ret_from_sys_call
	movl _current,%ebx
	andl $~CF_MASK,EFLAGS(%esp)	# clear carry - assume no errors
	movl $0,errno(%ebx)
	movl %db6,%edx
	movl %edx,dbgreg6(%ebx)  # save current hardware debugging status
	testb $0x20,flags(%ebx)		# PF_TRACESYS
	jne 1f
	call _sys_call_table(,%eax,4)
	movl %eax,EAX(%esp)		# save the return value
	movl errno(%ebx),%edx
	negl %edx
	je ret_from_sys_call
	movl %edx,EAX(%esp)
	orl $(CF_MASK),EFLAGS(%esp)	# set carry to indicate error
	jmp ret_from_sys_call
.align 4
1:	call _syscall_trace
	movl ORIG_EAX(%esp),%eax
	call _sys_call_table(,%eax,4)
	movl %eax,EAX(%esp)		# save the return value
	movl _current,%eax
	movl errno(%eax),%edx
	negl %edx
	je 1f
	movl %edx,EAX(%esp)
	orl $(CF_MASK),EFLAGS(%esp)	# set carry to indicate error
1:	call _syscall_trace

	.align 4,0x90
ret_from_sys_call:
	cmpl $0,_intr_count
	jne 2f
	movl _bh_mask,%eax
	andl _bh_active,%eax
	jne handle_bottom_half
9:	movl EFLAGS(%esp),%eax		# check VM86 flag: CS/SS are
	testl $(VM_MASK),%eax		# different then
	jne 1f
	cmpw $(KERNEL_CS),CS(%esp)	# was old code segment supervisor ?
	je 2f
1:	sti
	orl $(IF_MASK),%eax		# these just try to make sure
	andl $~NT_MASK,%eax		# the program doesn't do anything
	movl %eax,EFLAGS(%esp)		# stupid
	cmpl $0,_need_resched
	jne reschedule
	movl _current,%eax
	cmpl _task,%eax			# task[0] cannot have signals
	je 2f
	cmpl $0,state(%eax)		# state
	jne reschedule
	cmpl $0,counter(%eax)		# counter
	je reschedule
	movl blocked(%eax),%ecx
	movl %ecx,%ebx			# save blocked in %ebx for signal handling
	notl %ecx
	andl signal(%eax),%ecx
	jne signal_return
2:	RESTORE_ALL
.align 4
signal_return:
	movl %esp,%ecx
	pushl %ecx
	testl $(VM_MASK),EFLAGS(%ecx)
	jne v86_signal_return
	pushl %ebx
	call _do_signal
	popl %ebx
	popl %ebx
	RESTORE_ALL
.align 4
v86_signal_return:
	call _save_v86_state
	movl %eax,%esp
	pushl %eax
	pushl %ebx
	call _do_signal
	popl %ebx
	popl %ebx
	RESTORE_ALL

.align 4
_divide_error:
	pushl $0		# no error code
	pushl $_do_divide_error
.align 4,0x90
error_code:
	push %fs
	push %es
	push %ds
	pushl %eax
	pushl %ebp
	pushl %edi
	pushl %esi
	pushl %edx
	pushl %ecx
	pushl %ebx
	movl $0,%eax
	movl %eax,%db7			# disable hardware debugging...
	cld
	movl $-1, %eax
	xchgl %eax, ORIG_EAX(%esp)	# orig_eax (get the error code. )
	xorl %ebx,%ebx			# zero ebx
	mov %gs,%bx			# get the lower order bits of gs
	xchgl %ebx, GS(%esp)		# get the address and save gs.
	pushl %eax			# push the error code
	lea 4(%esp),%edx
	pushl %edx
	movl $(KERNEL_DS),%edx
	mov %dx,%ds
	mov %dx,%es
	movl $(USER_DS),%edx
	mov %dx,%fs
	pushl %eax
	movl _current,%eax
	movl %db6,%edx
	movl %edx,dbgreg6(%eax)  # save current hardware debugging status
	popl %eax
	call *%ebx
	addl $8,%esp
	jmp ret_from_sys_call

.align 4
_coprocessor_error:
	pushl $0
	pushl $_do_coprocessor_error
	jmp error_code

.align 4
_device_not_available:
	pushl $-1		# mark this as an int
	SAVE_ALL
	pushl $ret_from_sys_call
	movl %cr0,%eax
	testl $0x4,%eax			# EM (math emulation bit)
	je _math_state_restore
	pushl $0		# temporary storage for ORIG_EIP
	call _math_emulate
	addl $4,%esp
	ret

.align 4
_debug:
	pushl $0
	pushl $_do_debug
	jmp error_code

.align 4
_nmi:
	pushl $0
	pushl $_do_nmi
	jmp error_code

.align 4
_int3:
	pushl $0
	pushl $_do_int3
	jmp error_code

.align 4
_overflow:
	pushl $0
	pushl $_do_overflow
	jmp error_code

.align 4
_bounds:
	pushl $0
	pushl $_do_bounds
	jmp error_code

.align 4
_invalid_op:
	pushl $0
	pushl $_do_invalid_op
	jmp error_code

.align 4
_coprocessor_segment_overrun:
	pushl $0
	pushl $_do_coprocessor_segment_overrun
	jmp error_code

.align 4
_reserved:
	pushl $0
	pushl $_do_reserved
	jmp error_code

.align 4
_double_fault:
	pushl $_do_double_fault
	jmp error_code

.align 4
_invalid_TSS:
	pushl $_do_invalid_TSS
	jmp error_code

.align 4
_segment_not_present:
	pushl $_do_segment_not_present
	jmp error_code

.align 4
_stack_segment:
	pushl $_do_stack_segment
	jmp error_code

.align 4
_general_protection:
	pushl $_do_general_protection
	jmp error_code

.align 4
_alignment_check:
	pushl $_do_alignment_check
	jmp error_code

.align 4
_page_fault:
	pushl $_do_page_fault
	jmp error_code
