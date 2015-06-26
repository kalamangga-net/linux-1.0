/*
 *  linux/fs/hpfs/hpfs_fs.c
 *  read-only HPFS
 *  version 1.0
 *
 *  Chris Smith 1993
 *
 *  Sources & references:
 *   Duncan, _Design ... of HPFS_, MSJ 4(5)   (C) 1989 Microsoft Corp
 *   linux/fs/minix  Copyright (C) 1991, 1992, 1993  Linus Torvalds
 *   linux/fs/msdos  Written 1992, 1993 by Werner Almesberger
 *   linux/fs/isofs  Copyright (C) 1991  Eric Youngdale
 */

#include <linux/fs.h>
#include <linux/hpfs_fs.h>
#include <linux/errno.h>
#include <linux/malloc.h>
#include <linux/sched.h>
#include <linux/locks.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <asm/bitops.h>
#include <asm/segment.h>

#include "hpfs.h"

/* 
 * HPFS is a mixture of 512-byte blocks and 2048-byte blocks.  The 2k blocks
 * are used for directories and bitmaps.  For bmap to work, we must run the
 * file system with 512-byte blocks.  The 2k blocks are assembled in buffers
 * obtained from kmalloc.
 *
 * For a file's i-number we use the sector number of its fnode, coded.
 * (Directory ino's are even, file ino's are odd, and ino >> 1 is the
 * sector address of the fnode.  This is a hack to allow lookup() to
 * tell read_inode() whether it is necessary to read the fnode.)
 *
 * The map_xxx routines all read something into a buffer and return a
 * pointer somewhere in the buffer.  The caller must do the brelse.
 * The other routines are balanced.
 *
 * For details on the data structures see hpfs.h and the Duncan paper.
 *
 * Overview
 *
 * [ The names of these data structures, except fnode, are not Microsoft's
 * or IBM's.  I don't know what names they use.  The semantics described
 * here are those of this implementation, and any coincidence between it
 * and real HPFS is to be hoped for but not guaranteed by me, and
 * certainly not guaranteed by MS or IBM.  Who know nothing about this. ]
 *
 * [ Also, the following will make little sense if you haven't read the
 * Duncan paper, which is excellent. ]
 *
 * HPFS is a tree.  There are 3 kinds of nodes.  A directory is a tree
 * of dnodes, and a file's allocation info is a tree of sector runs
 * stored in fnodes and anodes.
 *
 * The top pointer is in the super block, it points to the fnode of the
 * root directory.
 *
 * The root directory -- all directories -- gives file names, dates &c,
 * and fnode addresses.  If the directory fits in one dnode, that's it,
 * otherwise the top dnode points to other dnodes, forming a tree.  A
 * dnode tree (one directory) might look like
 *
 *     ((a b c) d (e f g) h (i j) k l (m n o p))
 *
 * The subtrees appear between the files.  Each dir entry contains, along
 * with the name and fnode, a dnode pointer to the subtree that precedes it
 * (if there is one; a flag tells that).  The first entry in every directory
 * is ^A^A, the "." entry for the directory itself.  The last entry in every
 * dnode is \377, a fake entry whose only valid fields are the bit marking
 * it last and the down pointer to the subtree preceding it, if any.
 *
 * The "value" field of directory entries is an fnode address.  The fnode
 * tells where the sectors of the file are.  The fnode for a subdirectory
 * contains one pointer, to the root dnode of the subdirectory.  The fnode
 * for a data file contains, in effect, a tiny anode.  (Most of the space
 * in fnodes is for extended attributes.)
 *
 * anodes and the anode part of fnodes are trees of extents.  An extent
 * is a (length, disk address) pair, labeled with the file address being
 * mapped.  E.g.,
 *
 *     (0: 3@1000  3: 1@2000  4: 2@10)
 *
 * means the file:disk sector map (0:1000 1:1001 2:1002 3:2000 4:10 5:11).
 *
 * There is space for 8 file:len@disk triples in an fnode, or for 40 in an
 * anode.  If this is insufficient, subtrees are used, as in
 *
 *  (6: (0: 3@1000  3: 1@2000  4: 2@10)  12: (6: 3@8000  9: 1@9000  10: 2@20))
 *
 * The label on a subtree is the first address *after* that tree.  The
 * subtrees are always anodes.  The label:subtree pairs require only
 * two words each, so non-leaf subtrees have a different format; there
 * is room for 12 label:subtree pairs in an fnode, or 60 in an anode.
 *
 * Within a directory, each dnode contains a pointer up to its parent
 * dnode.  The root dnode points up to the directory's fnode.
 *
 * Each fnode contains a pointer to the directory that contains it
 * (to the fnode of the directory).  So this pointer in a directory
 * fnode is "..".
 *
 * On the disk, dnodes are all together in the center of the partition,
 * and HPFS even manages to put all the dnodes for a single directory
 * together, generally.  fnodes are out with the data.  anodes are seldom
 * seen -- in fact noncontiguous files are seldom seen.  I think this is
 * partly the open() call that lets programs specify the length of an
 * output file when they know it, and partly because HPFS.IFS really is
 * very good at resisting fragmentation. 
 */

/* notation */

#define little_ushort(x) (*(unsigned short *) &(x))
typedef void nonconst;

/* super block ops */

static void hpfs_read_inode(struct inode *);
static void hpfs_put_super(struct super_block *);
static void hpfs_statfs(struct super_block *, struct statfs *);
static int hpfs_remount_fs(struct super_block *, int *, char *);

static const struct super_operations hpfs_sops =
{
	hpfs_read_inode,		/* read_inode */
	NULL,				/* notify_change */
	NULL,				/* write_inode */
	NULL,				/* put_inode */
	hpfs_put_super,			/* put_super */
	NULL,				/* write_super */
	hpfs_statfs,			/* statfs */
	hpfs_remount_fs,		/* remount_fs */
};

/* file ops */

static int hpfs_file_read(struct inode *, struct file *, char *, int);
static secno hpfs_bmap(struct inode *, unsigned);

static const struct file_operations hpfs_file_ops =
{
	NULL,				/* lseek - default */
	hpfs_file_read,			/* read */
	NULL,				/* write */
	NULL,				/* readdir - bad */
	NULL,				/* select - default */
	NULL,				/* ioctl - default */
	generic_mmap,			/* mmap */
	NULL,				/* no special open is needed */
	NULL,				/* release */
	file_fsync,			/* fsync */
};

static const struct inode_operations hpfs_file_iops =
{
	(nonconst *) & hpfs_file_ops,	/* default file operations */
	NULL,				/* create */
	NULL,				/* lookup */
	NULL,				/* link */
	NULL,				/* unlink */
	NULL,				/* symlink */
	NULL,				/* mkdir */
	NULL,				/* rmdir */
	NULL,				/* mknod */
	NULL,				/* rename */
	NULL,				/* readlink */
	NULL,				/* follow_link */
	(int (*)(struct inode *, int))
	&hpfs_bmap,			/* bmap */
	NULL,				/* truncate */
	NULL,				/* permission */
};

/* directory ops */

static int hpfs_dir_read(struct inode *inode, struct file *filp,
			 char *buf, int count);
static int hpfs_readdir(struct inode *inode, struct file *filp,
			struct dirent *dirent, int count);
static int hpfs_lookup(struct inode *, const char *, int, struct inode **);

static const struct file_operations hpfs_dir_ops =
{
	NULL,				/* lseek - default */
	hpfs_dir_read,			/* read */
	NULL,				/* write - bad */
	hpfs_readdir,			/* readdir */
	NULL,				/* select - default */
	NULL,				/* ioctl - default */
	NULL,				/* mmap */
	NULL,				/* no special open code */
	NULL,				/* no special release code */
	file_fsync,			/* fsync */
};

static const struct inode_operations hpfs_dir_iops =
{
	(nonconst *) & hpfs_dir_ops,	/* default directory file ops */
	NULL,				/* create */
	hpfs_lookup,			/* lookup */
	NULL,				/* link */
	NULL,				/* unlink */
	NULL,				/* symlink */
	NULL,				/* mkdir */
	NULL,				/* rmdir */
	NULL,				/* mknod */
	NULL,				/* rename */
	NULL,				/* readlink */
	NULL,				/* follow_link */
	NULL,				/* bmap */
	NULL,				/* truncate */
	NULL,				/* permission */
};

/* Four 512-byte buffers and the 2k block obtained by concatenating them */

struct quad_buffer_head {
	struct buffer_head *bh[4];
	void *data;
};

/* forwards */

static int parse_opts(char *opts, uid_t *uid, gid_t *gid, umode_t *umask,
		      int *lowercase, int *conv);
static int check_warn(int not_ok,
		      const char *p1, const char *p2, const char *p3);
static int zerop(void *addr, unsigned len);
static void count_dnodes(struct inode *inode, dnode_secno dno,
			 unsigned *n_dnodes, unsigned *n_subdirs);
static unsigned count_bitmap(struct super_block *s);
static unsigned count_one_bitmap(dev_t dev, secno secno);
static secno bplus_lookup(struct inode *inode, struct bplus_header *b,
			  secno file_secno, struct buffer_head **bhp);
static struct hpfs_dirent *map_dirent(struct inode *inode, dnode_secno dno,
				    const unsigned char *name, unsigned len,
				      struct quad_buffer_head *qbh);
static struct hpfs_dirent *map_pos_dirent(struct inode *inode, off_t *posp,
					  struct quad_buffer_head *qbh);
static void write_one_dirent(struct dirent *dirent, const unsigned char *name,
			     unsigned namelen, ino_t ino, int lowercase);
static dnode_secno dir_subdno(struct inode *inode, unsigned pos);
static struct hpfs_dirent *map_nth_dirent(dev_t dev, dnode_secno dno,
					  int n,
					  struct quad_buffer_head *qbh);
static unsigned choose_conv(unsigned char *p, unsigned len);
static unsigned convcpy_tofs(unsigned char *out, unsigned char *in,
			     unsigned len);
static dnode_secno fnode_dno(dev_t dev, ino_t ino);
static struct fnode *map_fnode(dev_t dev, ino_t ino,
			       struct buffer_head **bhp);
static struct anode *map_anode(dev_t dev, unsigned secno,
			       struct buffer_head **bhp);
static struct dnode *map_dnode(dev_t dev, unsigned secno,
			       struct quad_buffer_head *qbh);
static void *map_sector(dev_t dev, unsigned secno, struct buffer_head **bhp);
static void *map_4sectors(dev_t dev, unsigned secno,
			  struct quad_buffer_head *qbh);
static void brelse4(struct quad_buffer_head *qbh);

/*
 * make inode number for a file
 */

static inline ino_t file_ino(fnode_secno secno)
{
	return secno << 1 | 1;
}

/*
 * make inode number for a directory
 */

static inline ino_t dir_ino(fnode_secno secno)
{
	return secno << 1;
}

/*
 * get fnode address from an inode number
 */

static inline fnode_secno ino_secno(ino_t ino)
{
	return ino >> 1;
}

/*
 * test for directory's inode number 
 */

static inline int ino_is_dir(ino_t ino)
{
	return (ino & 1) == 0;
}

/*
 * conv= options
 */

#define CONV_BINARY 0			/* no conversion */
#define CONV_TEXT 1			/* crlf->newline */
#define CONV_AUTO 2			/* decide based on file contents */

/*
 * local time (HPFS) to GMT (Unix)
 */

static inline time_t local_to_gmt(time_t t)
{
	extern struct timezone sys_tz;
	return t + sys_tz.tz_minuteswest * 60;
}

/* super block ops */

/*
 * mount.  This gets one thing, the root directory inode.  It does a
 * bunch of guessed-at consistency checks.
 */

struct super_block *hpfs_read_super(struct super_block *s,
				    void *options, int silent)
{
	struct hpfs_boot_block *bootblock;
	struct hpfs_super_block *superblock;
	struct hpfs_spare_block *spareblock;
	struct hpfs_dirent *de;
	struct buffer_head *bh0, *bh1, *bh2;
	struct quad_buffer_head qbh;
	dnode_secno root_dno;
	dev_t dev;
	uid_t uid;
	gid_t gid;
	umode_t umask;
	int lowercase;
	int conv;
	int dubious;

	/*
	 * Get the mount options
	 */

	if (!parse_opts(options, &uid, &gid, &umask, &lowercase, &conv)) {
		printk("HPFS: syntax error in mount options.  Not mounted.\n");
		s->s_dev = 0;
		return 0;
	}

	/*
	 * Fill in the super block struct
	 */

	lock_super(s);
	dev = s->s_dev;
	set_blocksize(dev, 512);

	/*
	 * fetch sectors 0, 16, 17
	 */

	bootblock = map_sector(dev, 0, &bh0);
	if (!bootblock)
		goto bail;

	superblock = map_sector(dev, 16, &bh1);
	if (!superblock)
		goto bail0;

	spareblock = map_sector(dev, 17, &bh2);
	if (!spareblock)
		goto bail1;

	/*
	 * Check that this fs looks enough like a known one that we can find
	 * and read the root directory.
	 */

	if (bootblock->magic != 0xaa55
	    || superblock->magic != SB_MAGIC
	    || spareblock->magic != SP_MAGIC
	    || bootblock->sig_28h != 0x28
	    || memcmp(&bootblock->sig_hpfs, "HPFS    ", 8)
	    || little_ushort(bootblock->bytes_per_sector) != 512) {
		printk("HPFS: hpfs_read_super: Not HPFS\n");
		goto bail2;
	}

	/*
	 * Check for inconsistencies -- possibly wrong guesses here, possibly
	 * filesystem problems.
	 */

	dubious = 0;

	dubious |= check_warn(spareblock->dirty != 0,
		       "`Improperly stopped'", "flag is set", "run CHKDSK");
	dubious |= check_warn(spareblock->n_spares_used != 0,
			      "Spare blocks", "may be in use", "run CHKDSK");

	/*
	 * Above errors mean we could get wrong answers if we proceed,
	 * so don't
	 */

	if (dubious)
		goto bail2;

	dubious |= check_warn((spareblock->n_dnode_spares !=
			       spareblock->n_dnode_spares_free),
			      "Spare dnodes", "may be in use", "run CHKDSK");
	dubious |= check_warn(superblock->zero1 != 0,
			      "#1", "unknown word nonzero", "investigate");
	dubious |= check_warn(superblock->zero3 != 0,
			      "#3", "unknown word nonzero", "investigate");
	dubious |= check_warn(superblock->zero4 != 0,
			      "#4", "unknown word nonzero", "investigate");
	dubious |= check_warn(!zerop(superblock->zero5,
				     sizeof superblock->zero5),
			      "#5", "unknown word nonzero", "investigate");
	dubious |= check_warn(!zerop(superblock->zero6,
				     sizeof superblock->zero6),
			      "#6", "unknown word nonzero", "investigate");

	if (dubious)
		printk("HPFS: Proceeding, but operation may be unreliable\n");

	/*
	 * set fs read only
	 */

	s->s_flags |= MS_RDONLY;

	/*
	 * fill in standard stuff
	 */

	s->s_magic = HPFS_SUPER_MAGIC;
	s->s_blocksize = 512;
	s->s_blocksize_bits = 9;
	s->s_op = (struct super_operations *) &hpfs_sops;

	/*
	 * fill in hpfs stuff
	 */

	s->s_hpfs_root = dir_ino(superblock->root);
	s->s_hpfs_fs_size = superblock->n_sectors;
	s->s_hpfs_dirband_size = superblock->n_dir_band / 4;
	s->s_hpfs_dmap = superblock->dir_band_bitmap;
	s->s_hpfs_bitmaps = superblock->bitmaps;
	s->s_hpfs_uid = uid;
	s->s_hpfs_gid = gid;
	s->s_hpfs_mode = 0777 & ~umask;
	s->s_hpfs_n_free = -1;
	s->s_hpfs_n_free_dnodes = -1;
	s->s_hpfs_lowercase = lowercase;
	s->s_hpfs_conv = conv;

	/*
	 * done with the low blocks
	 */

	brelse(bh2);
	brelse(bh1);
	brelse(bh0);

	/*
	 * all set.  try it out.
	 */

	s->s_mounted = iget(s, s->s_hpfs_root);
	unlock_super(s);

	if (!s->s_mounted) {
		printk("HPFS: hpfs_read_super: inode get failed\n");
		s->s_dev = 0;
		return 0;
	}

	/*
	 * find the root directory's . pointer & finish filling in the inode
	 */

	root_dno = fnode_dno(dev, s->s_hpfs_root);
	if (root_dno)
		de = map_dirent(s->s_mounted, root_dno, "\001\001", 2, &qbh);
	if (!root_dno || !de) {
		printk("HPFS: "
		       "hpfs_read_super: root dir isn't in the root dir\n");
		s->s_dev = 0;
		return 0;
	}

	s->s_mounted->i_atime = local_to_gmt(de->read_date);
	s->s_mounted->i_mtime = local_to_gmt(de->write_date);
	s->s_mounted->i_ctime = local_to_gmt(de->creation_date);

	brelse4(&qbh);
	return s;

 bail2:
	brelse(bh2);
 bail1:
	brelse(bh1);
 bail0:
	brelse(bh0);
 bail:
	s->s_dev = 0;
	unlock_super(s);
	return 0;
}

static int check_warn(int not_ok,
		      const char *p1, const char *p2, const char *p3)
{
	if (not_ok)
		printk("HPFS: %s %s. Please %s\n", p1, p2, p3);
	return not_ok;
}

static int zerop(void *addr, unsigned len)
{
	unsigned char *p = addr;
	return p[0] == 0 && memcmp(p, p + 1, len - 1) == 0;
}

/*
 * A tiny parser for option strings, stolen from dosfs.
 */

static int parse_opts(char *opts, uid_t *uid, gid_t *gid, umode_t *umask,
		      int *lowercase, int *conv)
{
	char *p, *rhs;

	*uid = current->uid;
	*gid = current->gid;
	*umask = current->umask;
	*lowercase = 1;
	*conv = CONV_BINARY;

	if (!opts)
		return 1;

	for (p = strtok(opts, ","); p != 0; p = strtok(0, ",")) {
		if ((rhs = strchr(p, '=')) != 0)
			*rhs++ = '\0';
		if (!strcmp(p, "uid")) {
			if (!rhs || !*rhs)
				return 0;
			*uid = simple_strtoul(rhs, &rhs, 0);
			if (*rhs)
				return 0;
		}
		else if (!strcmp(p, "gid")) {
			if (!rhs || !*rhs)
				return 0;
			*gid = simple_strtoul(rhs, &rhs, 0);
			if (*rhs)
				return 0;
		}
		else if (!strcmp(p, "umask")) {
			if (!rhs || !*rhs)
				return 0;
			*umask = simple_strtoul(rhs, &rhs, 8);
			if (*rhs)
				return 0;
		}
		else if (!strcmp(p, "case")) {
			if (!strcmp(rhs, "lower"))
				*lowercase = 1;
			else if (!strcmp(rhs, "asis"))
				*lowercase = 0;
			else
				return 0;
		}
		else if (!strcmp(p, "conv")) {
			if (!strcmp(rhs, "binary"))
				*conv = CONV_BINARY;
			else if (!strcmp(rhs, "text"))
				*conv = CONV_TEXT;
			else if (!strcmp(rhs, "auto"))
				*conv = CONV_AUTO;
			else
				return 0;
		}
		else
			return 0;
	}

	return 1;
}

/*
 * read_inode.  This is called with exclusive access to a new inode that
 * has only (i_dev,i_ino) set.  It is responsible for filling in the rest.
 * We leave the dates blank, to be filled in from the dir entry.
 *
 * NOTE that there must be no sleeping from the return in this routine
 * until lookup() finishes filling in the inode, otherwise the partly
 * completed inode would be visible during the sleep.
 *
 * It is done in this strange and sinful way because the alternative
 * is to read the fnode, find the dir pointer in it, read that fnode
 * to get the dnode pointer, search through that whole directory for
 * the ino we're reading, and get the dates.  It works that way, but
 * ls sounds like fsck.
 */

static void hpfs_read_inode(struct inode *inode)
{
	struct super_block *s = inode->i_sb;

	/* be ready to bail out */

	inode->i_op = 0;
	inode->i_mode = 0;

	if (inode->i_ino == 0
	    || ino_secno(inode->i_ino) >= inode->i_sb->s_hpfs_fs_size) {
		printk("HPFS: read_inode: bad ino\n");
		return;
	}

	/*
	 * canned stuff
	 */

	inode->i_uid = s->s_hpfs_uid;
	inode->i_gid = s->s_hpfs_gid;
	inode->i_mode = s->s_hpfs_mode;
	inode->i_hpfs_conv = s->s_hpfs_conv;

	inode->i_hpfs_dno = 0;
	inode->i_hpfs_n_secs = 0;
	inode->i_hpfs_file_sec = 0;
	inode->i_hpfs_disk_sec = 0;
	inode->i_hpfs_dpos = 0;
	inode->i_hpfs_dsubdno = 0;

	/*
	 * figure out whether we are looking at a directory or a file
	 */

	if (ino_is_dir(inode->i_ino))
		inode->i_mode |= S_IFDIR;
	else {
		inode->i_mode |= S_IFREG;
		inode->i_mode &= ~0111;
	}

	/*
	 * these fields must be filled in from the dir entry, which we don't
	 * have but lookup does.  It will fill them in before letting the
	 * inode out of its grasp.
	 */

	inode->i_atime = 0;
	inode->i_mtime = 0;
	inode->i_ctime = 0;
	inode->i_size = 0;

	/*
	 * fill in the rest
	 */

	if (S_ISREG(inode->i_mode)) {

		inode->i_op = (struct inode_operations *) &hpfs_file_iops;
		inode->i_nlink = 1;
		inode->i_blksize = 512;

	}
	else {
		unsigned n_dnodes, n_subdirs;
		struct buffer_head *bh0;
		struct fnode *fnode = map_fnode(inode->i_dev,
						inode->i_ino, &bh0);

		if (!fnode) {
			printk("HPFS: read_inode: no fnode\n");
			inode->i_mode = 0;
			return;
		}

		inode->i_hpfs_parent_dir = dir_ino(fnode->up);
		inode->i_hpfs_dno = fnode->u.external[0].disk_secno;

		brelse(bh0);

		n_dnodes = n_subdirs = 0;
		count_dnodes(inode, inode->i_hpfs_dno, &n_dnodes, &n_subdirs);

		inode->i_op = (struct inode_operations *) &hpfs_dir_iops;
		inode->i_blksize = 512;	/* 2048 here confuses ls & du & ... */
		inode->i_blocks = 4 * n_dnodes;
		inode->i_size = 512 * inode->i_blocks;
		inode->i_nlink = 2 + n_subdirs;
	}
}

/*
 * unmount.
 */

static void hpfs_put_super(struct super_block *s)
{
	lock_super(s);
	s->s_dev = 0;
	unlock_super(s);
}

/*
 * statfs.  For free inode counts we report the count of dnodes in the
 * directory band -- not exactly right but pretty analagous.
 */

static void hpfs_statfs(struct super_block *s, struct statfs *buf)
{
	/*
	 * count the bits in the bitmaps, unless we already have
	 */

	if (s->s_hpfs_n_free == -1) {
		s->s_hpfs_n_free = count_bitmap(s);
		s->s_hpfs_n_free_dnodes =
		    count_one_bitmap(s->s_dev, s->s_hpfs_dmap);
	}

	/*
	 * fill in the user statfs struct
	 */

	put_fs_long(s->s_magic, &buf->f_type);
	put_fs_long(512, &buf->f_bsize);
	put_fs_long(s->s_hpfs_fs_size, &buf->f_blocks);
	put_fs_long(s->s_hpfs_n_free, &buf->f_bfree);
	put_fs_long(s->s_hpfs_n_free, &buf->f_bavail);
	put_fs_long(s->s_hpfs_dirband_size, &buf->f_files);
	put_fs_long(s->s_hpfs_n_free_dnodes, &buf->f_ffree);
	put_fs_long(254, &buf->f_namelen);
}

/*
 * remount.  Don't let read only be turned off.
 */

static int hpfs_remount_fs(struct super_block *s, int *flags, char *data)
{
	if (!(*flags & MS_RDONLY))
		return -EINVAL;
	return 0;
}

/*
 * count the dnodes in a directory, and the subdirs.
 */

static void count_dnodes(struct inode *inode, dnode_secno dno,
			 unsigned *n_dnodes, unsigned *n_subdirs)
{
	struct quad_buffer_head qbh;
	struct dnode *dnode;
	struct hpfs_dirent *de;
	struct hpfs_dirent *de_end;

	dnode = map_dnode(inode->i_dev, dno, &qbh);
	if (!dnode)
		return;
	de = dnode_first_de(dnode);
	de_end = dnode_end_de(dnode);

	(*n_dnodes)++;

	for (; de < de_end; de = de_next_de(de)) {
		if (de->down)
			count_dnodes(inode, de_down_pointer(de),
				     n_dnodes, n_subdirs);
		if (de->directory && !de->first)
			(*n_subdirs)++;
		if (de->last || de->length == 0)
			break;
	}

	brelse4(&qbh);
}

/*
 * count the bits in the free space bit maps
 */

static unsigned count_bitmap(struct super_block *s)
{
	unsigned n, count, n_bands;
	secno *bitmaps;
	struct quad_buffer_head qbh;

	/*
	 * there is one bit map for each 16384 sectors
	 */
	n_bands = (s->s_hpfs_fs_size + 0x3fff) >> 14;

	/*
	 * their locations are given in an array pointed to by the super
	 * block
	 */
	bitmaps = map_4sectors(s->s_dev, s->s_hpfs_bitmaps, &qbh);
	if (!bitmaps)
		return 0;

	count = 0;

	/*
	 * map each one and count the free sectors
	 */
	for (n = 0; n < n_bands; n++)
		if (bitmaps[n] == 0)
			printk("HPFS: bit map pointer missing\n");
		else
			count += count_one_bitmap(s->s_dev, bitmaps[n]);

	brelse4(&qbh);
	return count;
}

/*
 * Read in one bit map, count the bits, return the count.
 */

static unsigned count_one_bitmap(dev_t dev, secno secno)
{
	struct quad_buffer_head qbh;
	char *bits;
	unsigned i, count;

	bits = map_4sectors(dev, secno, &qbh);
	if (!bits)
		return 0;

	count = 0;

	for (i = 0; i < 8 * 2048; i++)
		count += (test_bit(i, bits) != 0);
	brelse4(&qbh);

	return count;
}

/* file ops */

/*
 * read.  Read the bytes, put them in buf, return the count.
 */

static int hpfs_file_read(struct inode *inode, struct file *filp,
			  char *buf, int count)
{
	unsigned q, r, n, n0;
	struct buffer_head *bh;
	char *block;
	char *start;

	if (inode == 0 || !S_ISREG(inode->i_mode))
		return -EINVAL;

	/*
	 * truncate count at EOF
	 */
	if (count > inode->i_size - filp->f_pos)
		count = inode->i_size - filp->f_pos;

	start = buf;
	while (count > 0) {
		/*
		 * get file sector number, offset in sector, length to end of
		 * sector
		 */
		q = filp->f_pos >> 9;
		r = filp->f_pos & 511;
		n = 512 - r;

		/*
		 * get length to copy to user buffer
		 */
		if (n > count)
			n = count;

		/*
		 * read the sector, copy to user
		 */
		block = map_sector(inode->i_dev, hpfs_bmap(inode, q), &bh);
		if (!block)
			return -EIO;

		/*
		 * but first decide if it has \r\n, if the mount option said
		 * to do that
		 */
		if (inode->i_hpfs_conv == CONV_AUTO)
			inode->i_hpfs_conv = choose_conv(block + r, n);

		if (inode->i_hpfs_conv == CONV_BINARY) {
			/*
			 * regular copy, output length is same as input
			 * length
			 */
			memcpy_tofs(buf, block + r, n);
			n0 = n;
		}
		else {
			/*
			 * squeeze out \r, output length varies
			 */
			n0 = convcpy_tofs(buf, block + r, n);
			if (count > inode->i_size - filp->f_pos - n + n0)
				count = inode->i_size - filp->f_pos - n + n0;
		}

		brelse(bh);

		/*
		 * advance input n bytes, output n0 bytes
		 */
		filp->f_pos += n;
		buf += n0;
		count -= n0;
	}

	return buf - start;
}

/*
 * This routine implements conv=auto.  Return CONV_BINARY or CONV_TEXT.
 */

static unsigned choose_conv(unsigned char *p, unsigned len)
{
	unsigned tvote, bvote;
	unsigned c;

	tvote = bvote = 0;

	while (len--) {
		c = *p++;
		if (c < ' ')
			if (c == '\r' && len && *p == '\n')
				tvote += 10;
			else if (c == '\t' || c == '\n');
			else
				bvote += 5;
		else if (c < '\177')
			tvote++;
		else
			bvote += 5;
	}

	if (tvote > bvote)
		return CONV_TEXT;
	else
		return CONV_BINARY;
}

/*
 * This routine implements conv=text.  :s/crlf/nl/
 */

static unsigned convcpy_tofs(unsigned char *out, unsigned char *in,
			     unsigned len)
{
	unsigned char *start = out;

	while (len--) {
		unsigned c = *in++;
		if (c == '\r' && (len == 0 || *in == '\n'));
		else
			put_fs_byte(c, out++);
	}

	return out - start;
}

/*
 * Return the disk sector number containing a file sector.
 */

static secno hpfs_bmap(struct inode *inode, unsigned file_secno)
{
	unsigned n, disk_secno;
	struct fnode *fnode;
	struct buffer_head *bh;

	/*
	 * There is one sector run cached in the inode. See if the sector is
	 * in it.
	 */

	n = file_secno - inode->i_hpfs_file_sec;
	if (n < inode->i_hpfs_n_secs)
		return inode->i_hpfs_disk_sec + n;

	/*
	 * No, read the fnode and go find the sector.
	 */

	else {
		fnode = map_fnode(inode->i_dev, inode->i_ino, &bh);
		if (!fnode)
			return 0;
		disk_secno = bplus_lookup(inode, &fnode->btree,
					  file_secno, &bh);
		brelse(bh);
		return disk_secno;
	}
}

/*
 * Search allocation tree *b for the given file sector number and return
 * the disk sector number.  Buffer *bhp has the tree in it, and can be
 * reused for subtrees when access to *b is no longer needed.
 * *bhp is busy on entry and exit. 
 */

static secno bplus_lookup(struct inode *inode, struct bplus_header *b,
			  secno file_secno, struct buffer_head **bhp)
{
	int i;

	/*
	 * A leaf-level tree gives a list of sector runs.  Find the one
	 * containing the file sector we want, cache the map info in the
	 * inode for later, and return the corresponding disk sector.
	 */

	if (!b->internal) {
		struct bplus_leaf_node *n = b->u.external;
		for (i = 0; i < b->n_used_nodes; i++) {
			unsigned t = file_secno - n[i].file_secno;
			if (t < n[i].length) {
				inode->i_hpfs_file_sec = n[i].file_secno;
				inode->i_hpfs_disk_sec = n[i].disk_secno;
				inode->i_hpfs_n_secs = n[i].length;
				return n[i].disk_secno + t;
			}
		}
	}

	/*
	 * A non-leaf tree gives a list of subtrees.  Find the one containing
	 * the file sector we want, read it in, and recurse to search it.
	 */

	else {
		struct bplus_internal_node *n = b->u.internal;
		for (i = 0; i < b->n_used_nodes; i++) {
			if (file_secno < n[i].file_secno) {
				struct anode *anode;
				anode_secno ano = n[i].down;
				brelse(*bhp);
				anode = map_anode(inode->i_dev, ano, bhp);
				if (!anode)
					break;
				return bplus_lookup(inode, &anode->btree,
						    file_secno, bhp);
			}
		}
	}

	/*
	 * If we get here there was a hole in the file.  As far as I know we
	 * never do get here, but falling off the end would be indelicate. So
	 * return a pointer to a handy all-zero sector.  This is not a
	 * reasonable way to handle files with holes if they really do
	 * happen.
	 */

	printk("HPFS: bplus_lookup: sector not found\n");
	return 15;
}

/* directory ops */

/*
 * lookup.  Search the specified directory for the specified name, set
 * *result to the corresponding inode.
 *
 * lookup uses the inode number to tell read_inode whether it is reading
 * the inode of a directory or a file -- file ino's are odd, directory
 * ino's are even.  read_inode avoids i/o for file inodes; everything
 * needed is up here in the directory.  (And file fnodes are out in
 * the boondocks.)
 */

static int hpfs_lookup(struct inode *dir, const char *name, int len,
		       struct inode **result)
{
	struct quad_buffer_head qbh;
	struct hpfs_dirent *de;
	struct inode *inode;
	ino_t ino;

	/* In case of madness */

	*result = 0;
	if (dir == 0)
		return -ENOENT;
	if (!S_ISDIR(dir->i_mode))
		goto bail;

	/*
	 * Read in the directory entry. "." is there under the name ^A^A .
	 * Always read the dir even for . and .. in case we need the dates.
	 */

	if (name[0] == '.' && len == 1)
		de = map_dirent(dir, dir->i_hpfs_dno, "\001\001", 2, &qbh);
	else if (name[0] == '.' && name[1] == '.' && len == 2)
		de = map_dirent(dir,
				fnode_dno(dir->i_dev, dir->i_hpfs_parent_dir),
				"\001\001", 2, &qbh);
	else
		de = map_dirent(dir, dir->i_hpfs_dno, name, len, &qbh);

	/*
	 * This is not really a bailout, just means file not found.
	 */

	if (!de)
		goto bail;

	/*
	 * Get inode number, what we're after.
	 */

	if (de->directory)
		ino = dir_ino(de->fnode);
	else
		ino = file_ino(de->fnode);

	/*
	 * Go find or make an inode.
	 */

	if (!(inode = iget(dir->i_sb, ino)))
		goto bail1;

	/*
	 * Fill in the info from the directory if this is a newly created
	 * inode.
	 */

	if (!inode->i_atime) {
		inode->i_atime = local_to_gmt(de->read_date);
		inode->i_mtime = local_to_gmt(de->write_date);
		inode->i_ctime = local_to_gmt(de->creation_date);
		if (de->read_only)
			inode->i_mode &= ~0222;
		if (!de->directory) {
			inode->i_size = de->file_size;
			/*
			 * i_blocks should count the fnode and any anodes.
			 * We count 1 for the fnode and don't bother about
			 * anodes -- the disk heads are on the directory band
			 * and we want them to stay there.
			 */
			inode->i_blocks = 1 + ((inode->i_size + 511) >> 9);
		}
	}

	brelse4(&qbh);

	/*
	 * Made it.
	 */

	*result = inode;
	iput(dir);
	return 0;

	/*
	 * Didn't.
	 */
 bail1:
	brelse4(&qbh);
 bail:
	iput(dir);
	return -ENOENT;
}

/*
 * Compare two counted strings ignoring case.
 * HPFS directory order sorts letters as if they're upper case.
 */

static inline int memcasecmp(const unsigned char *s1, const unsigned char *s2,
			     unsigned n)
{
	int t;

	if (n != 0)
		do {
			unsigned c1 = *s1++;
			unsigned c2 = *s2++;
			if (c1 - 'a' < 26)
				c1 -= 040;
			if (c2 - 'a' < 26)
				c2 -= 040;
			if ((t = c1 - c2) != 0)
				return t;
		} while (--n != 0);

	return 0;
}

/*
 * Search a directory for the given name, return a pointer to its dir entry
 * and a pointer to the buffer containing it.
 */

static struct hpfs_dirent *map_dirent(struct inode *inode, dnode_secno dno,
				      const unsigned char *name, unsigned len,
				      struct quad_buffer_head *qbh)
{
	struct dnode *dnode;
	struct hpfs_dirent *de;
	struct hpfs_dirent *de_end;
	int t, l;

	/*
	 * read the dnode at the root of our subtree
	 */
	dnode = map_dnode(inode->i_dev, dno, qbh);
	if (!dnode)
		return 0;

	/*
	 * get pointers to start and end+1 of dir entries
	 */
	de = dnode_first_de(dnode);
	de_end = dnode_end_de(dnode);

	/*
	 * look through the entries for the name we're after
	 */
	for ( ; de < de_end; de = de_next_de(de)) {

		/*
		 * compare names
		 */
		l = len < de->namelen ? len : de->namelen;
		t = memcasecmp(name, de->name, l);

		/*
		 * initial substring matches, compare lengths
		 */
		if (t == 0) {
			t = len - de->namelen;
			/* bingo */
			if (t == 0)
				return de;
		}

		/*
		 * wanted name .lt. dir name => not present.
		 */
		if (t < 0) {
			/*
			 * if there is a subtree, search it.
			 */
			if (de->down) {
				dnode_secno sub_dno = de_down_pointer(de);
				brelse4(qbh);
				return map_dirent(inode, sub_dno,
						  name, len, qbh);
			}
			else
				break;
		}

		/*
		 * de->last is set on the last name in the dnode (it's always
		 * a "\377" pseudo entry).  de->length == 0 means we're about
		 * to infinite loop. This test does nothing in a well-formed
		 * dnode.
		 */
		if (de->last || de->length == 0)
			break;
	}

	/*
	 * name not found.
	 */

	return 0;
}

/*
 * readdir.  Return exactly 1 dirent.  (I tried and tried, but currently
 * the interface with libc just does not permit more than 1.  If it gets
 * fixed, throw this out and just walk the tree and write records into
 * the user buffer.)
 *
 * We keep track of our position in the dnode tree with a sort of
 * dewey-decimal record of subtree locations.  Like so:
 *
 *   (1 (1.1 1.2 1.3) 2 3 (3.1 (3.1.1 3.1.2) 3.2 3.3 (3.3.1)) 4)
 *
 * Subtrees appear after their file, out of lexical order,
 * which would be before their file.  It's easier.
 *
 * A directory can't hold more than 56 files, so 6 bits are used for
 * position numbers.  If the tree is so deep that the position encoding
 * doesn't fit, I'm sure something absolutely fascinating happens.
 *
 * The actual sequence of f_pos values is
 *     0 => .   -1 => ..   1 1.1 ... 8.9 9 => files  -2 => eof
 *
 * The directory inode caches one position-to-dnode correspondence so
 * we won't have to repeatedly scan the top levels of the tree. 
 */

static int hpfs_readdir(struct inode *inode, struct file *filp,
			struct dirent *dirent, int likely_story)
{
	struct quad_buffer_head qbh;
	struct hpfs_dirent *de;
	int namelen, lc;
	ino_t ino;

	if (inode == 0
	    || inode->i_sb == 0
	    || !S_ISDIR(inode->i_mode))
		return -EBADF;

	lc = inode->i_sb->s_hpfs_lowercase;

	switch (filp->f_pos) {
	case 0:
		write_one_dirent(dirent, ".", 1, inode->i_ino, lc);
		filp->f_pos = -1;
		return 1;

	case -1:
		write_one_dirent(dirent, "..", 2,
				 inode->i_hpfs_parent_dir, lc);
		filp->f_pos = 1;
		return 2;

	case -2:
		return 0;

	default:
		de = map_pos_dirent(inode, &filp->f_pos, &qbh);
		if (!de) {
			filp->f_pos = -2;
			return 0;
		}

		namelen = de->namelen;
		if (de->directory)
			ino = dir_ino(de->fnode);
		else
			ino = file_ino(de->fnode);
		write_one_dirent(dirent, de->name, namelen, ino, lc);
		brelse4(&qbh);

		return namelen;
	}
}

/*
 * Send the given name and ino off to the user dirent struct at *dirent.
 * Blam it to lowercase if the mount option said to.
 *
 * Note that Linux d_reclen is the length of the file name, and has nothing
 * to do with the length of the dirent record.
 */

static void write_one_dirent(struct dirent *dirent, const unsigned char *name,
			     unsigned namelen, ino_t ino, int lowercase)
{
	unsigned n;

	put_fs_long(ino, &dirent->d_ino);
	put_fs_word(namelen, &dirent->d_reclen);

	if (lowercase)
		for (n = namelen; n != 0;) {
			unsigned t = name[--n];
			if (t - 'A' < 26)
				t += 040;
			put_fs_byte(t, &dirent->d_name[n]);
		}
	else
		memcpy_tofs(dirent->d_name, name, namelen);

	put_fs_byte(0, &dirent->d_name[namelen]);
}

/*
 * Map the dir entry at subtree coordinates given by *posp, and
 * increment *posp to point to the following dir entry. 
 */

static struct hpfs_dirent *map_pos_dirent(struct inode *inode, off_t *posp,
					  struct quad_buffer_head *qbh)
{
	unsigned pos, q, r;
	dnode_secno dno;
	struct hpfs_dirent *de;

	/*
	 * Get the position code and split off the rightmost index r
	 */

	pos = *posp;
	q = pos >> 6;
	r = pos & 077;

	/*
	 * Get the sector address of the dnode
	 * pointed to by the leading part q
	 */

	dno = dir_subdno(inode, q);
	if (!dno)
		return 0;

	/*
	 * Get the entry at index r in dnode q
	 */

	de = map_nth_dirent(inode->i_dev, dno, r, qbh);

	/*
	 * If none, we're out of files in this dnode.  Ascend.
	 */

	if (!de) {
		if (q == 0)
			return 0;
		*posp = q + 1;
		return map_pos_dirent(inode, posp, qbh);
	}

	/*
	 * If a subtree is here, descend.
	 */

	if (de->down)
		*posp = pos << 6 | 1;
	else
		*posp = pos + 1;

	/*
	 * Don't return the ^A^A and \377 entries.
	 */

	if (de->first || de->last) {
		brelse4(qbh);
		return map_pos_dirent(inode, posp, qbh);
	}
	else
		return de;
}

/*
 * Return the address of the dnode with subtree coordinates given by pos.
 */

static dnode_secno dir_subdno(struct inode *inode, unsigned pos)
{
	struct hpfs_dirent *de;
	struct quad_buffer_head qbh;

	/*
	 * 0 is the root dnode
	 */

	if (pos == 0)
		return inode->i_hpfs_dno;

	/*
	 * we have one pos->dnode translation cached in the inode
	 */

	else if (pos == inode->i_hpfs_dpos)
		return inode->i_hpfs_dsubdno;

	/*
	 * otherwise go look
	 */

	else {
		unsigned q = pos >> 6;
		unsigned r = pos & 077;
		dnode_secno dno;

		/*
		 * dnode at position q
		 */
		dno = dir_subdno(inode, q);
		if (dno == 0)
			return 0;

		/*
		 * entry at index r
		 */
		de = map_nth_dirent(inode->i_dev, dno, r, &qbh);
		if (!de || !de->down)
			return 0;

		/*
		 * get the dnode down pointer
		 */
		dno = de_down_pointer(de);
		brelse4(&qbh);

		/*
		 * cache it for next time
		 */
		inode->i_hpfs_dpos = pos;
		inode->i_hpfs_dsubdno = dno;
		return dno;
	}
}

/*
 * Return the dir entry at index n in dnode dno, or 0 if there isn't one
 */

static struct hpfs_dirent *map_nth_dirent(dev_t dev, dnode_secno dno,
					  int n,
					  struct quad_buffer_head *qbh)
{
	int i;
	struct hpfs_dirent *de, *de_end;
	struct dnode *dnode = map_dnode(dev, dno, qbh);

	de = dnode_first_de(dnode);
	de_end = dnode_end_de(dnode);

	for (i = 1; de < de_end; i++, de = de_next_de(de)) {
		if (i == n)
			return de;
		if (de->last || de->length == 0)
			break;
	}

	brelse4(qbh);
	return 0;
}

static int hpfs_dir_read(struct inode *inode, struct file *filp,
			 char *buf, int count)
{
	return -EISDIR;
}

/* Return the dnode pointer in a directory fnode */

static dnode_secno fnode_dno(dev_t dev, ino_t ino)
{
	struct buffer_head *bh;
	struct fnode *fnode;
	dnode_secno dno;

	fnode = map_fnode(dev, ino, &bh);
	if (!fnode)
		return 0;

	dno = fnode->u.external[0].disk_secno;
	brelse(bh);
	return dno;
}

/* Map an fnode into a buffer and return pointers to it and to the buffer. */

static struct fnode *map_fnode(dev_t dev, ino_t ino, struct buffer_head **bhp)
{
	struct fnode *fnode;

	if (ino == 0) {
		printk("HPFS: missing fnode\n");
		return 0;
	}

	fnode = map_sector(dev, ino_secno(ino), bhp);
	if (fnode)
		if (fnode->magic != FNODE_MAGIC) {
			printk("HPFS: map_fnode: bad fnode pointer\n");
			brelse(*bhp);
			return 0;
		}
	return fnode;
}

/* Map an anode into a buffer and return pointers to it and to the buffer. */

static struct anode *map_anode(dev_t dev, unsigned secno,
			       struct buffer_head **bhp)
{
	struct anode *anode;

	if (secno == 0) {
		printk("HPFS: missing anode\n");
		return 0;
	}

	anode = map_sector(dev, secno, bhp);
	if (anode)
		if (anode->magic != ANODE_MAGIC || anode->self != secno) {
			printk("HPFS: map_anode: bad anode pointer\n");
			brelse(*bhp);
			return 0;
		}
	return anode;
}

/* Map a dnode into a buffer and return pointers to it and to the buffer. */

static struct dnode *map_dnode(dev_t dev, unsigned secno,
			       struct quad_buffer_head *qbh)
{
	struct dnode *dnode;

	if (secno == 0) {
		printk("HPFS: missing dnode\n");
		return 0;
	}

	dnode = map_4sectors(dev, secno, qbh);
	if (dnode)
		if (dnode->magic != DNODE_MAGIC || dnode->self != secno) {
			printk("HPFS: map_dnode: bad dnode pointer\n");
			brelse4(qbh);
			return 0;
		}
	return dnode;
}

/* Map a sector into a buffer and return pointers to it and to the buffer. */

static void *map_sector(dev_t dev, unsigned secno, struct buffer_head **bhp)
{
	struct buffer_head *bh;

	if ((*bhp = bh = bread(dev, secno, 512)) != 0)
		return bh->b_data;
	else {
		printk("HPFS: map_sector: read error\n");
		return 0;
	}
}

/* Map 4 sectors into a 4buffer and return pointers to it and to the buffer. */

static void *map_4sectors(dev_t dev, unsigned secno,
			  struct quad_buffer_head *qbh)
{
	struct buffer_head *bh;
	char *data;

	if (secno & 3) {
		printk("HPFS: map_4sectors: unaligned read\n");
		return 0;
	}

	qbh->data = data = kmalloc(2048, GFP_KERNEL);
	if (!data)
		goto bail;

	qbh->bh[0] = bh = breada(dev,
				 secno, secno + 1, secno + 2, secno + 3, -1);
	if (!bh)
		goto bail0;
	memcpy(data, bh->b_data, 512);

	qbh->bh[1] = bh = bread(dev, secno + 1, 512);
	if (!bh)
		goto bail1;
	memcpy(data + 512, bh->b_data, 512);

	qbh->bh[2] = bh = bread(dev, secno + 2, 512);
	if (!bh)
		goto bail2;
	memcpy(data + 2 * 512, bh->b_data, 512);

	qbh->bh[3] = bh = bread(dev, secno + 3, 512);
	if (!bh)
		goto bail3;
	memcpy(data + 3 * 512, bh->b_data, 512);

	return data;

 bail3:
	brelse(qbh->bh[2]);
 bail2:
	brelse(qbh->bh[1]);
 bail1:
	brelse(qbh->bh[0]);
 bail0:
	kfree_s(data, 2048);
 bail:
	printk("HPFS: map_4sectors: read error\n");
	return 0;
}

/* Deallocate a 4-buffer block */

static void brelse4(struct quad_buffer_head *qbh)
{
	brelse(qbh->bh[3]);
	brelse(qbh->bh[2]);
	brelse(qbh->bh[1]);
	brelse(qbh->bh[0]);
	kfree_s(qbh->data, 2048);
}
