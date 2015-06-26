/*
 *  linux/fs/proc/net.c
 *
 *  Copyright (C) 1991, 1992 Linus Torvalds
 *
 *  gjh 3/'93 heim@peanuts.informatik.uni-tuebingen.de (Gerald J. Heim)
 *            most of this file is stolen from base.c
 *            it works, but you shouldn't use it as a guideline
 *            for new proc-fs entries. once i'll make it better.
 * fvk 3/'93  waltje@uwalt.nl.mugnet.org (Fred N. van Kempen)
 *	      cleaned up the whole thing, moved "net" specific code to
 *	      the NET kernel layer (where it belonged in the first place).
 * Michael K. Johnson (johnsonm@stolaf.edu) 3/93
 *            Added support from my previous inet.c.  Cleaned things up
 *            quite a bit, modularized the code.
 * fvk 4/'93  waltje@uwalt.nl.mugnet.org (Fred N. van Kempen)
 *	      Renamed "route_get_info()" to "rt_get_info()" for consistency.
 *
 *  proc net directory handling functions
 */
#include <linux/autoconf.h>

#include <asm/segment.h>

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>

/* forward references */
static int proc_readnet(struct inode * inode, struct file * file,
			 char * buf, int count);
static int proc_readnetdir(struct inode *, struct file *,
			   struct dirent *, int);
static int proc_lookupnet(struct inode *,const char *,int,struct inode **);

/* the get_*_info() functions are in the net code, and are configured
   in via the standard mechanism... */
extern int unix_get_info(char *);
#ifdef CONFIG_INET
extern int tcp_get_info(char *);
extern int udp_get_info(char *);
extern int raw_get_info(char *);
extern int arp_get_info(char *);
extern int dev_get_info(char *);
extern int rt_get_info(char *);
#endif /* CONFIG_INET */


static struct file_operations proc_net_operations = {
	NULL,			/* lseek - default */
	proc_readnet,		/* read - bad */
	NULL,			/* write - bad */
	proc_readnetdir,	/* readdir */
	NULL,			/* select - default */
	NULL,			/* ioctl - default */
	NULL,			/* mmap */
	NULL,			/* no special open code */
	NULL,			/* no special release code */
	NULL			/* can't fsync */
};

/*
 * proc directories can do almost nothing..
 */
struct inode_operations proc_net_inode_operations = {
	&proc_net_operations,	/* default net directory file-ops */
	NULL,			/* create */
	proc_lookupnet,		/* lookup */
	NULL,			/* link */
	NULL,			/* unlink */
	NULL,			/* symlink */
	NULL,			/* mkdir */
	NULL,			/* rmdir */
	NULL,			/* mknod */
	NULL,			/* rename */
	NULL,			/* readlink */
	NULL,			/* follow_link */
	NULL,			/* bmap */
	NULL,			/* truncate */
	NULL			/* permission */
};

static struct proc_dir_entry net_dir[] = {
	{ 1,2,".." },
	{ 8,1,"." },
	{ 128,4,"unix" }
#ifdef CONFIG_INET
	,{ 129,3,"arp" },
	{ 130,5,"route" },
	{ 131,3,"dev" },
	{ 132,3,"raw" },
	{ 133,3,"tcp" },
	{ 134,3,"udp" }
#endif	/* CONFIG_INET */
};

#define NR_NET_DIRENTRY ((sizeof (net_dir))/(sizeof (net_dir[0])))


static int proc_lookupnet(struct inode * dir,const char * name, int len,
	struct inode ** result)
{
	unsigned int ino;
	int i;

	*result = NULL;
	if (!dir)
		return -ENOENT;
	if (!S_ISDIR(dir->i_mode)) {
		iput(dir);
		return -ENOENT;
	}
	i = NR_NET_DIRENTRY;
	while (i-- > 0 && !proc_match(len,name,net_dir+i))
		/* nothing */;
	if (i < 0) {
		iput(dir);
		return -ENOENT;
	}
	ino = net_dir[i].low_ino;
	if (!(*result = iget(dir->i_sb,ino))) {
		iput(dir);
		return -ENOENT;
	}
	iput(dir);
	return 0;
}

static int proc_readnetdir(struct inode * inode, struct file * filp,
	struct dirent * dirent, int count)
{
	struct proc_dir_entry * de;
	unsigned int ino;
	int i,j;

	if (!inode || !S_ISDIR(inode->i_mode))
		return -EBADF;
	ino = inode->i_ino;
	if (((unsigned) filp->f_pos) < NR_NET_DIRENTRY) {
		de = net_dir + filp->f_pos;
		filp->f_pos++;
		i = de->namelen;
		ino = de->low_ino;
		put_fs_long(ino, &dirent->d_ino);
		put_fs_word(i,&dirent->d_reclen);
		put_fs_byte(0,i+dirent->d_name);
		j = i;
		while (i--)
			put_fs_byte(de->name[i], i+dirent->d_name);
		return j;
	}
	return 0;
}


static int proc_readnet(struct inode * inode, struct file * file,
			char * buf, int count)
{
	char * page;
	int length;
	int end;
	unsigned int ino;

	if (count < 0)
		return -EINVAL;
	if (!(page = (char*) __get_free_page(GFP_KERNEL)))
		return -ENOMEM;
	ino = inode->i_ino;
	switch (ino) {
#ifdef CONFIG_INET
		case 128:
			length = unix_get_info(page);
			break;
		case 129:
			length = arp_get_info(page);
			break;
		case 130:
			length = rt_get_info(page);
			break;
		case 131:
			length = dev_get_info(page);
			break;
		case 132:
			length = raw_get_info(page);
			break;
		case 133:
			length = tcp_get_info(page);
			break;
		case 134:
			length = udp_get_info(page);
			break;
#endif /* CONFIG_INET */
		default:
			free_page((unsigned long) page);
			return -EBADF;
	}
	if (file->f_pos >= length) {
		free_page((unsigned long) page);
		return 0;
	}
	if (count + file->f_pos > length)
		count = length - file->f_pos;
	end = count + file->f_pos;
	memcpy_tofs(buf, page + file->f_pos, count);
	free_page((unsigned long) page);
	file->f_pos = end;
	return count;

}
