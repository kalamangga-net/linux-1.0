/*
 * Microsoft busmouse driver based on Logitech driver (see busmouse.c)
 *
 * Microsoft BusMouse support by Teemu Rantanen (tvr@cs.hut.fi) (02AUG92)
 *
 * Microsoft Bus Mouse support modified by Derrick Cole (cole@concert.net)
 *    8/28/92
 *
 * Microsoft Bus Mouse support folded into 0.97pl4 code
 *    by Peter Cervasio (pete%q106fm.uucp@wupost.wustl.edu) (08SEP92)
 * Changes:  Logitech and Microsoft support in the same kernel.
 *           Defined new constants in busmouse.h for MS mice.
 *           Added int mse_busmouse_type to distinguish busmouse types
 *           Added a couple of new functions to handle differences in using
 *             MS vs. Logitech (where the int variable wasn't appropriate).
 *
 * Modified by Peter Cervasio (address above) (26SEP92)
 * Changes:  Included code to (properly?) detect when a Microsoft mouse is
 *           really attached to the machine.  Don't know what this does to
 *           Logitech bus mice, but all it does is read ports.
 *
 * Modified by Christoph Niemann (niemann@rubdv15.etdv.ruhr-uni-bochum.de)
 * Changes:  Better interrupt-handler (like in busmouse.c).
 *	     Some changes to reduce code-size.
 *	     Changed dectection code to use inb_p() instead of doing empty
 *	     loops to delay i/o.
 *
 * version 0.3a
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/busmouse.h>
#include <linux/signal.h>
#include <linux/errno.h>

#include <asm/io.h>
#include <asm/segment.h>
#include <asm/system.h>
#include <asm/irq.h>

static struct mouse_status mouse;

static void ms_mouse_interrupt(int unused)
{
        char dx, dy;
	unsigned char buttons;

	outb(MS_MSE_COMMAND_MODE, MS_MSE_CONTROL_PORT);
	outb((inb(MS_MSE_DATA_PORT) | 0x20), MS_MSE_DATA_PORT);

	outb(MS_MSE_READ_X, MS_MSE_CONTROL_PORT);
	dx = inb(MS_MSE_DATA_PORT);

	outb(MS_MSE_READ_Y, MS_MSE_CONTROL_PORT);
	dy = inb(MS_MSE_DATA_PORT);

	outb(MS_MSE_READ_BUTTONS, MS_MSE_CONTROL_PORT);
	buttons = ~(inb(MS_MSE_DATA_PORT)) & 0x07;

	outb(MS_MSE_COMMAND_MODE, MS_MSE_CONTROL_PORT);
	outb((inb(MS_MSE_DATA_PORT) & 0xdf), MS_MSE_DATA_PORT);

	if (dx != 0 || dy != 0 || buttons != mouse.buttons || ((~buttons) & 0x07)) {
		mouse.buttons = buttons;
		mouse.dx += dx;
		mouse.dy += dy;
		mouse.ready = 1;
		wake_up_interruptible(&mouse.wait);
	}
}

static void release_mouse(struct inode * inode, struct file * file)
{
	MS_MSE_INT_OFF();
	mouse.active = mouse.ready = 0; 
	free_irq(MOUSE_IRQ);
}

static int open_mouse(struct inode * inode, struct file * file)
{
	if (!mouse.present)
		return -EINVAL;
	if (mouse.active)
		return -EBUSY;
	mouse.active = 1;
	mouse.ready = mouse.dx = mouse.dy = 0;	
	mouse.buttons = 0x80;
	if (request_irq(MOUSE_IRQ, ms_mouse_interrupt)) {
		mouse.active = 0;
		return -EBUSY;
	}
	outb(MS_MSE_START, MS_MSE_CONTROL_PORT);
	MS_MSE_INT_ON();	
	return 0;
}


static int write_mouse(struct inode * inode, struct file * file, char * buffer, int count)
{
	return -EINVAL;
}

static int read_mouse(struct inode * inode, struct file * file, char * buffer, int count)
{
	int i, dx, dy;

	if (count < 3)
		return -EINVAL;
	if (!mouse.ready)
		return -EAGAIN;
	put_fs_byte(mouse.buttons | 0x80, buffer);
	dx = mouse.dx < -127 ? -127 : mouse.dx > 127 ?  127 :  mouse.dx;
	dy = mouse.dy < -127 ?  127 : mouse.dy > 127 ? -127 : -mouse.dy;
	put_fs_byte((char)dx, buffer + 1);
	put_fs_byte((char)dy, buffer + 2);
	for (i = 3; i < count; i++)
		put_fs_byte(0x00, buffer + i);
	mouse.dx -= dx;
	mouse.dy += dy;
	mouse.ready = 0;
	return i;	
}

static int mouse_select(struct inode *inode, struct file *file, int sel_type, select_table * wait)
{
	if (sel_type != SEL_IN)
		return 0;
	if (mouse.ready) 
		return 1;
	select_wait(&mouse.wait,wait);
	return 0;
}

struct file_operations ms_bus_mouse_fops = {
	NULL,		/* mouse_seek */
	read_mouse,
	write_mouse,
	NULL, 		/* mouse_readdir */
	mouse_select, 	/* mouse_select */
	NULL, 		/* mouse_ioctl */
	NULL,		/* mouse_mmap */
	open_mouse,
	release_mouse,
};

unsigned long ms_bus_mouse_init(unsigned long kmem_start)
{
	int mse_byte, i;

	mouse.present = mouse.active = mouse.ready = 0;
	mouse.buttons = 0x80;
	mouse.dx = mouse.dy = 0;
	mouse.wait = NULL;
	if (inb_p(MS_MSE_SIGNATURE_PORT) == 0xde) {

		mse_byte = inb_p(MS_MSE_SIGNATURE_PORT);

		for (i = 0; i < 4; i++) {
			if (inb_p(MS_MSE_SIGNATURE_PORT) == 0xde) {
				if (inb_p(MS_MSE_SIGNATURE_PORT) == mse_byte)
					mouse.present = 1;
				else
					mouse.present = 0;
			} else
				mouse.present = 0;
		}
	}
	if (mouse.present == 0) {
		return kmem_start;
	}
	MS_MSE_INT_OFF();
	printk("Microsoft BusMouse detected and installed.\n");
	return kmem_start;
}
