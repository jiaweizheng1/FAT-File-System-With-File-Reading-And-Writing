#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "disk.h"
#include "fs.h"

#define FAT_EOC 0xFFFF

struct Superblock	//unsigned specs
{
	uint8_t signature[8]; //ECS150FS
	uint16_t num_blks_vd;
	uint16_t root_dir_blk_index;
	uint16_t data_blk_start_index;
	uint16_t num_data_blks;
	uint8_t num_blks_fat;
	uint8_t padding[4079];
} __attribute__((__packed__));

struct FAT
{
	uint16_t *array;
} __attribute__((__packed__));

struct Entry
{
	uint8_t filename[FS_FILENAME_LEN];
	uint32_t size_file_bytes;
	uint16_t index_first_data_blk;
	uint8_t padding[10];
} __attribute__((__packed__));

struct Rootdir 
{
	struct Entry array[FS_FILE_MAX_COUNT];
} __attribute__((__packed__));

struct File
{
	uint8_t filename[FS_FILENAME_LEN];
	size_t offset;
};

struct FDTable
{
	struct File file[FS_OPEN_MAX_COUNT];
	int fd_open; //max is FS_OPEN_MAX_COUNT 32
};

static struct Superblock superblock;
static struct FAT fat;
static struct Rootdir rootdir;
static struct FDTable fdtable;
static int fsmounted;	//boolean; either one fs is mounted or none

int fs_mount(const char *diskname)
{
	if(block_disk_open(diskname) == -1) return -1;

	//map or mount superblock
	if(block_read(0, &superblock) == -1) return -1;

	//validate disk 
	//validate superblock
	if(memcmp(superblock.signature, "ECS150FS", 8) != 0) return -1; //signature
	if(1 + superblock.num_blks_fat + 1 + superblock.num_data_blks
		!= superblock.num_blks_vd) return -1;	//block amount
	if(superblock.num_blks_vd != block_disk_count()) return -1;	//block amount

	//validate FAT using ceiling function
	//https://www.geeksforgeeks.org/find-ceil-ab-without-using-ceil-function/
	if(superblock.num_blks_fat != (superblock.num_data_blks * 2 + BLOCK_SIZE
		- 1)/(BLOCK_SIZE)) return -1;

	//validate disk order
	if(1 + superblock.num_blks_fat != superblock.root_dir_blk_index)
		return -1;	//root index
	if(superblock.root_dir_blk_index + 1 != superblock.data_blk_start_index)
		return -1; //first data index

	//map or mount FAT; 4096 bytes * num fat blocks
	fat.array = (uint16_t*)malloc(sizeof(uint16_t) * 2048 
		* superblock.num_blks_fat);
	void *buf = (void*)malloc(sizeof(BLOCK_SIZE));
	//copy block by block
	for(size_t i = 1, j = 0; i < superblock.root_dir_blk_index; i++, j++)
	{
		if(block_read(i, buf) == -1) return -1;
		memcpy(fat.array + j * BLOCK_SIZE, buf, BLOCK_SIZE);
	}
	free(buf);	//no longer needed

	//validate Fat array
	if(fat.array[0] != FAT_EOC) return -1;

	//map or mount Root dir
	if(block_read(superblock.root_dir_blk_index, &rootdir) == -1) return -1;

	fdtable.fd_open = 0;

	fsmounted = 1;

	return 0;
}

int fs_umount(void)
{
	//error check open fds
	if(!fsmounted || fdtable.fd_open > 0) return -1;

	//save disk and close
	if(block_write(0, &superblock) == -1) return -1;

	void *buf = (void*)malloc(sizeof(BLOCK_SIZE));
	for(size_t i = 1, j = 0; i < superblock.root_dir_blk_index; i++, j++)
	{
		if(block_write(i, fat.array + j * BLOCK_SIZE) == -1) return -1;
	}
	free(buf); //no longer needed

	if(block_write(superblock.root_dir_blk_index, &rootdir) == -1) return -1;

	if(block_disk_close() == -1) return -1;

	free(fat.array);	//reset fat.array for new mounted disk

	fsmounted = 0;

	return 0;
}

int fs_info(void)
{
	if(!fsmounted) return -1;

	printf("__FS's Superblock info__\n");
	printf("Num blks fat: %i\n", superblock.num_blks_fat);
	printf("Root dir index: %i\n", superblock.root_dir_blk_index);
	printf("Data blk start index: %i\n", superblock.data_blk_start_index);
	printf("Num blks data: %i\n", superblock.num_data_blks);
	printf("Total blocks: %i\n", superblock.num_blks_vd);

	int num_fat_free_entries = 0;
	for(int i = 0; i < 2048 * superblock.num_blks_fat; i++)
	{
		if(fat.array[i] == 0) num_fat_free_entries++;
	}

	int num_rootdir_free_entries = 0;
	for(int i = 0; i < FS_FILE_MAX_COUNT; i++)
	{
		if(rootdir.array[i].filename[0] == '\0') num_rootdir_free_entries++;
	}

	printf("fat_free_ratio: %d", num_fat_free_entries/(2048 
		* superblock.num_blks_fat));
	printf("rootdir_free_ratio: %d", num_rootdir_free_entries
		/FS_FILE_MAX_COUNT);
	printf("data_free_ratio: %d", 1 - (superblock.num_data_blks 
		- num_fat_free_entries)/(superblock.num_data_blks));
	
	return 0;
}

int fs_create(const char *filename)
{
	/* TODO: Phase 2 */
}

int fs_delete(const char *filename)
{
	/* TODO: Phase 2 */
}

int fs_ls(void)
{
	/* TODO: Phase 2 */
}

int fs_open(const char *filename)
{
	/* TODO: Phase 3 */
}

int fs_close(int fd)
{
	/* TODO: Phase 3 */
}

int fs_stat(int fd)
{
	/* TODO: Phase 3 */
}

int fs_lseek(int fd, size_t offset)
{
	/* TODO: Phase 3 */
}

int fs_write(int fd, void *buf, size_t count)
{
	/* TODO: Phase 4 */
}

int fs_read(int fd, void *buf, size_t count)
{
	/* TODO: Phase 4 */
}