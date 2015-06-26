#ifndef _NFS_FS_SB
#define _NFS_FS_SB

#include <linux/nfs.h>

struct nfs_server {
	struct file *file;
	int lock;
	struct wait_queue *wait;
	int flags;
	int rsize;
	int wsize;
	int timeo;
	int retrans;
	int acregmin;
	int acregmax;
	int acdirmin;
	int acdirmax;
	char hostname[256];
};

/*
 * nfs super-block data in memory
 */

struct nfs_sb_info {
	struct nfs_server s_server;
	struct nfs_fh s_root;
};

#endif
