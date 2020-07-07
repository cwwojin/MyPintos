#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#ifdef EFILESYS
#include "filesys/fat.h"
#endif
#include "threads/thread.h"

/* A directory. */
struct dir {
	struct inode *inode;                /* Backing store. */
	off_t pos;                          /* Current position. */
};

/* A single directory entry. */
struct dir_entry {
	disk_sector_t inode_sector;         /* Sector number of header. */
	char name[NAME_MAX + 1];            /* Null terminated file name. */
	bool in_use;                        /* In use or free? */
};

/* Creates a directory with space for ENTRY_CNT entries in the
 * given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (disk_sector_t sector, size_t entry_cnt) {
#ifdef EFILESYS
	bool result = inode_create (sector_to_cluster(sector), entry_cnt * sizeof (struct dir_entry), true);
	if(result){	//add '.' and '..' to each directory.
		char cur[2] = ".";
		char parent[3] = "..";
		struct dir* new_dir = dir_open(inode_open(sector));
		dir_add(new_dir, cur, sector);
		if(sector != cluster_to_sector(ROOT_DIR_CLUSTER)){
			//dir_add(new_dir, cur, sector);
			struct dir* cur_dir = thread_current()->current_dir;
			dir_add(new_dir, parent, inode_get_inumber(cur_dir->inode));
		}
		dir_close(new_dir);
	}
	return result;
#else
	return inode_create (sector, entry_cnt * sizeof (struct dir_entry));
#endif
}

/* Opens and returns the directory for the given INODE, of which
 * it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) {
	struct dir *dir = calloc (1, sizeof *dir);
	if (inode != NULL && dir != NULL) {
		dir->inode = inode;
		dir->pos = 0;
		return dir;
	} else {
		inode_close (inode);
		free (dir);
		return NULL;
	}
}

/* Opens the root directory and returns a directory for it.
 * Return true if successful, false on failure. */
struct dir *
dir_open_root (void) {
#ifdef EFILESYS
	return dir_open (inode_open (cluster_to_sector(ROOT_DIR_CLUSTER)));
#else
	return dir_open (inode_open (ROOT_DIR_SECTOR));
#endif
}

/* Opens and returns a new directory for the same inode as DIR.
 * Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) {
	return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) {
	if (dir != NULL) {
		inode_close (dir->inode);
		free (dir);
	}
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) {
	return dir->inode;
}

/* Searches DIR for a file with the given NAME.
 * If successful, returns true, sets *EP to the directory entry
 * if EP is non-null, and sets *OFSP to the byte offset of the
 * directory entry if OFSP is non-null.
 * otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
		struct dir_entry *ep, off_t *ofsp) {
	struct dir_entry e;
	size_t ofs;

	ASSERT (dir != NULL);
	ASSERT (name != NULL);

	for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
			ofs += sizeof e)
		if (e.in_use && !strcmp (name, e.name)) {
			if (ep != NULL)
				*ep = e;
			if (ofsp != NULL)
				*ofsp = ofs;
			return true;
		}
	return false;
}

/* Searches DIR for a file with the given NAME
 * and returns true if one exists, false otherwise.
 * On success, sets *INODE to an inode for the file, otherwise to
 * a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
		struct inode **inode) {
	struct dir_entry e;

	ASSERT (dir != NULL);
	ASSERT (name != NULL);

	if (lookup (dir, name, &e, NULL))
		*inode = inode_open (e.inode_sector);
	else
		*inode = NULL;

	return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
 * file by that name.  The file's inode is in sector
 * INODE_SECTOR.
 * Returns true if successful, false on failure.
 * Fails if NAME is invalid (i.e. too long) or a disk or memory
 * error occurs. */
bool
dir_add (struct dir *dir, const char *name, disk_sector_t inode_sector) {
	struct dir_entry e;
	off_t ofs;
	bool success = false;

	ASSERT (dir != NULL);
	ASSERT (name != NULL);

	/* Check NAME for validity. */
	if (*name == '\0' || strlen (name) > NAME_MAX)
		return false;

	/* Check that NAME is not in use. */
	if (lookup (dir, name, NULL, NULL))
		goto done;

	/* Set OFS to offset of free slot.
	 * If there are no free slots, then it will be set to the
	 * current end-of-file.

	 * inode_read_at() will only return a short read at end of file.
	 * Otherwise, we'd need to verify that we didn't get a short
	 * read due to something intermittent such as low memory. */
	for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
			ofs += sizeof e)
		if (!e.in_use)
			break;

	/* Write slot. */
	e.in_use = true;
	strlcpy (e.name, name, sizeof e.name);
	e.inode_sector = inode_sector;
	success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

done:
	return success;
}

/* Removes any entry for NAME in DIR.
 * Returns true if successful, false on failure,
 * which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) {
#ifdef EFILESYS
	if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return false;
#endif
	struct dir_entry e;
	struct inode *inode = NULL;
	bool success = false;
	off_t ofs;

	ASSERT (dir != NULL);
	ASSERT (name != NULL);

	/* Find directory entry. */
	if (!lookup (dir, name, &e, &ofs))
		goto done;

	/* Open inode. */
	inode = inode_open (e.inode_sector);
	if (inode == NULL)
		goto done;
#ifdef EFILESYS
	if (inode_isdir(inode)){	//directory inode but NOT empty!!
		if(!dir_isempty(dir_open(inode)) 
			|| inode_get_inumber(inode) == inode_get_inumber(dir_get_inode(thread_current()->current_dir)))
			goto done;
	}
#endif
	/* Erase directory entry. */
	e.in_use = false;
	if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e)
		goto done;

	/* Remove inode. */
	inode_remove (inode);
	success = true;

done:
	inode_close (inode);
	return success;
}

/* Reads the next directory entry in DIR and stores the name in
 * NAME.  Returns true if successful, false if the directory
 * contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1]) {
	struct dir_entry e;

	while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) {
		dir->pos += sizeof e;
		if (e.in_use) {
			strlcpy (name, e.name, NAME_MAX + 1);
			return true;
		}
	}
	return false;
}

#ifdef EFILESYS
bool dir_isempty(struct dir* dir){
	struct dir_entry e;
	off_t ofs;
	for (ofs = 2 * sizeof e; inode_read_at(dir->inode, &e, sizeof e, ofs) == sizeof e; ofs += sizeof e){
		if (e.in_use){
			return false;
		}
	}
	return true;
}
bool do_chdir(const char* dir){
	//parse the directory path.
	struct dir* search_dir;
	int l = strlen(dir);
	if(l == 0) return false;
	if(l == 1 && dir[0] == '/'){	//SPECIAL CASE : setting cwd to the ROOT.
		dir_close(thread_current()->current_dir);
		thread_current()->current_dir = dir_open_root();
		return true;
	}
	char* path_name = malloc((l + 1) * sizeof(char));
	char* dir_name = malloc(15 * sizeof(char));
	struct inode* inode;
	strlcpy(path_name, dir, (l + 1));
	search_dir = parse_path(path_name, dir_name);
	if(search_dir == NULL)
		return false;
	if(!dir_lookup(search_dir, dir_name, &inode))
		return false;
	//check if inode is a directory. if not, fail.
	if(!inode_isdir(inode))
		return false;
	//get the directory. close the current directory & change.
	struct dir* target = dir_open(inode);
	dir_close(thread_current()->current_dir);
	thread_current()->current_dir = target;
	free(path_name);
	free(dir_name);
	return true;
}
bool do_mkdir(const char* dir){
	//parse the directory path.
	struct dir* search_dir;
	int l = strlen(dir);
	if(l == 0) return false;
	char* path_name = malloc((l + 1) * sizeof(char));
	char* dir_name = malloc(15 * sizeof(char));
	strlcpy(path_name, dir, (l + 1));
	search_dir = parse_path(path_name, dir_name);
	if(search_dir == NULL){
		printf("parsing failed!\n");
		return false;
	}
	cluster_t cluster;
	bool success = (fat_allocate(1, &cluster) 
			&& dir_create(cluster_to_sector(cluster), 16) 
			&& dir_add(search_dir, dir_name, cluster_to_sector(cluster)));
	free(path_name);
	free(dir_name);
	return success;
}
bool do_readdir(struct dir* dir, char* name){
	if(dir == NULL)
		return false;
	if(dir->pos < 2 * sizeof(struct dir_entry)){
		dir->pos = 2 * sizeof(struct dir_entry);
	}
	bool result = dir_readdir (dir, name);
	return result;
}
#endif
