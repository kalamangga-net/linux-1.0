#ifndef _NFS_FS_I
#define _NFS_FS_I

#include <linux/nfs.h>

/*
 * nfs fs inode data in memory
 */
struct nfs_inode_info {
	struct nfs_fh fhandle;
};

#endif
