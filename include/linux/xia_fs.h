#ifndef _XIA_FS_H
#define _XIA_FS_H

/*
 * include/linux/xia_fs.h
 *
 * Copyright (C) Q. Frank Xia, 1993.
 *
 * Based on Linus' minix_fs.h.
 * Copyright (C) Linus Torvalds, 1991, 1992.
 */

#define _XIAFS_SUPER_MAGIC 0x012FD16D
#define _XIAFS_ROOT_INO 1
#define _XIAFS_BAD_INO  2

#define _XIAFS_NAME_LEN 248

#define _XIAFS_INODES_PER_BLOCK ((BLOCK_SIZE)/(sizeof(struct xiafs_inode)))

struct xiafs_inode {		/* 64 bytes */
    mode_t   i_mode;
    nlink_t  i_nlinks;
    uid_t    i_uid;
    gid_t    i_gid;
    size_t   i_size;		/* 8 */
    time_t   i_ctime;
    time_t   i_atime;
    time_t   i_mtime;
    daddr_t  i_zone[8];
    daddr_t  i_ind_zone;
    daddr_t  i_dind_zone;
};

/*
 * linux super-block data on disk
 */
struct xiafs_super_block {
    u_char  s_boot_segment[512];	/*  1st sector reserved for boot */
    u_long  s_zone_size;		/*  0: the name says it		 */
    u_long  s_nzones;			/*  1: volume size, zone aligned */ 
    u_long  s_ninodes;			/*  2: # of inodes		 */
    u_long  s_ndatazones;		/*  3: # of data zones		 */
    u_long  s_imap_zones;		/*  4: # of imap zones           */
    u_long  s_zmap_zones;		/*  5: # of zmap zones		 */
    u_long  s_firstdatazone;		/*  6: first data zone           */
    u_long  s_zone_shift;		/*  7: z size = 1KB << z shift   */
    u_long  s_max_size;			/*  8: max size of a single file */
    u_long  s_reserved0;		/*  9: reserved			 */
    u_long  s_reserved1;		/* 10: 				 */
    u_long  s_reserved2;		/* 11:				 */
    u_long  s_reserved3;		/* 12:				 */
    u_long  s_firstkernzone;		/* 13: first kernel zone	 */
    u_long  s_kernzones;		/* 14: kernel size in zones	 */
    u_long  s_magic;			/* 15: magic number for xiafs    */
};

struct xiafs_direct {
    ino_t   d_ino;
    u_short d_rec_len;
    u_char  d_name_len;
    char    d_name[_XIAFS_NAME_LEN+1];
};

extern int xiafs_lookup(struct inode * dir,const char * name, int len, 
			struct inode ** result);
extern int xiafs_create(struct inode * dir,const char * name, int len, int mode,
		      	struct inode ** result);
extern int xiafs_mkdir(struct inode * dir, const char * name, int len, int mode);
extern int xiafs_rmdir(struct inode * dir, const char * name, int len);
extern int xiafs_unlink(struct inode * dir, const char * name, int len);
extern int xiafs_symlink(struct inode * inode, const char * name, int len,
		       	const char * symname);
extern int xiafs_link(struct inode * oldinode, struct inode * dir, 
		    	const char * name, int len);
extern int xiafs_mknod(struct inode * dir, const char * name, int len, 
		     	int mode, int rdev);
extern int xiafs_rename(struct inode * old_dir, const char * old_name, 
		      	int old_len, struct inode * new_dir, 
		      	const char * new_name, int new_len);
extern struct inode * xiafs_new_inode(struct inode * dir);
extern void xiafs_free_inode(struct inode * inode);
extern unsigned long xiafs_count_free_inodes(struct super_block *sb);
extern int xiafs_new_zone(struct super_block * sb, u_long prev_addr);
extern void xiafs_free_zone(struct super_block * sb, int block);
extern unsigned long xiafs_count_free_zones(struct super_block *sb);

extern int xiafs_bmap(struct inode *,int);

extern struct buffer_head * xiafs_getblk(struct inode *, int, int);
extern struct buffer_head * xiafs_bread(struct inode *, int, int);

extern void xiafs_truncate(struct inode *);
extern void xiafs_put_super(struct super_block *);
extern struct super_block *xiafs_read_super(struct super_block *,void *,int);
extern void xiafs_read_inode(struct inode *);
extern void xiafs_write_inode(struct inode *);
extern void xiafs_put_inode(struct inode *);
extern void xiafs_statfs(struct super_block *, struct statfs *);
extern int xiafs_sync_inode(struct inode *);
extern int xiafs_sync_file(struct inode *, struct file *);

extern struct inode_operations xiafs_file_inode_operations;
extern struct inode_operations xiafs_dir_inode_operations;
extern struct inode_operations xiafs_symlink_inode_operations;

#endif  /* _XIA_FS_H */








