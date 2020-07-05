#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "devices/disk.h"
#ifdef EFILESYS
#include "filesys/fat.h"
#endif
#include "threads/thread.h"

/* The disk that contains the file system. */
struct disk *filesys_disk;

static void do_format (void);

/* Initializes the file system module.
 * If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) {
	filesys_disk = disk_get (0, 1);
	if (filesys_disk == NULL)
		PANIC ("hd0:1 (hdb) not present, file system initialization failed");

	inode_init ();

#ifdef EFILESYS
	fat_init ();

	if (format)
		do_format ();

	fat_open ();
	
	/* Set ROOT_DIR as the current directory of the Initial process. */
	thread_current()->current_dir = dir_open_root();
#else
	/* Original FS */
	free_map_init ();

	if (format)
		do_format ();

	free_map_open ();
#endif
}

/* Shuts down the file system module, writing any unwritten data
 * to disk. */
void
filesys_done (void) {
	/* Original FS */
#ifdef EFILESYS
	fat_close ();
#else
	free_map_close ();
#endif
}

#ifdef EFILESYS
bool
filesys_create (const char *name, off_t initial_size) {
	cluster_t inode_cluster = 0;
	struct dir *dir = dir_open_root ();
	bool success = (dir != NULL
			&& fat_allocate (1, &inode_cluster)
			&& inode_create (inode_cluster, initial_size, false)
			&& dir_add (dir, name, cluster_to_sector(inode_cluster)));
	if(!success && inode_cluster != 0)
		fat_remove_chain (inode_cluster, 0);
	dir_close (dir);
	return success;
}
#else
/* Creates a file named NAME with the given INITIAL_SIZE.
 * Returns true if successful, false otherwise.
 * Fails if a file named NAME already exists,
 * or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) {
	disk_sector_t inode_sector = 0;
	struct dir *dir = dir_open_root ();
	bool success = (dir != NULL
			&& free_map_allocate (1, &inode_sector)
			&& inode_create (inode_sector, initial_size)
			&& dir_add (dir, name, inode_sector));
	if (!success && inode_sector != 0)
		free_map_release (inode_sector, 1);
	dir_close (dir);

	return success;
}
#endif

/* Opens the file with the given NAME.
 * Returns the new file if successful or a null pointer
 * otherwise.
 * Fails if no file named NAME exists,
 * or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name) {
	struct dir *dir = dir_open_root ();
	struct inode *inode = NULL;

	if (dir != NULL)
		dir_lookup (dir, name, &inode);
	dir_close (dir);

	return file_open (inode);
}

/* Deletes the file named NAME.
 * Returns true if successful, false on failure.
 * Fails if no file named NAME exists,
 * or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) {
	struct dir *dir = dir_open_root ();
	bool success = dir != NULL && dir_remove (dir, name);
	dir_close (dir);

	return success;
}

/* Formats the file system. */
static void
do_format (void) {
	printf ("Formatting file system...");

#ifdef EFILESYS
	/* Create FAT and save it to the disk. */
	fat_create ();
	fat_close ();
#else
	free_map_create ();
	if (!dir_create (ROOT_DIR_SECTOR, 16))
		PANIC ("root directory creation failed");
	free_map_close ();
#endif

	printf ("done.\n");
}

#ifdef EFILESYS
struct dir* parse_path (char* path_name, char* file_name) {
	struct dir* dir;
	if (path_name == NULL || file_name == NULL)
		return NULL;
	if (strlen(path_name) == 0)
		return NULL;
	char *token;
	char *nexttoken;
	char *saveptr;
	token = strtok_r(path_name, "/", &savePtr);
	nexttoken = strtok_r(NULL, "/", &savePtr);
	while(token != NULL && nextToken!= NULL){
		/* dir에서token이름의파일을검색하여inode의정보를저장*/
		struct inode* inode_token;
		dir_lookup (dir, token, &inode_token);
		/* inode가파일일경우NULL 반환*/
		/* dir의디렉터리정보를메모리에서해지*/
		/* inode의디렉터리정보를dir에저장*/
		/* token에검색할경로이름저장*/
	}
	/* token의파일이름을file_name에저장
	/* dir정보반환*/
}
#endif
