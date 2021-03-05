#include <linux/autoconf.h>
/* 
 * Herein lies all the functions/variables that are "exported" for linkage
 * With dynamically loaded kernel modules. Could do with making this a bit
 * cleaner!
 *
 *  Jon.
 */

register_chrdev
unregister_chrdev
register_blkdev
unregister_blkdev
wake_up_interruptible

wp_works_ok
__verify_write

current
jiffies
printk
schedule

#ifdef CONFIG_FTAPE
#
# The next labels are needed for ftape driver.
#
ftape_big_buffer
do_floppy
#endif
