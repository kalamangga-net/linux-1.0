/*
 *  linux/fs/filesystems.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  table of configured filesystems
 */

#include <linux/config.h>
#include <linux/fs.h>
#ifdef CONFIG_MINIX_FS
#include <linux/minix_fs.h>
#endif
#ifdef CONFIG_XIA_FS
#include <linux/xia_fs.h>
#endif
#ifdef CONFIG_PROC_FS
#include <linux/proc_fs.h>
#endif
#ifdef CONFIG_EXT2_FS
#include <linux/ext2_fs.h>
#endif
#ifdef CONFIG_EXT_FS
#include <linux/ext_fs.h>
#endif
#ifdef CONFIG_MSDOS_FS
#include <linux/msdos_fs.h>
#endif
#ifdef CONFIG_NFS_FS
#include <linux/nfs_fs.h>
#endif
#ifdef CONFIG_ISO9660_FS
#include <linux/iso_fs.h>
#endif
#ifdef CONFIG_HPFS_FS
#include <linux/hpfs_fs.h>
#endif
#ifdef CONFIG_SYSV_FS
#include <linux/sysv_fs.h>
#endif

struct file_system_type file_systems[] = {
#ifdef CONFIG_MINIX_FS
	{minix_read_super,	"minix",	1},
#endif
#ifdef CONFIG_EXT_FS
	{ext_read_super,	"ext",		1},
#endif
#ifdef CONFIG_EXT2_FS
	{ext2_read_super,	"ext2",		1},
#endif
#ifdef CONFIG_XIA_FS
	{xiafs_read_super,	"xiafs",	1},
#endif
#ifdef CONFIG_MSDOS_FS
	{msdos_read_super,	"msdos",	1},
#endif
#ifdef CONFIG_PROC_FS
	{proc_read_super,	"proc",		0},
#endif
#ifdef CONFIG_NFS_FS
	{nfs_read_super,	"nfs",		0},
#endif
#ifdef CONFIG_ISO9660_FS
	{isofs_read_super,	"iso9660",	1},
#endif
#ifdef CONFIG_SYSV_FS
	{sysv_read_super,	"xenix",	1},
	{sysv_read_super,	"sysv",		1},
	{sysv_read_super,	"coherent",	1},
#endif
#ifdef CONFIG_HPFS_FS
	{hpfs_read_super,	"hpfs",		1},
#endif
	{NULL,			NULL,		0}
};
