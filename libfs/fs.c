#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "disk.h"
#include "fs.h"

#define FAT_EOC 0xFFFF

struct Superblock //unsigned specs
{
	//1 byte = 8 bits
	//signedness and bit size
	//https://www.gnu.org/software/libc/manual/html_node/Integers.html
	uint8_t signature[8]; //ECS150FS
	uint16_t num_blks_vd;
	uint16_t root_dir_blk_index;
	uint16_t data_blk_start_index;
	uint16_t num_data_blks;
	uint8_t num_blks_fat;
	uint8_t padding[4079];
} __attribute__((__packed__)); //avoid the compiler to interfere with their layout

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
	struct Entry entries[FS_FILE_MAX_COUNT];
} __attribute__((__packed__));

static struct Superblock superblock;
static struct FAT fat;
static struct Rootdir rootdir;

int fs_mount(const char *diskname)
{
	//check valid disk 
	if(block_disk_open(diskname) == -1) return -1;

	//map or mount superblock
	if(block_read(0, &superblock) == -1) return -1;

	//check valid superblock
	if(memcmp(superblock.signature, "ECS150FS", 8) != 0) return -1; //signature
	if(1 + superblock.num_blks_fat + 1 + superblock.num_data_blks
		!= superblock.num_blks_vd) return -1; //block amount
	if(superblock.num_blks_vd != block_disk_count()) return -1; //block amount
	
	//check valid FAT
	//https://www.geeksforgeeks.org/find-ceil-ab-without-using-ceil-function/
	//ceil((num_data_blks*2)/BLOCK_SIZE)
	//ceilVal = (a+b-1) / b, a=num_data_blks*2, b=BLOCK_SIZE, divide b by 2
	if(superblock.num_blks_fat != (superblock.num_data_blks*2 + (BLOCK_SIZE/2)
		- 1)/(BLOCK_SIZE/2)) return -1;
	if(superblock.num_blks_fat + 1 != superblock.root_dir_blk_index) 
		return -1; //root index
	if(superblock.root_dir_blk_index + 1 != superblock.data_blk_start_index)
		return -1; //first data index
	
	fat.array = (uint16_t*)malloc(num_blks_fat*BLOCK_SIZE); //num_blks_fat is not known at compile time, so it cannot be static

	//check valid root dir

	//mounted superblock
	return 0;
}

int fs_umount(void)
{
	/* TODO: Phase 1 */
}

int fs_info(void)
{
	/* TODO: Phase 1 */
}

int fs_create(const char *filename)
{
	/* TODO: Phase 2 */
	data_blk_start_index = FAT_EOC;
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
