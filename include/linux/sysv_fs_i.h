#ifndef _SYSV_FS_I
#define _SYSV_FS_I

/*
 * SystemV/Coherent FS inode data in memory
 */
struct sysv_inode_info {
	unsigned long i_data[10+1+1+1];	/* zone numbers: max. 10 data blocks,
					 * then 1 indirection block,
					 * then 1 double indirection block,
					 * then 1 triple indirection block.
					 */
	/* the following are only used if  block_size < BLOCK_SIZE  */
	int i_lock;			/* lock to protect against simultaneous	*/
	struct wait_queue * i_wait;	/* write and truncate			*/
					/*					*/
};

#endif

