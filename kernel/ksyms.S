#include <linux/autoconf.h>
/* 
 * Herein lies all the functions/variables that are "exported" for linkage
 * With dynamically loaded kernel modules. Could do with making this a bit
 * cleaner!
 *
 *  Jon.
 */

_register_chrdev
_unregister_chrdev
_register_blkdev
_unregister_blkdev
_wake_up_interruptible

_wp_works_ok
___verify_write

_current
_jiffies
_printk
_schedule

#ifdef CONFIG_FTAPE
#
# The next labels are needed for ftape driver.
#
_ftape_big_buffer
_do_floppy
#endif
