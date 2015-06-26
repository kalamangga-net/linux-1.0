/*
 *  linux/include/linux/ext2_fs_i.h
 *
 *  Copyright (C) 1992, 1993, 1994  Remy Card (card@masi.ibp.fr)
 *                                  Laboratoire MASI - Institut Blaise Pascal
 *                                  Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/include/linux/minix_fs_i.h
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#ifndef _LINUX_EXT2_FS_I
#define _LINUX_EXT2_FS_I

/*
 * second extended file system inode data in memory
 */
struct ext2_inode_info {
	unsigned long  i_data[15];
	unsigned long  i_flags;
	unsigned long  i_faddr;
	unsigned char  i_frag;
	unsigned char  i_fsize;
	unsigned short i_pad1;
	unsigned long  i_file_acl;
	unsigned long  i_dir_acl;
	unsigned long  i_dtime;
	unsigned long  i_version;
	unsigned long  i_block_group;
	unsigned long  i_next_alloc_block;
	unsigned long  i_next_alloc_goal;
	unsigned long  i_prealloc_block;
	unsigned long  i_prealloc_count;
};

#endif	/* _LINUX_EXT2_FS_I */
