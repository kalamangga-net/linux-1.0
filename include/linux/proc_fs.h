#ifndef _LINUX_PROC_FS_H
#define _LINUX_PROC_FS_H

/*
 * The proc filesystem constants/structures
 */

#define PROC_ROOT_INO 1

#define PROC_SUPER_MAGIC 0x9fa0

struct proc_dir_entry {
	unsigned short low_ino;
	unsigned short namelen;
	char * name;
};

extern struct super_block *proc_read_super(struct super_block *,void *,int);
extern void proc_put_inode(struct inode *);
extern void proc_put_super(struct super_block *);
extern void proc_statfs(struct super_block *, struct statfs *);
extern void proc_read_inode(struct inode *);
extern void proc_write_inode(struct inode *);
extern int proc_match(int, const char *, struct proc_dir_entry *);

extern struct inode_operations proc_root_inode_operations;
extern struct inode_operations proc_base_inode_operations;
extern struct inode_operations proc_net_inode_operations;
extern struct inode_operations proc_mem_inode_operations;
extern struct inode_operations proc_array_inode_operations;
extern struct inode_operations proc_kmsg_inode_operations;
extern struct inode_operations proc_link_inode_operations;
extern struct inode_operations proc_fd_inode_operations;
extern struct inode_operations proc_net_inode_operations;

#endif
