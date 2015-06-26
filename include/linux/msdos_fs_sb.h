#ifndef _MSDOS_FS_SB
#define _MSDOS_FS_SB

/*
 * MS-DOS file system in-core superblock data
 */

struct msdos_sb_info {
	unsigned short cluster_size; /* sectors/cluster */
	unsigned char fats,fat_bits; /* number of FATs, FAT bits (12 or 16) */
	unsigned short fat_start,fat_length; /* FAT start & length (sec.) */
	unsigned short dir_start,dir_entries; /* root dir start & entries */
	unsigned short data_start;   /* first data sector */
	unsigned long clusters;      /* number of clusters */
	uid_t fs_uid;
	gid_t fs_gid;
	int quiet; /* fake successful chmods and chowns */
	unsigned short fs_umask;
	unsigned char name_check; /* r = relaxed, n = normal, s = strict */
	unsigned char conversion; /* b = binary, t = text, a = auto */
	struct wait_queue *fat_wait;
	int fat_lock;
	int prev_free; /* previously returned free cluster number */
	int free_clusters; /* -1 if undefined */
};

#endif
