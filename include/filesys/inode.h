#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/disk.h"
#include "filesys/fat.h"
#include "filesys/file.h"

struct bitmap;

void inode_init (void);
#ifdef EFILESYS
bool inode_create (cluster_t cluster, off_t length, bool is_dir);
bool do_isdir (struct file* file);
bool inode_isdir (struct inode* inode);
#else
bool inode_create (disk_sector_t, off_t);
#endif
struct inode *inode_open (disk_sector_t);
struct inode *inode_reopen (struct inode *);
disk_sector_t inode_get_inumber (const struct inode *);
void inode_close (struct inode *);
void inode_remove (struct inode *);
off_t inode_read_at (struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at (struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write (struct inode *);
void inode_allow_write (struct inode *);
off_t inode_length (const struct inode *);

#endif /* filesys/inode.h */
