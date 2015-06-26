#ifndef _LINUX_MALLOC_H
#define _LINUX_MALLOC_H

#include <linux/config.h>

#ifdef CONFIG_DEBUG_MALLOC
#define kmalloc(a,b) deb_kmalloc(__FILE__,__LINE__,a,b)
#define kfree_s(a,b) deb_kfree_s(__FILE__,__LINE__,a,b)

void *deb_kmalloc(const char *deb_file, unsigned short deb_line,unsigned int size, int priority);
void deb_kfree_s (const char *deb_file, unsigned short deb_line,void * obj, int size);
void deb_kcheck_s(const char *deb_file, unsigned short deb_line,void * obj, int size);

#define kfree(a) deb_kfree_s(__FILE__,__LINE__, a,0)
#define kcheck(a) deb_kcheck_s(__FILE__,__LINE__, a,0)
#define kcheck_s(a,b) deb_kcheck_s(__FILE__,__LINE__, a,b)

#else /* !debug */

void * kmalloc(unsigned int size, int priority);
void kfree_s(void * obj, int size);

#define kcheck_s(a,b) 0

#define kfree(x) kfree_s((x), 0)
#define kcheck(x) kcheck_s((x), 0)

#endif

#endif /* _LINUX_MALLOC_H */
