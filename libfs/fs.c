#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "disk.h"
#include "fs.h"

#define FAT_EOC 0xffff

typedef enum {false, true} bool;

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
static struct FD *fdtable;
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
	//check if num_blks_fat = ceil((num_data_blks*2)/BLOCK_SIZE)
	if(superblock->num_blks_fat != ((superblock->num_data_blks * 2) / 
		BLOCK_SIZE) + (((superblock->num_data_blks * 2) % BLOCK_SIZE) != 0))
		return -1;

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

	//allocate and reset fd table
	fdtable = calloc(FS_OPEN_MAX_COUNT, sizeof(struct FD));

	fsmounted = true;

	return 0;
}

int fs_umount(void)
{
	//error check
	if(!fsmounted || fd_open > 0) return -1;

	//save disk and close
	//dont need to write back superblock because we didnt change it

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
	free(fdtable);

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
	printf("fat_free_ratio=%d/%d\n", num_fat_free_entries, 
	superblock->num_data_blks);

	int num_rootdir_free_entries = 0;
	for(int i = 0; i < FS_FILE_MAX_COUNT; i++)
	{
		//free entry in root dir if first char of filename is null
		if(rootdir[i].filename[0] == '\0') num_rootdir_free_entries++;
	}
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
	strcpy((char*)rootdir[first_available_index].filename, filename);
	rootdir[first_available_index].size_file_bytes = 0;
	rootdir[first_available_index].index_first_data_blk = FAT_EOC;

	if(block_write(superblock->root_dir_blk_index, rootdir) == -1) return -1;
	
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

	//find file in root dir
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
	rootdir[i].index_first_data_blk = '\0';

	//clean file's contents in FAT
	while(index_cur_data_blk != FAT_EOC)
	{
		uint16_t index_next_data_blk = fat[index_cur_data_blk].value;
		fat[index_cur_data_blk].value = 0;
		index_cur_data_blk = index_next_data_blk;
	}

	//write back FAT, which starts at block index 1 in disk
	for(size_t i = 1, j = 0; i < superblock->root_dir_blk_index; i++, j++)
	{
		if(block_write(i, (void*)fat + j * BLOCK_SIZE) == -1) return -1;
	}

	//write back root dir
	if(block_write(superblock->root_dir_blk_index, rootdir) == -1) return -1;

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
			strcpy((char*)fdtable[fd].filename, filename);
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
		|| offset > (size_t)fs_stat(fd)
		|| fdtable[fd].filename[0] == '\0') return -1;

	//set new offset
	fdtable[fd].offset = offset;

	return 0;
}

//phase 4

//---start of helper functions
uint16_t index_data_blk(uint16_t data_start_index , size_t file_offset)
{
	//index of data blk according to offset and the start index
	//update data_start_index using fat entry pointers
	//Example: we read 1st block if offset 4095 and 2nd block if offset 4096
	int num_blk_skip = file_offset / BLOCK_SIZE;
	while(num_blk_skip > 0 && data_start_index != FAT_EOC)
	{
		data_start_index = fat[data_start_index].value;
		num_blk_skip--;
	}

	return data_start_index;
}

uint16_t allocate_new_data_blk()
{
	//allocate the first avaliable fat entry and data block
	//note claiming fat entry 0 or data blk 0 is not allowed by disk format
	for(uint16_t fat_index = 1; fat_index < superblock->num_data_blks
		; fat_index++)
	{
		if(fat[fat_index].value == 0)
		{
			fat[fat_index].value = FAT_EOC;
			return fat_index;
		}
	}

	//else failed to allocate a new data blk
	//note again claiming data blk 0 is illegal by disk format
	//so this will be our error flag
	return 0;
}
//---end of helper functions

int fs_write(int fd, void *buf, size_t count)
{
	//validation
	if (!fsmounted || fd < 0 || fd >= FS_OPEN_MAX_COUNT || 
		fdtable[fd].filename[0] == '\0' || buf == NULL) return -1;

	if(count == 0) return 0; //user input want to write nothing

	//prep
	//find file entry in root dir for changing file size if necessary
	struct RootDirEntry *rootdirentry = NULL;
	for(int i = 0; i < FS_FILE_MAX_COUNT; i++)
	{
		if(strcmp((char*)rootdir[i].filename, 
		fdtable[fd].filename) == 0)
		{
			rootdirentry = &rootdir[i];
		}
	}
	size_t offset = fdtable[fd].offset;

	//allocate more blks if necessary
	if(offset + count > rootdirentry->size_file_bytes)
	{
		//empty file gets its own first block
		if(rootdirentry->index_first_data_blk == FAT_EOC)
		{
			uint16_t new_blk_index = allocate_new_data_blk();
			if(new_blk_index == 0) return 0;	//no space on disk to write
			rootdirentry->index_first_data_blk = new_blk_index;
		}
		uint16_t data_index = rootdirentry->index_first_data_blk;

		//ceiling function from geeks for geeks
		int blocks_want = ((count + offset) / BLOCK_SIZE) 
			+ (((count + offset) % BLOCK_SIZE) != 0);

		//reaching here means atleast 1 blk is allocated to file
		//so we can move forward one blk
		blocks_want--;

		while(blocks_want > 0)
		{
			if(fat[data_index].value == FAT_EOC)
			{
				uint16_t new_blk_index = allocate_new_data_blk();
				if(new_blk_index == 0) break; //no more blocks to allocate
				fat[data_index].value = new_blk_index;
			}
			data_index = fat[data_index].value;
			blocks_want--;
		}
	}
	
	//move to the correct blk based on file's offset
	uint16_t file_data_blk_idex = 
		index_data_blk(rootdirentry->index_first_data_blk, offset);	
	//not enough blocks allocated to write starting from offset
	if(file_data_blk_idex == FAT_EOC) return 0;
	//special case left index for writing first block
	int left = offset % BLOCK_SIZE;
	int amount_to_write_in_blk;
	size_t bytes_wrote = 0;
	//bounce buffer with index 0 to 4095
	uint8_t bounce_buffer[BLOCK_SIZE];
	while(file_data_blk_idex != FAT_EOC && count > 0)
	{
		if(left + count > BLOCK_SIZE)
		{
			amount_to_write_in_blk = BLOCK_SIZE - left;
		}
		else	//special case for writing last blk or total one blk
		{
			//write blk from index left or 0 up to remaining count bytes
			amount_to_write_in_blk = count;
		}

		if(amount_to_write_in_blk < BLOCK_SIZE)
		{
			//for writing the first and last block, there are cases when we 
			//want to retain information that is already written to it because 
			//we dont want to overwrite the entire block. EX: file size 4096 
			//and offset is at middle of file and we write 1 byte. Only that 1 
			//byte should change in the file's contents and nothing else.
			block_read(superblock->data_blk_start_index + file_data_blk_idex
			, (void*)bounce_buffer);
			memcpy(bounce_buffer + left, buf, amount_to_write_in_blk);
			block_write(superblock->data_blk_start_index + file_data_blk_idex
			, (void*)bounce_buffer);
		}
		else 
		{
			//otherwise, we overwrite the entire block for blocks that are 
			//neither the first block or the last block
			block_write(superblock->data_blk_start_index + file_data_blk_idex
			, (void*)buf);
		}

		//move start position in input buffer for next blk write
		buf += amount_to_write_in_blk;	
		bytes_wrote += amount_to_write_in_blk;
		count -= amount_to_write_in_blk;
		
		//move to writing next blk
		file_data_blk_idex = fat[file_data_blk_idex].value;

		left = 0; //for subsequent blks other than first blk, start at index 0
	}

	//Example: file size 1 and offset currently at 0
	//write 1 byte wont change size but write 2 byte will change size
	if(offset + bytes_wrote > rootdirentry->size_file_bytes)
	{
		rootdirentry->size_file_bytes = offset + bytes_wrote;
	}

	fdtable[fd].offset += bytes_wrote;
	return bytes_wrote;
}

int fs_read(int fd, void *buf, size_t count)
{
	//validation
	if (!fsmounted || fd < 0 || fd >= FS_OPEN_MAX_COUNT || 
		fdtable[fd].filename[0] == '\0' || buf == NULL) return -1;

	if(count == 0) return 0; //user input want to read nothing

	//prep
	uint32_t file_size = fs_stat(fd);

	if(file_size == 0) return 0; //nothing to read for a empty file

	size_t offset = fdtable[fd].offset;

	//Example: file size 1 then should only read index 0
	//read nothing if offset is set beyond file's contents
	//...user should be writing instead to extend the file
	if(offset + 1 > file_size)	return 0;

	//Example: file size 1 then should only read index 0
	//reduce count to readable number of bytes left
	//if count > readable number of bytes left
	//file_size - offset is readable number of bytes left
	if(count > file_size - offset) count = file_size - offset;

	//otherwise, valid for reading so
	//get index of first data block in data array according to offset
	uint16_t data_start_index = 0;
	for (int i = 0; i < FS_FILE_MAX_COUNT; i++)
	{
		if(strcmp((char*)rootdir[i].filename, fdtable[fd].filename) == 0)
		{
			data_start_index = rootdir[i].index_first_data_blk;
			break;
		}
	}
	//move to the correct blk based on file's offset
	uint16_t file_data_blk_idex = index_data_blk(data_start_index, offset);	
	//special case left index for reading first block
	int left = offset % BLOCK_SIZE;
	int amount_to_read_in_blk;
	size_t bytes_read = count;
	//bounce buffer with index 0 to 4095
	uint8_t bounce_buffer[BLOCK_SIZE];
	while(count > 0)	//while not done reading
	{
		if(left + count > BLOCK_SIZE)
		{
			amount_to_read_in_blk = BLOCK_SIZE - left;
		}
		else	//special case for reading last blk or total one blk
		{
			//read blk from index left or 0 up to remaining count bytes
			amount_to_read_in_blk = count;
		}

		if(amount_to_read_in_blk < BLOCK_SIZE)
		{
			//for cases where we only want to read subset of the first block
			//and last block
			block_read(superblock->data_blk_start_index + file_data_blk_idex
			, (void*)bounce_buffer);
			memcpy(buf, bounce_buffer + left, amount_to_read_in_blk);
		}
		else
		{
			//otherwise, we read the entire block for blocks that are 
			//neither the first block or the last block
			block_read(superblock->data_blk_start_index + file_data_blk_idex
			, (void*)buf);
		}

		//move start position in input buffer for next blk read
		buf += amount_to_read_in_blk;	
		count -= amount_to_read_in_blk;
		
		//move to reading next blk
		file_data_blk_idex = fat[file_data_blk_idex].value;

		left = 0; //for subsequent blks other than first blk, start at index 0
	}

	fdtable[fd].offset += bytes_read;
	return bytes_read;
}
