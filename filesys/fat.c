#include "filesys/fat.h"
#include "devices/disk.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "filesys/inode.h"
#include "filesys/directory.h"

/* Should be less than DISK_SECTOR_SIZE */
struct fat_boot {
	unsigned int magic;
	unsigned int sectors_per_cluster; /* Fixed to 1 */
	unsigned int total_sectors;
	unsigned int fat_start;
	unsigned int fat_sectors; /* Size of FAT in sectors. */
	unsigned int root_dir_cluster;
};

/* FAT FS */
struct fat_fs {
	struct fat_boot bs;
	unsigned int *fat;
	unsigned int fat_length;
	disk_sector_t data_start;
	cluster_t last_clst;
	struct lock write_lock;
};

static struct fat_fs *fat_fs;

void fat_boot_create (void);
void fat_fs_init (void);

void
fat_init (void) {
	fat_fs = calloc (1, sizeof (struct fat_fs));
	if (fat_fs == NULL)
		PANIC ("FAT init failed");

	// Read boot sector from the disk
	unsigned int *bounce = malloc (DISK_SECTOR_SIZE);
	if (bounce == NULL)
		PANIC ("FAT init failed");
	disk_read (filesys_disk, FAT_BOOT_SECTOR, bounce);
	memcpy (&fat_fs->bs, bounce, sizeof (fat_fs->bs));
	free (bounce);

	// Extract FAT info
	if (fat_fs->bs.magic != FAT_MAGIC)
		fat_boot_create ();
	fat_fs_init ();
}

void
fat_open (void) {
	fat_fs->fat = calloc (fat_fs->fat_length, sizeof (cluster_t));
	if (fat_fs->fat == NULL)
		PANIC ("FAT load failed");

	// Load FAT directly from the disk
	uint8_t *buffer = (uint8_t *) fat_fs->fat;
	off_t bytes_read = 0;
	off_t bytes_left = sizeof (fat_fs->fat);
	const off_t fat_size_in_bytes = fat_fs->fat_length * sizeof (cluster_t);
	for (unsigned i = 0; i < fat_fs->bs.fat_sectors; i++) {
		bytes_left = fat_size_in_bytes - bytes_read;
		if (bytes_left >= DISK_SECTOR_SIZE) {
			disk_read (filesys_disk, fat_fs->bs.fat_start + i,
			           buffer + bytes_read);
			bytes_read += DISK_SECTOR_SIZE;
		} else {
			uint8_t *bounce = malloc (DISK_SECTOR_SIZE);
			if (bounce == NULL)
				PANIC ("FAT load failed");
			disk_read (filesys_disk, fat_fs->bs.fat_start + i, bounce);
			memcpy (buffer + bytes_read, bounce, bytes_left);
			bytes_read += bytes_left;
			free (bounce);
		}
	}
}

void
fat_close (void) {
	// Write FAT boot sector
	uint8_t *bounce = calloc (1, DISK_SECTOR_SIZE);
	if (bounce == NULL)
		PANIC ("FAT close failed");
	memcpy (bounce, &fat_fs->bs, sizeof (fat_fs->bs));
	disk_write (filesys_disk, FAT_BOOT_SECTOR, bounce);
	free (bounce);

	// Write FAT directly to the disk
	uint8_t *buffer = (uint8_t *) fat_fs->fat;
	off_t bytes_wrote = 0;
	off_t bytes_left = sizeof (fat_fs->fat);
	const off_t fat_size_in_bytes = fat_fs->fat_length * sizeof (cluster_t);
	for (unsigned i = 0; i < fat_fs->bs.fat_sectors; i++) {
		bytes_left = fat_size_in_bytes - bytes_wrote;
		if (bytes_left >= DISK_SECTOR_SIZE) {
			disk_write (filesys_disk, fat_fs->bs.fat_start + i,
			            buffer + bytes_wrote);
			bytes_wrote += DISK_SECTOR_SIZE;
		} else {
			bounce = calloc (1, DISK_SECTOR_SIZE);
			if (bounce == NULL)
				PANIC ("FAT close failed");
			memcpy (bounce, buffer + bytes_wrote, bytes_left);
			disk_write (filesys_disk, fat_fs->bs.fat_start + i, bounce);
			bytes_wrote += bytes_left;
			free (bounce);
		}
	}
}

void
fat_create (void) {
	// Create FAT boot
	fat_boot_create ();
	fat_fs_init ();

	// Create FAT table
	fat_fs->fat = calloc (fat_fs->fat_length, sizeof (cluster_t));
	if (fat_fs->fat == NULL)
		PANIC ("FAT creation failed");

	// Set up ROOT_DIR_CLST
	fat_put (ROOT_DIR_CLUSTER, EOChain);

	// Fill up ROOT_DIR_CLUSTER region with 0
	uint8_t *buf = calloc (1, DISK_SECTOR_SIZE);
	if (buf == NULL)
		PANIC ("FAT create failed due to OOM");
	disk_write (filesys_disk, cluster_to_sector (ROOT_DIR_CLUSTER), buf);
	free (buf);
	
	// Make a ROOT_DIR inode. inode is stored at ROOT_DIR_CLUSTER! initial size : 16 entries.
	if (!dir_create (cluster_to_sector (ROOT_DIR_CLUSTER), 16))
		PANIC ("ROOT_DIR inode creation failed!!");
}

void
fat_boot_create (void) {
	unsigned int fat_sectors =
	    (disk_size (filesys_disk) - 1)
	    / (DISK_SECTOR_SIZE / sizeof (cluster_t) * SECTORS_PER_CLUSTER + 1) + 1;
	fat_fs->bs = (struct fat_boot){
	    .magic = FAT_MAGIC,
	    .sectors_per_cluster = SECTORS_PER_CLUSTER,
	    .total_sectors = disk_size (filesys_disk),
	    .fat_start = 1,
	    .fat_sectors = fat_sectors,
	    .root_dir_cluster = ROOT_DIR_CLUSTER,
	};
}

void
fat_fs_init (void) {
	/* TODO: Your code goes here. */
	/*
	fat_fs->bs.{total_sectors, fat_start, fat_sectors}
	fat_fs->{fat, fat_length, data_start, last_clst, write_lock}
	*/
	fat_fs->fat_length = fat_fs->bs.total_sectors - fat_fs->bs.fat_sectors - 1;		//how many clusters in the File system?
	fat_fs->data_start = fat_fs->bs.fat_start + fat_fs->bs.fat_sectors;		//start of DATA section.
	fat_fs->last_clst = fat_fs->fat_length - 1;
	lock_init(&fat_fs->write_lock);
}

/*----------------------------------------------------------------------------*/
/* FAT handling                                                               */
/*----------------------------------------------------------------------------*/

/* NEW FUNCTION : Allocate a new cluster -> traverse and find first entry with stored value 0. */
static unsigned int alloc_cluster (void){
	unsigned int* fat = fat_fs->fat;	//fat table. index : 1 ~ last_clst.
	unsigned int last_clst = fat_fs->last_clst;
	unsigned int idx = 1;
	while(idx <= last_clst){
		cluster_t clst = fat[idx];
		if(clst == 0){		//0 = UNUSED, so allocate this.
			return idx;	//return the allocated cluster Number!
		}
		idx++;
	}
	return 0;	//No clusters to allocate, so return 0(UNUSED).
}

/* Add a cluster to the chain.
 * If CLST is 0, start a new chain.
 * Returns 0 if fails to allocate a new cluster. */
cluster_t
fat_create_chain (cluster_t clst) {
	/* TODO: Your code goes here. */
	unsigned int* fat = fat_fs->fat;
	lock_acquire(&fat_fs->write_lock);	//use Synchronization when modifying FAT!!
	unsigned int new_clst = alloc_cluster();
	if(new_clst == 0){
		lock_release(&fat_fs->write_lock);
		return 0;
	}
	
	if(clst == 0){				//start a new chain.
		fat[new_clst] = EOChain;
	}
	else{					//extend existing chain.
		fat[clst] = new_clst;
		fat[new_clst] = EOChain;
	}
	lock_release(&fat_fs->write_lock);
	return (cluster_t) new_clst;
}

/* Remove the chain of clusters starting from CLST.
 * If PCLST is 0, assume CLST as the start of the chain. */
void
fat_remove_chain (cluster_t clst, cluster_t pclst) {
	/* TODO: Your code goes here. */
	unsigned int* fat = fat_fs->fat;
	lock_acquire(&fat_fs->write_lock);
	unsigned int cur_clst = clst;
	while(cur_clst != EOChain){
		unsigned int next_clst = fat[cur_clst];
		fat[cur_clst] = 0;		//EMPTY this cluster.
		cur_clst = next_clst;		//If this is EOC, end.
	}
	if(pclst != 0 && fat[pclst] == clst){	//set pclst as EOC.
		fat[pclst] = EOChain;
	}
	lock_release(&fat_fs->write_lock);
}

/* Update a value in the FAT table. */
void
fat_put (cluster_t clst, cluster_t val) {
	/* TODO: Your code goes here. */
	unsigned int* fat = fat_fs->fat;
	lock_acquire(&fat_fs->write_lock);
	fat[clst] = val;
	lock_release(&fat_fs->write_lock);
}

/* Fetch a value in the FAT table. */
cluster_t
fat_get (cluster_t clst) {
	/* TODO: Your code goes here. */
	cluster_t result;
	unsigned int* fat = fat_fs->fat;
	lock_acquire(&fat_fs->write_lock);
	result = fat[clst];
	lock_release(&fat_fs->write_lock);
	return result;
}

/* Covert a cluster # to a sector number. */
disk_sector_t
cluster_to_sector (cluster_t clst) {
	/* TODO: Your code goes here. */
	//Translation : cluster# = disk# - C (constant).
	ASSERT(clst > 0);
	disk_sector_t data_start = fat_fs->data_start;
	//printf("cluster# = %d -> sector# = %d\n", clst, clst + data_start);
	return clst + data_start;
}

/* Convert sector # to cluster #. */
cluster_t sector_to_cluster (disk_sector_t sector){
	disk_sector_t data_start = fat_fs->data_start;
	//printf("sector# = %d -> cluster# = %d\n", sector, sector - data_start);
	return (cluster_t) (sector - data_start);
}

/* Traverse FAT to retrieve the N-th sector of the file. */
disk_sector_t fat_traverse(cluster_t start, unsigned int n){
	unsigned int* fat = fat_fs->fat;
	cluster_t clst = start;
	unsigned int i;
	for(i=0; i<n; i++){
		clst = fat[clst];
		if(clst == EOChain || clst == 0)
			return -1;
	}
	return cluster_to_sector(clst);
}

/* EXTENSIBLE FILES : Traverse FAT. Each time an EOF is reached, extend cluster & ZERO the disk region. */
disk_sector_t fat_traverse_extended(cluster_t start, unsigned int n){
	unsigned int* fat = fat_fs->fat;
	cluster_t clst = start;
	printf("start : %d, N : %d\n",start,n);
	unsigned int i;
	for(i=0; i<n; i++){
		cluster_t next_clst = fat[clst];
		printf("next_clst = 0x%X\n",next_clst);
		if(next_clst == 0)
			return -1;
		if(next_clst == EOChain){	//EOF -> extend chain & ZERO the disk region & update file length.
			next_clst = fat_create_chain(clst);
			printf("EXTENDED clst : %d -> Next_clst : %d\n", clst, next_clst);
			if(next_clst == 0)
				return -1;
			/* ZERO the disk region. */
			uint8_t *buf = calloc(1, DISK_SECTOR_SIZE);
			ASSERT(buf != NULL);
			disk_write (filesys_disk, cluster_to_sector(next_clst), buf);
			free (buf);
		}
		clst = next_clst;
	}
	return cluster_to_sector(clst);
}

/* Allocate a CNT sized NEW Chain of clusters, and save the starting cluster to clusterp. */
bool fat_allocate(size_t cnt, cluster_t *clusterp){
	*clusterp = 0;
	//if(cnt == 0)	return true;
	cluster_t start = fat_create_chain(0);		//Allocate first cluster.
	if(start == 0) return false;
	if(cnt == 0){
		*clusterp = start;
		printf("making file of size 0, starting cluster : %d\n", *clusterp);
		return;
	}
	cluster_t clst = start;
	size_t i;
	for(i=0; i<cnt-1; i++){				//Allocate the next (cnt-1) clusters.
		clst = fat_create_chain(clst);
		if(clst == 0){				//Failed allocation, so do free().
			fat_remove_chain(start, 0);
			return false;
		}
	}
	*clusterp = start;
	return true;
}

/* Free a CNT sized Chain. */
void fat_release(cluster_t cluster, size_t cnt){
	fat_remove_chain(cluster, 0);
}

