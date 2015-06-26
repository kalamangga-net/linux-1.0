#ifndef _XIA_FS_SB_H
#define _XIA_FS_SB_H

/*
 * include/linux/xia_fs_sb.h
 *
 * Copyright (C) Q. Frank Xia, 1993.
 *
 * Based on Linus' minix_fs_sb.h.
 * Copyright (C) Linus Torvalds, 1991, 1992.
 */

#define _XIAFS_IMAP_SLOTS 8
#define _XIAFS_ZMAP_SLOTS 32

struct xiafs_sb_info {
    u_long   s_nzones;
    u_long   s_ninodes;
    u_long   s_ndatazones;
    u_long   s_imap_zones;
    u_long   s_zmap_zones;
    u_long   s_firstdatazone;
    u_long   s_zone_shift;
    u_long   s_max_size;				/*  32 bytes */
    struct buffer_head * s_imap_buf[_XIAFS_IMAP_SLOTS];	/*  32 bytes */
    struct buffer_head * s_zmap_buf[_XIAFS_ZMAP_SLOTS];	/* 128 bytes */
    int      s_imap_iznr[_XIAFS_IMAP_SLOTS];		/*  32 bytes */
    int      s_zmap_zznr[_XIAFS_ZMAP_SLOTS];		/* 128 bytes */
    u_char   s_imap_cached;			/* flag for cached imap */
    u_char   s_zmap_cached;			/* flag for cached imap */
};

#endif /* _XIA_FS_SB_H */



