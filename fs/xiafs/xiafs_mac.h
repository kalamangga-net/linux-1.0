/*
 *  linux/fs/xiafs/xiafs_mac.h
 *
 *  Copyright (C) Q. Frank Xia, 1993.
 */

extern char internal_error_message[];
#define INTERN_ERR		internal_error_message, __FILE__, __LINE__
#define WHERE_ERR		__FILE__, __LINE__

#define XIAFS_ZSHIFT(sp)		((sp)->u.xiafs_sb.s_zone_shift)
#define XIAFS_ZSIZE(sp)		(BLOCK_SIZE << XIAFS_ZSHIFT(sp))
#define XIAFS_ZSIZE_BITS(sp)	(BLOCK_SIZE_BITS + XIAFS_ZSHIFT(sp))
#define XIAFS_ADDRS_PER_Z(sp)   	(BLOCK_SIZE >> (2 - XIAFS_ZSHIFT(sp)))
#define XIAFS_ADDRS_PER_Z_BITS(sp) 	(BLOCK_SIZE_BITS - 2 + XIAFS_ZSHIFT(sp))
#define XIAFS_BITS_PER_Z(sp)	(BLOCK_SIZE  << (3 + XIAFS_ZSHIFT(sp)))
#define XIAFS_BITS_PER_Z_BITS(sp)	(BLOCK_SIZE_BITS + 3 + XIAFS_ZSHIFT(sp))
#define XIAFS_INODES_PER_Z(sp)	(_XIAFS_INODES_PER_BLOCK << XIAFS_ZSHIFT(sp))

/* Use the most significant bytes of zone pointers to store block counter. */
/* This is ugly, but it works. Note, We have another 7 bytes for "expension". */

#define XIAFS_GET_BLOCKS(row_ip, blocks)  \
  blocks=((((row_ip)->i_zone[0] >> 24) & 0xff )|\
	  (((row_ip)->i_zone[1] >> 16) & 0xff00 )|\
	  (((row_ip)->i_zone[2] >>  8) & 0xff0000 ) )

/* XIAFS_PUT_BLOCKS should be called before saving zone pointers */
#define XIAFS_PUT_BLOCKS(row_ip, blocks)  \
  (row_ip)->i_zone[2]=((blocks)<< 8) & 0xff000000;\
  (row_ip)->i_zone[1]=((blocks)<<16) & 0xff000000;\
  (row_ip)->i_zone[0]=((blocks)<<24) & 0xff000000
