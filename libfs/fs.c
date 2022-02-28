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
	char filename[FS_FILENAME_LEN];
	size_t offset;	//offset can not be negative
};

struct FDTable
{
	struct FD fdarray[FS_OPEN_MAX_COUNT];
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
	//a different procedure because fat is not one block like the others
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

	//reset fd table
	memset(&fdtable, 0, sizeof(fdtable));

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

int fs_delete(const char *filename)
{
	if(!fsmounted || filename == NULL || strlen(filename) + 1
		> FS_FILENAME_LEN) return -1;

	//check if the file is currently open in fd_table
	for(int i = 0; i < FS_OPEN_MAX_COUNT; i++)
	{
		if(strcmp(fdtable.fdarray[i].filename, filename) == 0)
		{
			return -1;
		}
	}

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
	//validation
	if(!fsmounted || filename == NULL || strlen(filename) + 1
		> FS_FILENAME_LEN || fdtable.fd_open == FS_OPEN_MAX_COUNT) return -1;

	//validate file is already created in root dir
	int file_created = 0;
	for(int i = 0; i < FS_FILE_MAX_COUNT; i++)
	{
		if(strcmp((char*)rootdir.array[i].filename, filename) == 0) //equal
		{
			file_created = 1;
			break;
		}
	}
	if(file_created == 0) return -1;

	//get an empty fd
	int fd = 0;
	for(; fd < FS_OPEN_MAX_COUNT; fd++)
	{
		//apply same concept for when a entry is empty in root dir
		if(fdtable.fdarray[fd].filename[0] == '\0')
		{
			memcpy(fdtable.fdarray[fd].filename, filename, FS_FILENAME_LEN);
			fdtable.fdarray[fd].offset = 0;
			fdtable.fd_open++;
			break;
		}
	}
	
	return fd;
}

int fs_close(int fd)
{
	//validation
	if(!fsmounted || fd < 0 || fd >= FS_OPEN_MAX_COUNT 
		|| fdtable.fdarray[fd].filename[0] == '\0') return -1;

	//otherwise, safe to close fd and reset it for another file
	fdtable.fdarray[fd].filename[0] = '\0';
	fdtable.fd_open--;

	return 0;
}

int fs_stat(int fd)
{
	//validation
	if(!fsmounted || fd < 0 || fd >= FS_OPEN_MAX_COUNT 
		|| fdtable.fdarray[fd].filename[0] == '\0') return -1;

	//otherwise, return file size
	uint32_t file_size = -1;
	for(int i = 0; i < FS_FILE_MAX_COUNT; i++)
	{
		if(strcmp((char*)rootdir.array[i].filename, 
		fdtable.fdarray[fd].filename) == 0)
		{
			file_size = rootdir.array[i].size_file_bytes;
		}
	}

	return file_size;
}

int fs_lseek(int fd, size_t offset)
{
	//validation
	if(!fsmounted || fd < 0 || fd >= FS_OPEN_MAX_COUNT 
		|| offset > (size_t)fs_stat(fd)) return -1;

	//reset the offset
	fdtable.fdarray[fd].offset = offset;

	return 0;
}

int fs_write(int fd, void *buf, size_t count)
{
	if(fd || buf || count) return -1;
	return 0;
	/* TODO: Phase 4 */
}

int fs_read(int fd, void *buf, size_t count)
{
	//validation
	if (!fsmounted || fd<0 || fd>=FS_OPEN_MAX_COUNT || fdtable.fdarray[fd].filename[0] == '\0' || buf == NULL) return -1;
	
	//vars
	int first_blk = 1; //bool
	uint8_t data[count]; //=buf //need to iterate byte by byte
	size_t data_idx = 0;
	
	//in fd_table, get offset
	size_t offset =  fdtable.fdarray[fd].offset;
	
	//in rootdir, get fat_idx
	size_t fat_idx = 0;
	for(int i=0; i<FS_FILE_MAX_COUNT; ++i)
	{
		if(!strcmp((char*)rootdir.array[i].filename, (char*)fdtable.fdarray[fd].filename)) //equal
		{
			fat_idx = rootdir.array[i].index_first_data_blk;
			break;
		}
	}

	//in FAT
	size_t curr = 0;
	while (fat_idx != FAT_EOC)
	{
		//check offset //may not be in the first block
		if (first_blk && offset > curr+BLOCK_SIZE)
		{
			curr += BLOCK_SIZE;
			fat_idx = fat.array[fat_idx];
			continue;
		}
		
		//get data in DB, copy to bounce_buf
		uint8_t bounce_buf[BLOCK_SIZE]; //Each block is 4096 bytes //need to iterate byte by byte
		block_read(2+superblock.num_blks_fat+fat_idx, bounce_buf);
			
		//copy to data, start at offset if first_blk, end at count
		if (first_blk)
		{
			//https://stackoverflow.com/questions/17638730/are-multiple-conditions-allowed-in-a-for-loop
			for (; data_idx<BLOCK_SIZE-offset && data_idx<count; ++data_idx)
				memcpy(&data[data_idx], &bounce_buf[offset+data_idx], 1);
			first_blk = 0;
		}
		else
		{
			for (int i=0; i<BLOCK_SIZE && data_idx<count; ++data_idx, ++i)
				memcpy(&data[data_idx], &bounce_buf[i], 1);
		}
			
		if (data_idx >= count) break; //ends early
		fat_idx = fat.array[fat_idx];
	}
	
	//copy to buf
	memcpy(buf, (void*)data, data_idx); //memcpy(void *dest, const void *src, size_t n)
	fdtable.fdarray[fd].offset += data_idx;
	return data_idx;
}