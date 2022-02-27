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

struct FD	//packed not needed because this info is not written to disk
{
	uint8_t filename[FS_FILENAME_LEN];
	size_t offset;
};

struct FDTable
{
	struct FD fd[FS_OPEN_MAX_COUNT];
	int fd_open; //max is FS_OPEN_MAX_COUNT or 32
};

static struct Superblock superblock;
static struct FAT fat;
static struct Rootdir rootdir;
static struct FDTable fdtable;
static int fsmounted;	//boolean; either one fs is mounted or none

int fs_mount(const char *diskname)
{
	if(diskname == NULL) return -1;

	if(block_disk_open(diskname) == -1) return -1;

	//map or mount superblock
	if(block_read(0, &superblock) == -1) return -1;

	//validate disk 
	//validate superblock
	if(memcmp(superblock.signature, "ECS150FS", sizeof(superblock.signature)) 
		!= 0) return -1; //signature
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

	//map or mount FAT; 4096 bytes * num FAT blocks
	fat.array = (uint16_t*)malloc(sizeof(uint16_t) * 2048 
		* superblock.num_blks_fat);
	void *buf = (void*)malloc(BLOCK_SIZE);
	//copy block by block
	for(size_t i = 1, j = 0; i < superblock.root_dir_blk_index; i++, j++)
	{
		if(block_read(i, buf) == -1) return -1;
		memcpy(fat.array + j * BLOCK_SIZE, buf, BLOCK_SIZE);
	}
	free(buf);

	//validate Fat array
	if(fat.array[0] != FAT_EOC) return -1;

	//map or mount root dir
	if(block_read(superblock.root_dir_blk_index, &rootdir) == -1) return -1;

	fdtable.fd_open = 0;

	fsmounted = 1;

	return 0;
}

int fs_umount(void)
{
	//error check
	if(!fsmounted || fdtable.fd_open > 0) return -1;

	//save disk and close
	//write back superblock
	if(block_write(0, &superblock) == -1) return -1;

	//write back FAT
	for(size_t i = 1, j = 0; i < superblock.root_dir_blk_index; i++, j++)
	{
		if(block_write(i, fat.array + j * BLOCK_SIZE) == -1) return -1;
	}

	//write root dir
	if(block_write(superblock.root_dir_blk_index, &rootdir) == -1) return -1;

	if(block_disk_close() == -1) return -1;

	free(fat.array);	//free and reset fat array for reuse

	fsmounted = 0;

	return 0;
}

int fs_info(void)
{
	if(!fsmounted) return -1;

	printf("FS Info:\n");
	printf("total_blk_count=%i\n", superblock.num_blks_vd);
	printf("fat_blk_count=%i\n", superblock.num_blks_fat);
	printf("rdir_blk=%i\n", superblock.root_dir_blk_index);
	printf("data_blk=%i\n", superblock.data_blk_start_index);
	printf("data_blk_count=%i\n", superblock.num_data_blks);

	int num_fat_free_entries = 0;
	for(int i = 0; i < superblock.num_data_blks; i++)
	{
		//free entry in FAT if value is 0
		if(fat.array[i] == 0) num_fat_free_entries++;
	}

	int num_rootdir_free_entries = 0;
	for(int i = 0; i < FS_FILE_MAX_COUNT; i++)
	{
		//free entry in root dir if first char of filename is null
		if(rootdir.array[i].filename[0] == '\0') num_rootdir_free_entries++;
	}

	printf("fat_free_ratio=%d/%d\n", num_fat_free_entries, 
		superblock.num_data_blks);
	printf("rdir_free_ratio=%d/%d\n", num_rootdir_free_entries,
		FS_FILE_MAX_COUNT);
	
	return 0;
}

int fs_create(const char *filename)
{
	if(!fsmounted || filename == NULL || strlen(filename) + 1
		> FS_FILENAME_LEN) return -1;

	int num_files = 0, first_available_index = -1;	//-1 for invalid

	for(int i = 0; i < FS_FILE_MAX_COUNT; i++)
	{	///need to check every entry before we decide
		//filename already exists
		if(strcmp((char*)rootdir.array[i].filename, filename) == 0) return -1;
		if(rootdir.array[i].filename[0] != '\0')
		{
			num_files++;
		}
		else if(first_available_index == -1)
		{
			first_available_index = i;
		}
	}
	//root directory already has 128 files
	if(num_files == FS_FILE_MAX_COUNT) return -1;

	//else, safe to create this new file in root dir
	memcpy(rootdir.array[first_available_index].filename, filename
		, FS_FILENAME_LEN);
	rootdir.array[first_available_index].size_file_bytes = 0;
	rootdir.array[first_available_index].index_first_data_blk = FAT_EOC;
	
	return 0;
}

int fs_delete(const char *filename)	//IDK how check file is currently open???
{
	if(!fsmounted || filename == NULL || strlen(filename) + 1
		> FS_FILENAME_LEN) return -1;

	int i = 0, found_file = 0;

	for(; i < FS_FILE_MAX_COUNT; i++)
	{
		if(strcmp((char*)rootdir.array[i].filename, filename) == 0)
		{
			found_file = 1;
			break;
		}
	}
	//file not found
	if(found_file == 0) return -1;

	//otherwise, clean file's contents in root dir and FAT
	uint16_t index_cur_data_blk;
	index_cur_data_blk = rootdir.array[i].index_first_data_blk;

	//clean file's contents in root dir
	rootdir.array[i].filename[0] = '\0';
	rootdir.array[i].size_file_bytes = 0;
	rootdir.array[i].index_first_data_blk = FAT_EOC;

	//clean file's contents in FAT
	while(index_cur_data_blk != FAT_EOC)
	{
		uint16_t index_next_data_blk = fat.array[index_cur_data_blk];
		fat.array[index_cur_data_blk] = 0;
		index_cur_data_blk = index_next_data_blk;
	}

	return 0;
}

int fs_ls(void)
{
	if(!fsmounted) return -1;

	printf("FS Ls:\n");

	for(int i = 0; i < FS_FILE_MAX_COUNT; i++)
	{
		if(rootdir.array[i].filename[0] != '\0')
		{
			printf("file: %s, size: %i, data_blk: %i\n", 
			rootdir.array[i].filename, rootdir.array[i].size_file_bytes
			, rootdir.array[i].index_first_data_blk);
		}
	}

	return 0;
}

int fs_open(const char *filename)
{
	if(filename) return -1;
	return 0;
	/* TODO: Phase 3 */
}

int fs_close(int fd)
{
	if(fd) return -1;
	return 0;
	/* TODO: Phase 3 */
}

int fs_stat(int fd)
{
	if(fd) return -1;
	return 0;
	/* TODO: Phase 3 */
}

int fs_lseek(int fd, size_t offset)
{
	if(fd || offset) return -1;
	return 0;
	/* TODO: Phase 3 */
}

int fs_write(int fd, void *buf, size_t count)
{
	if(fd || buf || count) return -1;
	return 0;
	/* TODO: Phase 4 */
}

int fs_read(int fd, void *buf, size_t count)
{
	if(fd || buf || count) return -1;
	return 0;
	/* TODO: Phase 4 */
}