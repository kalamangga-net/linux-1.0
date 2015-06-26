/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		A saner implementation of the skbuff stuff scattered everywhere
 *		in the old NET2D code.
 *
 *	Authors:	Alan Cox <iiitac@pyr.swan.ac.uk>
 *
 *	Fixes:
 *		Alan Cox	:	Tracks memory and number of buffers for kernel memory report
 *					and memory leak hunting.
 *		Alan Cox	:	More generic kfree handler
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <asm/segment.h>
#include <asm/system.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/in.h>
#include "inet.h"
#include "dev.h"
#include "ip.h"
#include "protocol.h"
#include "arp.h"
#include "route.h"
#include "tcp.h"
#include "udp.h"
#include "skbuff.h"
#include "sock.h"


/* Socket buffer operations. Ideally much of this list swap stuff ought to be using
   exch instructions on the 386, and CAS/CAS2 on a 68K. This is the boring generic
   slow C version. No doubt when Linus sees this comment he'll do horrible things
   to this code 8-)
*/

/*
 *	Resource tracking variables
 */

volatile unsigned long net_memory=0;
volatile unsigned long net_skbcount=0;

/*
 *	Debugging paranoia. Can go later when this crud stack works
 */



void skb_check(struct sk_buff *skb, int line, char *file)
{
	if(skb->magic_debug_cookie==SK_FREED_SKB)
	{
		printk("File: %s Line %d, found a freed skb lurking in the undergrowth!\n",
			file,line);
		printk("skb=%p, real size=%ld, claimed size=%ld, magic=%d, list=%p, free=%d\n",
			skb,skb->truesize,skb->mem_len,skb->magic,skb->list,skb->free);
	}
	if(skb->magic_debug_cookie!=SK_GOOD_SKB)
	{
		printk("File: %s Line %d, passed a non skb!\n", file,line);
		printk("skb=%p, real size=%ld, claimed size=%ld, magic=%d, list=%p, free=%d\n",
			skb,skb->truesize,skb->mem_len,skb->magic,skb->list,skb->free);
	}
	if(skb->mem_len!=skb->truesize)
	{
		printk("File: %s Line %d, Dubious size setting!\n",file,line);
		printk("skb=%p, real size=%ld, claimed size=%ld, magic=%d, list=%p\n",
			skb,skb->truesize,skb->mem_len,skb->magic,skb->list);
	}
	/* Guess it might be acceptable then */
}

/*
 *	Insert an sk_buff at the start of a list.
 */

void skb_queue_head(struct sk_buff *volatile* list,struct sk_buff *newsk)
{
	unsigned long flags;

	IS_SKB(newsk);
	if(newsk->list)
		printk("Suspicious queue head: sk_buff on list!\n");
	save_flags(flags);
	cli();
	newsk->list=list;

	newsk->next=*list;

	if(*list)
		newsk->prev=(*list)->prev;
	else
		newsk->prev=newsk;
	newsk->prev->next=newsk;
	newsk->next->prev=newsk;
	IS_SKB(newsk->prev);
	IS_SKB(newsk->next);
	*list=newsk;
	restore_flags(flags);
}

/*
 *	Insert an sk_buff at the end of a list.
 */

void skb_queue_tail(struct sk_buff *volatile* list, struct sk_buff *newsk)
{
	unsigned long flags;

	if(newsk->list)
		printk("Suspicious queue tail: sk_buff on list!\n");

	IS_SKB(newsk);
	save_flags(flags);
	cli();

	newsk->list=list;
	if(*list)
	{
		(*list)->prev->next=newsk;
		newsk->prev=(*list)->prev;
		newsk->next=*list;
		(*list)->prev=newsk;
	}
	else
	{
		newsk->next=newsk;
		newsk->prev=newsk;
		*list=newsk;
	}
	IS_SKB(newsk->prev);
	IS_SKB(newsk->next);
	restore_flags(flags);

}

/*
 *	Remove an sk_buff from a list. This routine is also interrupt safe
 *	so you can grab read and free buffers as another process adds them.
 */

struct sk_buff *skb_dequeue(struct sk_buff *volatile* list)
{
	long flags;
	struct sk_buff *result;

	save_flags(flags);
	cli();

	if(*list==NULL)
	{
		restore_flags(flags);
		return(NULL);
	}

	result=*list;
	if(result->next==result)
		*list=NULL;
	else
	{
		result->next->prev=result->prev;
		result->prev->next=result->next;
		*list=result->next;
	}

	IS_SKB(result);
	restore_flags(flags);

	if(result->list!=list)
		printk("Dequeued packet has invalid list pointer\n");

	result->list=0;
	result->next=0;
	result->prev=0;
	return(result);
}

/*
 *	Insert a packet before another one in a list.
 */

void skb_insert(struct sk_buff *old, struct sk_buff *newsk)
{
	unsigned long flags;

	IS_SKB(old);
	IS_SKB(newsk);

	if(!old->list)
		printk("insert before unlisted item!\n");
	if(newsk->list)
		printk("inserted item is already on a list.\n");

	save_flags(flags);
	cli();
	newsk->list=old->list;
	newsk->next=old;
	newsk->prev=old->prev;
	newsk->next->prev=newsk;
	newsk->prev->next=newsk;

	restore_flags(flags);
}

/*
 *	Place a packet after a given packet in a list.
 */

void skb_append(struct sk_buff *old, struct sk_buff *newsk)
{
	unsigned long flags;

	IS_SKB(old);
	IS_SKB(newsk);

	if(!old->list)
		printk("append before unlisted item!\n");
	if(newsk->list)
		printk("append item is already on a list.\n");

	save_flags(flags);
	cli();
	newsk->list=old->list;
	newsk->prev=old;
	newsk->next=old->next;
	newsk->next->prev=newsk;
	newsk->prev->next=newsk;

	restore_flags(flags);
}

/*
 *	Remove an sk_buff from its list. Works even without knowing the list it
 *	is sitting on, which can be handy at times. It also means that THE LIST
 *	MUST EXIST when you unlink. Thus a list must have its contents unlinked
 *	_FIRST_.
 */

void skb_unlink(struct sk_buff *skb)
{
	unsigned long flags;
	save_flags(flags);
	cli();

	IS_SKB(skb);

	if(skb->list)
	{
		skb->next->prev=skb->prev;
		skb->prev->next=skb->next;
		if(*skb->list==skb)
		{
			if(skb->next==skb)
				*skb->list=NULL;
			else
				*skb->list=skb->next;
		}
		skb->next=0;
		skb->prev=0;
		skb->list=0;
	}
	restore_flags(flags);
}

/*
 *	An skbuff list has had its head reassigned. Move all the list
 *	pointers. Must be called with ints off during the whole head
 *	shifting
 */

void skb_new_list_head(struct sk_buff *volatile* list)
{
	struct sk_buff *skb=skb_peek(list);
	if(skb!=NULL)
	{
		do
		{
			IS_SKB(skb);
			skb->list=list;
			skb=skb->next;
		}
		while(skb!=*list);
	}
}

/*
 *	Peek an sk_buff. Unlike most other operations you _MUST_
 *	be careful with this one. A peek leaves the buffer on the
 *	list and someone else may run off with it. For an interrupt
 *	type system cli() peek the buffer copy the data and sti();
 */

struct sk_buff *skb_peek(struct sk_buff *volatile* list)
{
	return *list;
}

/*
 *	Get a clone of an sk_buff. This is the safe way to peek at
 *	a socket queue without accidents. Its a bit long but most
 *	of it acutally ends up as tiny bits of inline assembler
 *	anyway. Only the memcpy of upto 4K with ints off is not
 *	as nice as I'd like.
 */

struct sk_buff *skb_peek_copy(struct sk_buff *volatile* list)
{
	struct sk_buff *orig,*newsk;
	unsigned long flags;
	unsigned int len;
	/* Now for some games to avoid races */

	do
	{
		save_flags(flags);
		cli();
		orig=skb_peek(list);
		if(orig==NULL)
		{
			restore_flags(flags);
			return NULL;
		}
		IS_SKB(orig);
		len=orig->truesize;
		restore_flags(flags);

		newsk=alloc_skb(len,GFP_KERNEL);	/* May sleep */

		if(newsk==NULL)		/* Oh dear... not to worry */
			return NULL;

		save_flags(flags);
		cli();
		if(skb_peek(list)!=orig)	/* List changed go around another time */
		{
			restore_flags(flags);
			newsk->sk=NULL;
			newsk->free=1;
			newsk->mem_addr=newsk;
			newsk->mem_len=len;
			kfree_skb(newsk, FREE_WRITE);
			continue;
		}

		IS_SKB(orig);
		IS_SKB(newsk);
		memcpy(newsk,orig,len);
		newsk->list=NULL;
		newsk->magic=0;
		newsk->next=NULL;
		newsk->prev=NULL;
		newsk->mem_addr=newsk;
		newsk->h.raw+=((char *)newsk-(char *)orig);
		newsk->link3=NULL;
		newsk->sk=NULL;
		newsk->free=1;
	}
	while(0);

	restore_flags(flags);
	return(newsk);
}

/*
 *	Free an sk_buff. This still knows about things it should
 *	not need to like protocols and sockets.
 */

void kfree_skb(struct sk_buff *skb, int rw)
{
	if (skb == NULL) {
		printk("kfree_skb: skb = NULL\n");
		return;
	}
	IS_SKB(skb);
	if(skb->lock)
	{
		skb->free=1;	/* Free when unlocked */
		return;
	}
	
	if(skb->free == 2)
		printk("Warning: kfree_skb passed an skb that nobody set the free flag on!\n");
	if(skb->list)
		printk("Warning: kfree_skb passed an skb still on a list.\n");
	skb->magic = 0;
	if (skb->sk)
	{
		if(skb->sk->prot!=NULL)
		{
			if (rw)
				skb->sk->prot->rfree(skb->sk, skb->mem_addr, skb->mem_len);
			else
				skb->sk->prot->wfree(skb->sk, skb->mem_addr, skb->mem_len);

		}
		else
		{
			/* Non INET - default wmalloc/rmalloc handler */
			if (rw)
				skb->sk->rmem_alloc-=skb->mem_len;
			else
				skb->sk->wmem_alloc-=skb->mem_len;
			if(!skb->sk->dead)
			wake_up_interruptible(skb->sk->sleep);
			kfree_skbmem(skb->mem_addr,skb->mem_len);
		}
	}
	else
		kfree_skbmem(skb->mem_addr, skb->mem_len);
}

/*
 *	Allocate a new skbuff. We do this ourselves so we can fill in a few 'private'
 *	fields and also do memory statistics to find all the [BEEP] leaks.
 */

struct sk_buff *alloc_skb(unsigned int size,int priority)
{
	struct sk_buff *skb;
	extern unsigned long intr_count;

	if (intr_count && priority != GFP_ATOMIC) {
		printk("alloc_skb called nonatomically from interrupt %08lx\n",
			((unsigned long *)&size)[-1]);
		priority = GFP_ATOMIC;
	}
	skb=(struct sk_buff *)kmalloc(size,priority);
	if(skb==NULL)
		return NULL;
	skb->free= 2;	/* Invalid so we pick up forgetful users */
	skb->list= 0;	/* Not on a list */
	skb->lock= 0;
	skb->truesize=size;
	skb->mem_len=size;
	skb->mem_addr=skb;
	skb->fraglist=NULL;
	net_memory+=size;
	net_skbcount++;
	skb->magic_debug_cookie=SK_GOOD_SKB;
	skb->users=0;
	return skb;
}

/*
 *	Free an skbuff by memory
 */

void kfree_skbmem(void *mem,unsigned size)
{
	struct sk_buff *x=mem;
	IS_SKB(x);
	if(x->magic_debug_cookie==SK_GOOD_SKB)
	{
		x->magic_debug_cookie=SK_FREED_SKB;
		kfree_s(mem,size);
		net_skbcount--;
		net_memory-=size;
	}
}

/*
 *	Skbuff device locking
 */
 
void skb_kept_by_device(struct sk_buff *skb)
{
	skb->lock++;
}

void skb_device_release(struct sk_buff *skb, int mode)
{
	unsigned long flags;

	save_flags(flags);
	cli();
	if (!--skb->lock) {
		if (skb->free==1)
			kfree_skb(skb,mode);
	}
	restore_flags(flags);
}

int skb_device_locked(struct sk_buff *skb)
{
	if(skb->lock)
		return 1;
	return 0;
}
