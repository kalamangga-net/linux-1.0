/*
 *  linux/fs/msdos/inode.c
 *
 *  Written 1992,1993 by Werner Almesberger
 */

#include <linux/msdos_fs.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/stat.h>
#include <linux/locks.h>

#include <asm/segment.h>


void msdos_put_inode(struct inode *inode)
{
	struct inode *depend;
	struct super_block *sb;

	if (inode->i_nlink) {
		if (MSDOS_I(inode)->i_busy) cache_inval_inode(inode);
		return;
	}
	inode->i_size = 0;
	msdos_truncate(inode);
	depend = MSDOS_I(inode)->i_depend;
	sb = inode->i_sb;
	clear_inode(inode);
	if (depend) {
		if (MSDOS_I(depend)->i_old != inode) {
			printk("Invalid link (0x%X): expected 0x%X, got 0x%X\n",
			    (int) depend,(int) inode,(int) MSDOS_I(depend)->
			    i_old);
			fs_panic(sb,"...");
			return;
		}
		MSDOS_I(depend)->i_old = NULL;
		iput(depend);
	}
}


void msdos_put_super(struct super_block *sb)
{
	cache_inval_dev(sb->s_dev);
	lock_super(sb);
	sb->s_dev = 0;
	unlock_super(sb);
	return;
}


static struct super_operations msdos_sops = { 
	msdos_read_inode,
	msdos_notify_change,
	msdos_write_inode,
	msdos_put_inode,
	msdos_put_super,
	NULL, /* added in 0.96c */
	msdos_statfs,
	NULL
};


static int parse_options(char *options,char *check,char *conversion,uid_t *uid,
    gid_t *gid,int *umask,int *debug,int *fat,int *quiet)
{
	char *this_char,*value;

	*check = 'n';
	*conversion = 'b';
	*uid = current->uid;
	*gid = current->gid;
	*umask = current->umask;
	*debug = *fat = *quiet = 0;
	if (!options) return 1;
	for (this_char = strtok(options,","); this_char; this_char = strtok(NULL,",")) {
		if ((value = strchr(this_char,'=')) != NULL)
			*value++ = 0;
		if (!strcmp(this_char,"check") && value) {
			if (value[0] && !value[1] && strchr("rns",*value))
				*check = *value;
			else if (!strcmp(value,"relaxed")) *check = 'r';
			else if (!strcmp(value,"normal")) *check = 'n';
			else if (!strcmp(value,"strict")) *check = 's';
			else return 0;
		}
		else if (!strcmp(this_char,"conv") && value) {
			if (value[0] && !value[1] && strchr("bta",*value))
				*conversion = *value;
			else if (!strcmp(value,"binary")) *conversion = 'b';
			else if (!strcmp(value,"text")) *conversion = 't';
			else if (!strcmp(value,"auto")) *conversion = 'a';
			else return 0;
		}
		else if (!strcmp(this_char,"uid")) {
			if (!value || !*value)
				return 0;
			*uid = simple_strtoul(value,&value,0);
			if (*value)
				return 0;
		}
		else if (!strcmp(this_char,"gid")) {
			if (!value || !*value)
				return 0;
			*gid = simple_strtoul(value,&value,0);
			if (*value)
				return 0;
		}
		else if (!strcmp(this_char,"umask")) {
			if (!value || !*value)
				return 0;
			*umask = simple_strtoul(value,&value,8);
			if (*value)
				return 0;
		}
		else if (!strcmp(this_char,"debug")) {
			if (value) return 0;
			*debug = 1;
		}
		else if (!strcmp(this_char,"fat")) {
			if (!value || !*value)
				return 0;
			*fat = simple_strtoul(value,&value,0);
			if (*value || (*fat != 12 && *fat != 16))
				return 0;
		}
		else if (!strcmp(this_char,"quiet")) {
			if (value) return 0;
			*quiet = 1;
		}
		else return 0;
	}
	return 1;
}


/* Read the super block of an MS-DOS FS. */

struct super_block *msdos_read_super(struct super_block *s,void *data,
				     int silent)
{
	struct buffer_head *bh;
	struct msdos_boot_sector *b;
	int data_sectors,logical_sector_size,sector_mult;
	int debug,error,fat,quiet;
	char check,conversion;
	uid_t uid;
	gid_t gid;
	int umask;

	if (!parse_options((char *) data,&check,&conversion,&uid,&gid,&umask,
	    &debug,&fat,&quiet)) {
		s->s_dev = 0;
		return NULL;
	}
	cache_init();
	lock_super(s);
	bh = bread(s->s_dev, 0, BLOCK_SIZE);
	unlock_super(s);
	if (bh == NULL) {
		s->s_dev = 0;
		printk("MSDOS bread failed\n");
		return NULL;
	}
	b = (struct msdos_boot_sector *) bh->b_data;
	s->s_blocksize = 1024;	/* we cannot handle anything else yet */
	s->s_blocksize_bits = 10;	/* we cannot handle anything else yet */

/*
 * The DOS3 partition size limit is *not* 32M as many people think.  
 * Instead, it is 64K sectors (with the usual sector size being
 * 512 bytes, leading to a 32M limit).
 * 
 * DOS 3 partition managers got around this problem by faking a 
 * larger sector size, ie treating multiple physical sectors as 
 * a single logical sector.
 * 
 * We can accommodate this scheme by adjusting our cluster size,
 * fat_start, and data_start by an appropriate value.
 *
 * (by Drew Eckhardt)
 */

#define ROUND_TO_MULTIPLE(n,m) ((n) && (m) ? (n)+(m)-1-((n)-1)%(m) : 0)
    /* don't divide by zero */

	logical_sector_size = CF_LE_W(*(unsigned short *) &b->sector_size);
	sector_mult = logical_sector_size >> SECTOR_BITS;
	MSDOS_SB(s)->cluster_size = b->cluster_size*sector_mult;
	MSDOS_SB(s)->fats = b->fats;
	MSDOS_SB(s)->fat_start = CF_LE_W(b->reserved)*sector_mult;
	MSDOS_SB(s)->fat_length = CF_LE_W(b->fat_length)*sector_mult;
	MSDOS_SB(s)->dir_start = (CF_LE_W(b->reserved)+b->fats*CF_LE_W(
	    b->fat_length))*sector_mult;
	MSDOS_SB(s)->dir_entries = CF_LE_W(*((unsigned short *) &b->dir_entries
	    ));
	MSDOS_SB(s)->data_start = MSDOS_SB(s)->dir_start+ROUND_TO_MULTIPLE((
	    MSDOS_SB(s)->dir_entries << MSDOS_DIR_BITS) >> SECTOR_BITS,
	    sector_mult);
	data_sectors = (CF_LE_W(*((unsigned short *) &b->sectors)) ?
	    CF_LE_W(*((unsigned short *) &b->sectors)) :
	    CF_LE_L(b->total_sect))*sector_mult-MSDOS_SB(s)->data_start;
	error = !b->cluster_size || !sector_mult;
	if (!error) {
		MSDOS_SB(s)->clusters = b->cluster_size ? data_sectors/
		    b->cluster_size/sector_mult : 0;
		MSDOS_SB(s)->fat_bits = fat ? fat : MSDOS_SB(s)->clusters >
		    MSDOS_FAT12 ? 16 : 12;
		error = !MSDOS_SB(s)->fats || (MSDOS_SB(s)->dir_entries &
		    (MSDOS_DPS-1)) || MSDOS_SB(s)->clusters+2 > MSDOS_SB(s)->
		    fat_length*SECTOR_SIZE*8/MSDOS_SB(s)->fat_bits ||
		    (logical_sector_size & (SECTOR_SIZE-1)) || !b->secs_track ||
		    !b->heads;
	}
	brelse(bh);
	if (error || debug) {
		printk("[MS-DOS FS Rel. 12,FAT %d,check=%c,conv=%c,"
		    "uid=%d,gid=%d,umask=%03o%s]\n",MSDOS_SB(s)->fat_bits,check,
		    conversion,uid,gid,umask,MSDOS_CAN_BMAP(MSDOS_SB(s)) ?
		    ",bmap" : "");
		printk("[me=0x%x,cs=%d,#f=%d,fs=%d,fl=%d,ds=%d,de=%d,data=%d,"
		    "se=%d,ts=%ld,ls=%d]\n",b->media,MSDOS_SB(s)->cluster_size,
		    MSDOS_SB(s)->fats,MSDOS_SB(s)->fat_start,MSDOS_SB(s)->
		    fat_length,MSDOS_SB(s)->dir_start,MSDOS_SB(s)->dir_entries,
		    MSDOS_SB(s)->data_start,CF_LE_W(*(unsigned short *) &b->
		    sectors),b->total_sect,logical_sector_size);
	}
	if (error) {
		if (!silent)
			printk("VFS: Can't find a valid MSDOS filesystem on dev 0x%04x.\n",
				   s->s_dev);
		s->s_dev = 0;
		return NULL;
	}
	s->s_magic = MSDOS_SUPER_MAGIC;
	MSDOS_SB(s)->name_check = check;
	MSDOS_SB(s)->conversion = conversion;
	/* set up enough so that it can read an inode */
	s->s_op = &msdos_sops;
	MSDOS_SB(s)->fs_uid = uid;
	MSDOS_SB(s)->fs_gid = gid;
	MSDOS_SB(s)->fs_umask = umask;
	MSDOS_SB(s)->quiet = quiet;
	MSDOS_SB(s)->free_clusters = -1; /* don't know yet */
	MSDOS_SB(s)->fat_wait = NULL;
	MSDOS_SB(s)->fat_lock = 0;
	MSDOS_SB(s)->prev_free = 0;
	if (!(s->s_mounted = iget(s,MSDOS_ROOT_INO))) {
		s->s_dev = 0;
		printk("get root inode failed\n");
		return NULL;
	}
	return s;
}


void msdos_statfs(struct super_block *sb,struct statfs *buf)
{
	int free,nr;

	put_fs_long(sb->s_magic,&buf->f_type);
	put_fs_long(MSDOS_SB(sb)->cluster_size*SECTOR_SIZE,&buf->f_bsize);
	put_fs_long(MSDOS_SB(sb)->clusters,&buf->f_blocks);
	lock_fat(sb);
	if (MSDOS_SB(sb)->free_clusters != -1)
		free = MSDOS_SB(sb)->free_clusters;
	else {
		free = 0;
		for (nr = 2; nr < MSDOS_SB(sb)->clusters+2; nr++)
			if (!fat_access(sb,nr,-1)) free++;
		MSDOS_SB(sb)->free_clusters = free;
	}
	unlock_fat(sb);
	put_fs_long(free,&buf->f_bfree);
	put_fs_long(free,&buf->f_bavail);
	put_fs_long(0,&buf->f_files);
	put_fs_long(0,&buf->f_ffree);
	put_fs_long(12,&buf->f_namelen);
}


int msdos_bmap(struct inode *inode,int block)
{
	struct msdos_sb_info *sb;
	int cluster,offset;

	sb = MSDOS_SB(inode->i_sb);
	if ((sb->cluster_size & 1) || (sb->data_start & 1)) return 0;
	if (inode->i_ino == MSDOS_ROOT_INO) {
		if (sb->dir_start & 1) return 0;
		return (sb->dir_start >> 1)+block;
	}
	cluster = (block*2)/sb->cluster_size;
	offset = (block*2) % sb->cluster_size;
	if (!(cluster = get_cluster(inode,cluster))) return 0;
	return ((cluster-2)*sb->cluster_size+sb->data_start+offset) >> 1;
}


void msdos_read_inode(struct inode *inode)
{
	struct buffer_head *bh;
	struct msdos_dir_entry *raw_entry;
	int nr;

/* printk("read inode %d\n",inode->i_ino); */
	MSDOS_I(inode)->i_busy = 0;
	MSDOS_I(inode)->i_depend = MSDOS_I(inode)->i_old = NULL;
	MSDOS_I(inode)->i_binary = 1;
	inode->i_uid = MSDOS_SB(inode->i_sb)->fs_uid;
	inode->i_gid = MSDOS_SB(inode->i_sb)->fs_gid;
	if (inode->i_ino == MSDOS_ROOT_INO) {
		inode->i_mode = (S_IRWXUGO & ~MSDOS_SB(inode->i_sb)->fs_umask) |
		    S_IFDIR;
		inode->i_op = &msdos_dir_inode_operations;
		inode->i_nlink = msdos_subdirs(inode)+2;
		    /* subdirs (neither . nor ..) plus . and "self" */
		inode->i_size = MSDOS_SB(inode->i_sb)->dir_entries*
		    sizeof(struct msdos_dir_entry);
		inode->i_blksize = MSDOS_SB(inode->i_sb)->cluster_size*
		    SECTOR_SIZE;
		inode->i_blocks = (inode->i_size+inode->i_blksize-1)/
		    inode->i_blksize*MSDOS_SB(inode->i_sb)->cluster_size;
		MSDOS_I(inode)->i_start = 0;
		MSDOS_I(inode)->i_attrs = 0;
		inode->i_mtime = inode->i_atime = inode->i_ctime = 0;
		return;
	}
	if (!(bh = bread(inode->i_dev,inode->i_ino >> MSDOS_DPB_BITS,
	    BLOCK_SIZE))) {
		printk("dev = 0x%04X, ino = %ld\n",inode->i_dev,inode->i_ino);
		panic("msdos_read_inode: unable to read i-node block");
	}
	raw_entry = &((struct msdos_dir_entry *) (bh->b_data))
	    [inode->i_ino & (MSDOS_DPB-1)];
	if ((raw_entry->attr & ATTR_DIR) && !IS_FREE(raw_entry->name)) {
		inode->i_mode = MSDOS_MKMODE(raw_entry->attr,S_IRWXUGO &
		    ~MSDOS_SB(inode->i_sb)->fs_umask) | S_IFDIR;
		inode->i_op = &msdos_dir_inode_operations;
		MSDOS_I(inode)->i_start = CF_LE_W(raw_entry->start);
		inode->i_nlink = msdos_subdirs(inode);
		    /* includes .., compensating for "self" */
#ifdef DEBUG
		if (!inode->i_nlink) {
			printk("directory %d: i_nlink == 0\n",inode->i_ino);
			inode->i_nlink = 1;
		}
#endif
		inode->i_size = 0;
		if ((nr = CF_LE_W(raw_entry->start)) != 0)
			while (nr != -1) {
				inode->i_size += SECTOR_SIZE*MSDOS_SB(inode->
				    i_sb)->cluster_size;
				if (!(nr = fat_access(inode->i_sb,nr,-1))) {
					printk("Directory %ld: bad FAT\n",
					    inode->i_ino);
					break;
				}
			}
	}
	else {
		inode->i_mode = MSDOS_MKMODE(raw_entry->attr,(IS_NOEXEC(inode)
		    ? S_IRUGO|S_IWUGO : S_IRWXUGO) & ~MSDOS_SB(inode->i_sb)->fs_umask) |
		    S_IFREG;
		inode->i_op = MSDOS_CAN_BMAP(MSDOS_SB(inode->i_sb)) ? 
		    &msdos_file_inode_operations :
		    &msdos_file_inode_operations_no_bmap;
		MSDOS_I(inode)->i_start = CF_LE_W(raw_entry->start);
		inode->i_nlink = 1;
		inode->i_size = CF_LE_L(raw_entry->size);
	}
	MSDOS_I(inode)->i_binary = is_binary(MSDOS_SB(inode->i_sb)->conversion,
	    raw_entry->ext);
	MSDOS_I(inode)->i_attrs = raw_entry->attr & ATTR_UNUSED;
	/* this is as close to the truth as we can get ... */
	inode->i_blksize = MSDOS_SB(inode->i_sb)->cluster_size*SECTOR_SIZE;
	inode->i_blocks = (inode->i_size+inode->i_blksize-1)/
	    inode->i_blksize*MSDOS_SB(inode->i_sb)->cluster_size;
	inode->i_mtime = inode->i_atime = inode->i_ctime =
	    date_dos2unix(CF_LE_W(raw_entry->time),CF_LE_W(raw_entry->date));
	brelse(bh);
}


void msdos_write_inode(struct inode *inode)
{
	struct buffer_head *bh;
	struct msdos_dir_entry *raw_entry;

	inode->i_dirt = 0;
	if (inode->i_ino == MSDOS_ROOT_INO || !inode->i_nlink) return;
	if (!(bh = bread(inode->i_dev,inode->i_ino >> MSDOS_DPB_BITS,
	    BLOCK_SIZE))) {
		printk("dev = 0x%04X, ino = %ld\n",inode->i_dev,inode->i_ino);
		panic("msdos_write_inode: unable to read i-node block");
	}
	raw_entry = &((struct msdos_dir_entry *) (bh->b_data))
	    [inode->i_ino & (MSDOS_DPB-1)];
	if (S_ISDIR(inode->i_mode)) {
		raw_entry->attr = ATTR_DIR;
		raw_entry->size = 0;
	}
	else {
		raw_entry->attr = ATTR_NONE;
		raw_entry->size = CT_LE_L(inode->i_size);
	}
	raw_entry->attr |= MSDOS_MKATTR(inode->i_mode) |
	    MSDOS_I(inode)->i_attrs;
	raw_entry->start = CT_LE_L(MSDOS_I(inode)->i_start);
	date_unix2dos(inode->i_mtime,&raw_entry->time,&raw_entry->date);
	raw_entry->time = CT_LE_W(raw_entry->time);
	raw_entry->date = CT_LE_W(raw_entry->date);
	bh->b_dirt = 1;
	brelse(bh);
}


int msdos_notify_change(int flags,struct inode *inode)
{
	int error;

	error = 0;
	if ((flags & NOTIFY_UIDGID) && (inode->i_uid != MSDOS_SB(inode->i_sb)->
	    fs_uid || inode->i_gid != MSDOS_SB(inode->i_sb)->fs_gid)) {
		inode->i_uid = MSDOS_SB(inode->i_sb)->fs_uid;
		inode->i_gid = MSDOS_SB(inode->i_sb)->fs_gid;
		error = -EPERM;
	}
	if (!(flags & NOTIFY_MODE))
		return MSDOS_SB(inode->i_sb)->quiet ? 0 : error;
	if (inode->i_mode & ~MSDOS_VALID_MODE) {
		inode->i_mode &= MSDOS_VALID_MODE;
		error = -EPERM;
	}
	if (IS_NOEXEC(inode) && !S_ISDIR(inode->i_mode))
		inode->i_mode &= S_IFMT | S_IRUGO | S_IWUGO;
	else inode->i_mode |= S_IXUGO;
	inode->i_mode = ((inode->i_mode & S_IFMT) | ((((inode->i_mode & S_IRWXU
	    & ~MSDOS_SB(inode->i_sb)->fs_umask) | S_IRUSR) >> 6)*S_IXUGO)) &
	    ~MSDOS_SB(inode->i_sb)->fs_umask;
	return MSDOS_SB(inode->i_sb)->quiet ? 0 : error;
}
