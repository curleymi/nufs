/*
 *  Michael Curley
 *  cs3650
 *  ch03
 *
 *  notes:
 *   - most of the functions loosely tanslate to the nufs functions
 *   - many functions accept a pointer to a uint8_t, this is used to set the
 *     inode value in the calling function
 *   - read the function headers in storage.c for clear information on how each
 *     function should be called
 *   - this file is large, it contains the more complex file system functions to
 *     keep nufs cleaner
 */

#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>

// the number of direct block offsets a single inode has
// currently max size of file before using indirect block is 32768 bytes
#define DIRECT_BLOCK_COUNT 8

// the block size and number of blocks for the file system
#define BLOCK_SIZE 4096
#define BLOCK_COUNT 256

// the size of the 'disk'
#define DISK_SPACE BLOCK_SIZE * BLOCK_COUNT

// the number of inodes and data blocks for the system
// chosen such that the number of blocks does not exceed the size of the disk
// after the metadata
#define BITMAP_SIZE 253

// the number of bytes associated with the bitmaps
#define BITMAP_BYTES (BITMAP_SIZE / 8) + (BITMAP_SIZE % 8 == 0 ? 0 : 1)

// the inode used for this file system
typedef struct inode_t {
    mode_t mode;                            // permissions and node type
    uint32_t links;                         // the number of links
    uint32_t size;                          // the size of its data
    uint8_t block_count;                    // the number of blocks for the inode
    uint8_t d_blocks[DIRECT_BLOCK_COUNT];   // the direct block offsets
    uint8_t i_block;                        // the indirect block offset
    time_t a_time;                          // last access time
    time_t m_time;                          // last modify time
} inode_t;

// functions closely correspond to nufs functions
int storage_access(const char* path, uint8_t* inode_i);
int storage_truncate(off_t size, uint8_t inode_i);
int storage_read(const char* path, char* data, size_t len, off_t offset);
int storage_write(const char* path, const char* data, size_t len, off_t offset, uint8_t* inode_ret);
int storage_unlink(const char* path);
int storage_link(const char* from, const char* to);
int storage_mknod(const char* path, mode_t mode, uint8_t* inode_ret);

// directory manipulation functions
int directory_add(const char* item, uint8_t inode_parent, uint8_t* inode_new, uint8_t inode_to_add);
int directory_remove(const char* path);

// get data associated with inodes and blocks
inode_t* get_inode(uint8_t inode_i);
uint8_t* get_blocks(uint8_t inode_i);
void* get_block(uint8_t offset);

// initialization and destructor functions
void storage_init(const char* path);
void storage_free();

#endif
