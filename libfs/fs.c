#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "disk.h"
#include "fs.h"

#define FAT_EOC 0xFFFF

typedef enum {false, true} bool;

int DEBUG = 0; //enable/disable printf statements

void fs_print(char* str, int num)
{
	if (DEBUG) printf("===%s: %d===\n", str, num);
	else if (str || num) return; //avoid compiler error
}

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

struct FATEntry
{
	uint16_t value; //can be 0/num/FAT_EOC
} __attribute__((__packed__));

struct RootDirEntry
{
	uint8_t filename[FS_FILENAME_LEN];
	uint32_t size_file_bytes;
	uint16_t index_first_data_blk;
	uint8_t padding[10];
} __attribute__((__packed__));

struct FD	//packed not needed because this info is not written to disk
{
	char filename[FS_FILENAME_LEN];
	size_t offset;	//offset can not be negative
};

static struct Superblock *superblock;
static struct FATEntry *fat;
static struct RootDirEntry *rootdir;
static struct FD fdtable[FS_OPEN_MAX_COUNT];
static int fd_open;
static bool fsmounted;	//boolean; either one fs is mounted or none

//phase 1

int fs_mount(const char *diskname)
{
	if(diskname == NULL || fsmounted) return -1;

	if(block_disk_open(diskname) == -1) return -1;

	//map or mount superblock
	superblock = malloc(BLOCK_SIZE);
	if(block_read(0, superblock) == -1) return -1;

	//validate disk 
	//validate superblock
	if(memcmp(superblock->signature, "ECS150FS", 8) != 0) 
		return -1; //signature
	if(1 + superblock->num_blks_fat + 1 + superblock->num_data_blks
		!= superblock->num_blks_vd) return -1;	//block amount
	if(superblock->num_blks_vd != block_disk_count()) return -1; //block amount

	//validate FAT using ceiling function
	//https://www.geeksforgeeks.org/find-ceil-ab-without-using-ceil-function/
	//check if num_blks_fat == ceil((num_data_blks*2)/BLOCK_SIZE)
	//ceilVal = (a+b-1)/b, a=num_data_blks*2, b=BLOCK_SIZE, divide b by 2
	if(superblock->num_blks_fat != (superblock->num_data_blks * 2 + BLOCK_SIZE
		- 1)/(BLOCK_SIZE)) return -1;

	//validate disk order
	if(1 + superblock->num_blks_fat != superblock->root_dir_blk_index)
		return -1;	//root index
	if(superblock->root_dir_blk_index + 1 != superblock->data_blk_start_index)
		return -1; //first data index

	//map or mount FAT; 4096 bytes * num FAT blocks
	//a different procedure because fat is not one block like the others
	fat = malloc(BLOCK_SIZE * superblock->num_blks_fat); 
	//copy block by block
	for(size_t i = 1, j = 0; i < superblock->root_dir_blk_index; i++, j++)
	{
		if(block_read(i, (void*)fat + j * BLOCK_SIZE) == -1) return -1;
	}

	//validate Fat array
	if(fat[0].value != FAT_EOC) return -1;

	//map or mount root dir
	rootdir = malloc(BLOCK_SIZE);
	if(block_read(superblock->root_dir_blk_index, rootdir) == -1) return -1;

	//reset fd table
	memset(fdtable, 0, sizeof(fdtable));

	fsmounted = true;

	return 0;
}

int fs_umount(void)
{
	//error check
	if(!fsmounted || fd_open > 0) return -1;

	//save disk and close
	//write back superblock
	if(block_write(0, superblock) == -1) return -1;

	//write back FAT, which starts at block index 1 in disk
	for(size_t i = 1, j = 0; i < superblock->root_dir_blk_index; i++, j++)
	{
		if(block_write(i, (void*)fat + j * BLOCK_SIZE) == -1) return -1;
	}

	//write back root dir
	if(block_write(superblock->root_dir_blk_index, rootdir) == -1) return -1;

	if(block_disk_close() == -1) return -1;

	free(superblock);
	free(fat);
	free(rootdir);

	fsmounted = false;

	return 0;
}

int fs_info(void)
{
	if(!fsmounted) return -1;

	printf("FS Info:\n");
	printf("total_blk_count=%i\n", superblock->num_blks_vd);
	printf("fat_blk_count=%i\n", superblock->num_blks_fat);
	printf("rdir_blk=%i\n", superblock->root_dir_blk_index);
	printf("data_blk=%i\n", superblock->data_blk_start_index);
	printf("data_blk_count=%i\n", superblock->num_data_blks);

	int num_fat_free_entries = 0;
	//can skip first FAT_EOC
	for(uint16_t i = 1; i < superblock->num_data_blks; i++)	
	{
		//free entry in FAT if value is 0
		if(fat[i].value == 0) num_fat_free_entries++;
	}

	int num_rootdir_free_entries = 0;
	for(int i = 0; i < FS_FILE_MAX_COUNT; i++)
	{
		//free entry in root dir if first char of filename is null
		if(rootdir[i].filename[0] == '\0') num_rootdir_free_entries++;
	}

	printf("fat_free_ratio=%d/%d\n", num_fat_free_entries, 
		superblock->num_data_blks);
	printf("rdir_free_ratio=%d/%d\n", num_rootdir_free_entries,
		FS_FILE_MAX_COUNT);
	
	return 0;
}

//phase 2

int fs_create(const char *filename)
{
	if(!fsmounted || filename == NULL || strlen(filename) + 1
		> FS_FILENAME_LEN) return -1;

	int num_files = 0, first_available_index = -1;	//-1 for invalid

	for(int i = 0; i < FS_FILE_MAX_COUNT; i++)
	{	//need to check every entry before we decide
		//filename already exists
		if(strcmp((char*)rootdir[i].filename, filename) == 0) return -1;
		if(rootdir[i].filename[0] != '\0')
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
	memcpy(rootdir[first_available_index].filename, filename
		, FS_FILENAME_LEN);
	rootdir[first_available_index].size_file_bytes = 0;
	rootdir[first_available_index].index_first_data_blk = FAT_EOC;
	
	return 0;
}

int fs_delete(const char *filename)
{
	if(!fsmounted || filename == NULL || strlen(filename) + 1
		> FS_FILENAME_LEN) return -1;

	//check if the file is currently open in fd_table
	for(int i = 0; i < FS_OPEN_MAX_COUNT; i++)
	{
		if(strcmp(fdtable[i].filename, filename) == 0)
		{
			return -1;
		}
	}

	int i = 0;
	bool found_file = false;
	for(; i < FS_FILE_MAX_COUNT; i++)
	{
		if(strcmp((char*)rootdir[i].filename, filename) == 0)
		{
			found_file = true;
			break;
		}
	}
	//file not found
	if(found_file == false) return -1;

	//otherwise, clean file's contents in root dir and FAT
	uint16_t index_cur_data_blk = rootdir[i].index_first_data_blk;

	//clean file's contents in root dir
	rootdir[i].filename[0] = '\0';

	//clean file's contents in FAT
	while(index_cur_data_blk != FAT_EOC)
	{
		uint16_t index_next_data_blk = fat[index_cur_data_blk].value;
		fat[index_cur_data_blk].value = 0;
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
		if(rootdir[i].filename[0] != '\0')
		{
			printf("file: %s, size: %i, data_blk: %i\n", 
			rootdir[i].filename, rootdir[i].size_file_bytes
			, rootdir[i].index_first_data_blk);
		}
	}

	return 0;
}

//phase 3

int fs_open(const char *filename)
{
	//validation
	if(!fsmounted || filename == NULL || strlen(filename) + 1
		> FS_FILENAME_LEN || fd_open == FS_OPEN_MAX_COUNT) return -1;

	//validate file is already created in root dir
	bool file_created = false;
	for(int i = 0; i < FS_FILE_MAX_COUNT; i++)
	{
		if(strcmp((char*)rootdir[i].filename, filename) == 0) //equal
		{
			file_created = true;
			break;
		}
	}
	if(file_created == false) return -1;

	//get an empty fd
	int fd = 0;
	for(; fd < FS_OPEN_MAX_COUNT; fd++)
	{
		//apply same concept for when a entry is empty in root dir
		if(fdtable[fd].filename[0] == '\0')
		{
			memcpy(fdtable[fd].filename, filename, FS_FILENAME_LEN);
			fdtable[fd].offset = 0;
			fd_open++;
			break;
		}
	}
	
	return fd;
}

int fs_close(int fd)
{
	//validation
	if(!fsmounted || fd < 0 || fd >= FS_OPEN_MAX_COUNT 
		|| fdtable[fd].filename[0] == '\0') return -1;

	//otherwise, safe to close fd and reset it for another file
	fdtable[fd].filename[0] = '\0';
	fd_open--;

	return 0;
}

int fs_stat(int fd)
{
	//validation
	if(!fsmounted || fd < 0 || fd >= FS_OPEN_MAX_COUNT 
		|| fdtable[fd].filename[0] == '\0') return -1;

	//otherwise, return file size
	uint32_t file_size = -1;
	for(int i = 0; i < FS_FILE_MAX_COUNT; i++)
	{
		if(strcmp((char*)rootdir[i].filename, 
		fdtable[fd].filename) == 0)
		{
			file_size = rootdir[i].size_file_bytes;
		}
	}

	return file_size;
}

int fs_lseek(int fd, size_t offset)
{
	//validation
	if(!fsmounted || fd < 0 || fd >= FS_OPEN_MAX_COUNT 
		|| offset > (size_t)fs_stat(fd)) return -1;

	//set new offset
	fdtable[fd].offset = offset;

	return 0;
}

//phase 4

int fs_write(int fd, void *buf, size_t count)
{
	fs_print("WRITE", 0);
	
	//validation
	if (!fsmounted || fd<0 || fd>=FS_OPEN_MAX_COUNT || fdtable[fd].filename[0] == '\0' || buf == NULL) return -1;
	
	//in fdtable, get initial offset
	size_t offset = fdtable[fd].offset;
	fs_print("offset", offset);
	
	//in rootdir, get initial fat_idx
	size_t rootdir_fat_idx = 0;
	size_t rootdir_i = 0;
	size_t fat_idx = 0; //to be changed
	for (int i=0; i<FS_FILE_MAX_COUNT; ++i)
	{
		if(!strcmp((char*)rootdir[i].filename, (char*)fdtable[fd].filename)) //equal
		{
			rootdir_i = i; //for curr file entry in rootdir
			rootdir_fat_idx = rootdir[i].index_first_data_blk;
			fat_idx = rootdir_fat_idx;
			break;
		}
	}
	fs_print("fat_idx", fat_idx);
	
	//in FAT, get vars
	int first_blk = 1; //bool
	size_t curr = 0;
	
	size_t fat_idx_offset = fat_idx; //to be changed
	size_t num_data_blks = 0;
	size_t num_data_blks_used = 0; //before the offset block
	
	while (fat_idx != FAT_EOC)
	{
		if (first_blk && offset > curr+BLOCK_SIZE)
		{
			curr += BLOCK_SIZE;
			++num_data_blks_used;
		}
		else if (first_blk)
		{
			fat_idx_offset = fat_idx;
			first_blk = 0;
			
			fs_print("- fat_idx", fat_idx);
			fs_print("- fat_idx_offset", fat_idx_offset);
		}
		++num_data_blks;
		fat_idx = fat[fat_idx].value;
	}
	
	fs_print("fat_idx_offset", fat_idx_offset);
	fs_print("num_data_blks", num_data_blks);
	fs_print("num_data_blks_used", num_data_blks_used);
	
	//vars
	first_blk = 1; //reset
	fat_idx = fat_idx_offset;
	uint8_t data[num_data_blks*BLOCK_SIZE+count]; //<=buf, =>disk //need to iterate byte by byte
	size_t data_idx = 0;

	//in FAT, write to data
	if (fat_idx == FAT_EOC) //edge case
	{
		memcpy(&data[data_idx], buf, count); //copy C
		data_idx += count;
		
		if (DEBUG) for (int j=0; j<10; ++j) fs_print("z", (uint8_t)data[j]);
		if (DEBUG) for (int j=0; j<10; ++j) fs_print("a", ((uint8_t*)buf)[j]);
	}
	
	while (fat_idx != FAT_EOC) //get all data
	{
		//0. get data in DB, copy to bounce_buf
		//uint8_t bounce_buf[BLOCK_SIZE]; //Each block is 4096 bytes //need to iterate byte by byte
		//block_read(2+superblock->num_blks_fat+fat_idx, &bounce_buf);
		
		//1. copy to data, start at offset if first_blk
		if (first_blk) //ABDE --> AB C DE
		{
			//1.1 get data in DB, copy to bounce_buf
			uint8_t bounce_buf[BLOCK_SIZE]; //Each block is 4096 bytes //need to iterate byte by byte
			block_read(2+superblock->num_blks_fat+fat_idx, &bounce_buf);
			
			memcpy(&data[data_idx], &bounce_buf[data_idx], offset); //copy AB
			data_idx += offset;
		
			memcpy(&data[data_idx], buf, count); //copy C
			data_idx += count; //don't delete +1 //I guess it's for the NULL byte / EOF
			//deleted +1 b/c now it works
			//https://stackoverflow.com/questions/12389518/representing-eof-in-c-code
			//https://www.tutorialspoint.com/c_standard_library/c_function_memcpy.htm
			//also for memset and memcmp
			
			
			//only need to overwrite
			//memcpy(&data[data_idx], &bounce_buf[offset], BLOCK_SIZE-offset); //copy DE
			//data_idx += BLOCK_SIZE-offset;
			
			first_blk = 0;
			
			if (DEBUG) for (int j=0; j<10; ++j) fs_print("x", data[j]);
			fs_print((char*)bounce_buf, 0);
			fs_print((char*)buf, 0);
			fs_print((char*)data, 0);
		}
		else //ABCD --> ABDE
		{
			//memcpy(&data[data_idx], &bounce_buf, BLOCK_SIZE); //copy ABDE
			block_read(2+superblock->num_blks_fat+fat_idx, &data[data_idx]); //copy ABDE, no need for bounce_buf 
			data_idx += BLOCK_SIZE;
			
			if (DEBUG) for (size_t j=data_idx; j<data_idx+10; ++j) fs_print("y", data[j]);
		}
		
		fat_idx = fat[fat_idx].value;
	} //end while
	fs_print("end while", 0);
	fs_print("data_idx", data_idx);
	fs_print("fat_idx_offset == FAT_EOC", fat_idx_offset == FAT_EOC);

	//2. copy to DB (block write back), allocate new space if needed (check disk space) + change FAT
	//in FAT, write to DB
	size_t i=0;
	size_t j=0;
	size_t prev_fat_idx;
	(rootdir_fat_idx == FAT_EOC) ? (prev_fat_idx = rootdir_fat_idx) : (prev_fat_idx = fat_idx_offset); //edge case //don't delete ()
	fat_idx = fat_idx_offset;
	for (; i<data_idx; i+=BLOCK_SIZE, ++num_data_blks_used, prev_fat_idx=fat_idx, fat_idx=fat[fat_idx].value) //try to write all data
	{
		fs_print("prev_fat_idx", prev_fat_idx);
		fs_print("fat_idx", fat_idx);

		//check space
		if (num_data_blks_used >= num_data_blks) //need to allocate new space + change FAT
		//same as if (fat_idx == FAT_EOC)
		{
			//in FAT array, find first-fit
			int space_allocated = 0;
			for (j=0; j<superblock->num_data_blks; ++j) //num of FAT *entries* == superblock->num_data_blks
			{
				if (fat[j].value == 0) //empty
				{
					//now fat_idx == FAT_EOC
					//change rootdir if needed //edge case
					fs_print("change root/fat", 0);
					(rootdir_fat_idx == FAT_EOC) ? (rootdir[rootdir_i].index_first_data_blk = j) : (fat[prev_fat_idx].value = j); //instead of FAT_EOC
					fat_idx = j; //don't delete
					fs_print("changed root/fat", 1);
					fat[j].value = FAT_EOC;
					space_allocated = 1;
					
					fs_print("j", j);
					break;
				}
			}
			if (!space_allocated) //not enough space
			{
				fs_print("no space", 0);
				rootdir[rootdir_i].size_file_bytes = i;
				fdtable[fd].offset = i;
				if (i >= offset+count) return count;
				else return i-offset;
			}
		}

		fs_print("WRITE BACK", 0);
		fs_print("superblock->num_blks_fat", superblock->num_blks_fat);
		fs_print("fat_idx", fat_idx);
		fs_print("sum", 2+superblock->num_blks_fat+fat_idx);
		
		//write back to disk
		block_write(2+superblock->num_blks_fat+fat_idx, &data[i]);
		
		fs_print("fat_idx", fat_idx);
		fs_print("fat[fat_idx].value", fat[fat_idx].value);
		fs_print("data[i]", data[i]);
	} //end for loop
	fs_print("end for loop", 0);
	
	fs_print("count", count);
	rootdir[rootdir_i].size_file_bytes += count; //change file size
	fdtable[fd].offset = i; //change file offset
	return count;
}

int fs_read(int fd, void *buf, size_t count)
{
	//validation
	if (!fsmounted || fd < 0 || fd >= FS_OPEN_MAX_COUNT || 
		fdtable[fd].filename[0] == '\0' || buf == NULL) return -1;
	
	//in fdtable, get offset
	size_t offset = fdtable[fd].offset;
	
	//in rootdir, get fat_idx
	uint16_t fat_idx = 0;
	size_t file_size = 0;
	for (int i = 0; i < FS_FILE_MAX_COUNT; ++i)
	{
		if(!strcmp((char*)rootdir[i].filename, (char*)fdtable[fd].filename)) //equal
		{
			fat_idx = rootdir[i].index_first_data_blk;
			file_size = rootdir[i].size_file_bytes;
			break;
		}
	}
	
	//vars
	bool first_blk = true; //bool
	uint8_t data[count]; //=>buf //need to iterate byte by byte
	size_t data_idx = 0;

	//in FAT
	size_t curr = 0;
	while (fat_idx != FAT_EOC)
	{
		//check offset //may not be in the first block
		if (first_blk && offset > curr + BLOCK_SIZE)
		{
			curr += BLOCK_SIZE;
			fat_idx = fat[fat_idx].value;
			continue;
		}
		
		//1. get data in DB, copy to bounce_buf
		//uint8_t bounce_buf[BLOCK_SIZE]; //Each block is 4096 bytes //need to iterate byte by byte
		//block_read(2 + superblock->num_blks_fat + fat_idx, bounce_buf);
			
		//2. copy to data, start at offset if first_blk, end at count
		if (first_blk)
		{
			uint8_t bounce_buf[BLOCK_SIZE]; //Each block is 4096 bytes //need to iterate byte by byte
			block_read(2 + superblock->num_blks_fat + fat_idx, bounce_buf);
			
			//https://stackoverflow.com/questions/17638730/are-multiple-conditions-allowed-in-a-for-loop
			for (; data_idx < BLOCK_SIZE - offset && data_idx < count && offset + data_idx < file_size;
			     	++data_idx)
					memcpy(&data[data_idx], &bounce_buf[offset+data_idx], 1);
			first_blk = false;
		}
		else
		{
			if (data_idx+BLOCK_SIZE < count && offset + data_idx+BLOCK_SIZE < file_size) //middle blks
			{
				//no need for bounce_buf
				block_read(2 + superblock->num_blks_fat + fat_idx, &data[data_idx]);
				data_idx += BLOCK_SIZE;
			}
			else //last_blk
			{
				uint8_t bounce_buf[BLOCK_SIZE]; //Each block is 4096 bytes //need to iterate byte by byte
				block_read(2 + superblock->num_blks_fat + fat_idx, bounce_buf);
				
				for (int i = 0; i < BLOCK_SIZE && data_idx < count && offset + data_idx < file_size; 
				++data_idx, ++i)
					memcpy(&data[data_idx], &bounce_buf[i], 1);
			}
		}
		
		if (data_idx >= count) break; //ends early
		fat_idx = fat[fat_idx].value;
	}
	
	//3. copy to buf
	memcpy(buf, data, data_idx); //memcpy(void *dest, const void *src, size_t n)
	fdtable[fd].offset += data_idx;
	return data_idx;
}
