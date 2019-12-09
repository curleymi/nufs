/*
 *  Michael Curley
 *  cs3650
 *  ch03
 *
 *  notes:
 *   - directories are automatically BLOCK_SIZE on creation
 *   - mode and permissions match sys/stat defs
 *   - directories save data about its contents by path item, followed by null
 *     character, followed by a uint8_t offset to its inode
 *   - when navigating a path, the program always searches in root first
 *   - example: search for /dir/file.txt
 *      - search for "dir" in root
 *      - on found, get inode, it is the first uint8_t byte after the null
 *        terminator for the string "dir\0"
 *      - search in dir's inode blocks for "file.txt"
 *      - on found set inode's offset for calling function
 *   - based on cs3650 course code
 */

#define _GNU_SOURCE

#include "storage.h"
#include "bitmap.h"
#include "path.h"

#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

// -------------------------- GLOBAL VARIABLES --------------------------

// the disk file's file descriptor
static int        g_Disk_FD =       -1;

// the base of the disk from mmap
static void*      g_Disk_Base =      0;

// the first slot of the data block bitmap
static uint8_t*   g_Block_Bitmap =   0;

// the first slot of the inode bitmap
static uint8_t*   g_Inode_Bitmap =   0;

// the first slot of the inode structures
static inode_t*   g_Inode_Base =     0;

// the first slot of the data blocks
static void*      g_Block_Base =     0;



// -------------------------- CONSTANTS ---------------------------------

// the flag used to tell if the 'disk' file was previously initialized
const uint8_t c_Init_Flag = 0x99;

// the mask to allign a pointer to a 4k BLOCK_SIZE
const uint64_t c_Block_Mask = 0xFFFFFFFFFFFFF000;



// -------------------------- NUFS SIMILAR FUNCTIONS --------------------

// recursively searches the given delimited path, sets the inode offset pointer
// to the inode associated with the path
int search(const char* path, uint8_t* inode_i) {
    // the path has been fully searched, return success
    if (strlen(path) == 0) {
        return 0;
    }

    // get the inode confirm it is a directory, if it is not there is a bug
    inode_t* inode = get_inode(*inode_i);
    assert((mode_t)(inode->mode & S_IFDIR) == S_IFDIR);

    // get the data blocks for the directory
    uint8_t* blocks = get_blocks(*inode_i);
    void* block;

    // loop over all data blocks
    for (int i = 0; i < inode->block_count; i++) {
        // get the current data block
        block = get_block(blocks[i]);

        // loop until last directory item is found
        while (strlen(block)) {
            // set the inode to the current item's inode
            *inode_i = *(uint8_t*)(block + strlen(block) + 1);
            
            // if the current item if the item of interest
            if (strcmp(block, path) == 0) {
                // recursively search that inode's data for the next path item
                return search(path + strlen(path) + 1, inode_i);
            }

            // continue to the next item in the block
            block += strlen(block) + 2;
        }
    }

    // if the full loops above successfully finish, there is no entry
    return -ENOENT;
}

// accesses the given path's inode
// note: inode_i can be null if just checking the item exists
int storage_access(const char* path, uint8_t* inode_i) {
    // delimit the path and set the search to start in root (0)
    uint8_t path_inode = 0;
    char* path_d = delimit_path(path);

    // launch the search algorithm
    int rv = search(path_d, &path_inode);

    // free the delimited path
    free_delimited_path(path_d);

    // set the inode pointer if not null
    if (inode_i != 0) {
        *inode_i = path_inode;
    }

    return rv;
}

// truncates the given inode's size
int storage_truncate(off_t size, uint8_t inode_i) {
    // get the inode and set the number of blocks needed for the new size
    inode_t* inode = get_inode(inode_i);
    int blocks_needed = (size / BLOCK_SIZE) + (size % BLOCK_SIZE == 0 ? 0 : 1);

    // assume success
    int rv = 0;

    // only need to alloc/free blocks if the counts are different
    if (blocks_needed != inode->block_count) {
        // get the inodes data block offsets
        uint8_t* blocks = get_blocks(inode_i);
        
        // free blocks for other file data
        if (blocks_needed < inode->block_count) {
            // loop over all unneeded blocks and update the bitmap
            for (int i = blocks_needed; i < inode->block_count; i++) {
                bitmap_set(g_Block_Bitmap, 0, blocks[i], BITMAP_SIZE);
            }

            // if the block was previously using an indirect offset, switch to
            // direct block offfsets if possible
            if (blocks_needed <= DIRECT_BLOCK_COUNT && inode->block_count > DIRECT_BLOCK_COUNT) {
                // copy all block offsets to the direct array
                for (int i = 0; i < blocks_needed; i++) {
                    inode->d_blocks[i] = blocks[i];
                }

                // the indirect block offset cannot be 0, 0 is always the root
                assert(inode->i_block != 0);
                
                // update the indirect block as free
                bitmap_set(g_Block_Bitmap, 0, inode->i_block, BITMAP_SIZE);
            }
        }
        // allocate more blocks
        else {
            // get the number of new blocks needed and allocate array to hold
            // the new offsets
            int new_blocks_count = blocks_needed - inode->block_count;
            uint8_t* new_blocks = malloc(new_blocks_count * sizeof(uint8_t));
            uint8_t block_pointer_switch = 0;
            
            // allocate a block for every new block needed
            for (int i = 0; i < new_blocks_count; i++) {
                // alloc the block
                rv = bitmap_next(g_Block_Bitmap, BITMAP_SIZE);
                new_blocks[i] = (uint8_t)rv;
                
                // on success update bitmap so the next free block can be
                // obtained
                if (rv >= 0) {
                    bitmap_set(g_Block_Bitmap, 1, new_blocks[i], BITMAP_SIZE);

                    // set rv to succes for when loop ends
                    rv = 0;
                }
                // on failure do not attempt to allocate more blocks
                else {
                    break;
                }
            }

            // if successful in allocating all needed blocks
            if (rv == 0) {
                // if a direct offsets are being used and indirect blocks are
                // needed, allocate another block to hold the indirect offsets
                if (inode->block_count <= DIRECT_BLOCK_COUNT && blocks_needed > DIRECT_BLOCK_COUNT) {
                    if ((rv = bitmap_next(g_Block_Bitmap, BITMAP_SIZE)) >= 0) {
                        // on success set the indirect block and update the
                        // bitmap
                        inode->i_block = (uint8_t)rv;

                        // copy the old blocks to the new indirect offset block
                        uint8_t* i_blocks = get_block(inode->i_block);
                        for (int i = 0; i < inode->block_count; i++) {
                            i_blocks[i] = blocks[i];
                        }

                        // the block offsets pointer is now the newly allocated
                        // block, set block pointer switch flag is true, update
                        // bitmap
                        bitmap_set(g_Block_Bitmap, 1, inode->i_block, BITMAP_SIZE);
                        block_pointer_switch = 1;
                        blocks = i_blocks;
                        rv = 0;
                    }
                }

                // on success of allocation of new blocks and/or switch to
                // indirect block
                if (rv == 0) {
                    // set the new blocks in the inode's block array, blocks
                    // variable has been updated to point to the indirect block
                    // memory if a switch had occurred
                    for (int i = 0; i < new_blocks_count; i++) {
                        blocks[i + inode->block_count] = new_blocks[i];
                    }

                    // allocation success
                    rv = 0;
                }
            }

            // on failure free all the blocks
            if (rv != 0) {
                int i = 0;

                // free all allocated blocks
                while (new_blocks[i] >= 0 && i < new_blocks_count) {
                    bitmap_set(g_Block_Bitmap, 0, new_blocks[i++], BITMAP_SIZE);
                }

                // if the block pointer switch was a success, free indirect
                // block
                if (block_pointer_switch) {
                    bitmap_set(g_Block_Bitmap, 0, inode->i_block, BITMAP_SIZE);
                }
            }

            // free the new blocks array
            free(new_blocks);
        }
    }
    
    // on success update inode's size and block count
    if (rv == 0) {
        inode->block_count = blocks_needed;
        inode->size = size;
    }

    return rv;
}

// reads len bytes of data from the given path at the given offset
int storage_read(const char* path, char* data, size_t len, off_t offset) {
    uint8_t inode_i;

    // get access to the path's inode
    int rv = storage_access(path, &inode_i);
    if (rv == 0) {
        // get the inode, update read len if it will exceed the file size
        inode_t* inode = get_inode(inode_i);
        if (offset + len > inode->size) {
            len = inode->size - offset;
        }

        // get the data blocks for the inode
        uint8_t* blocks = get_blocks(inode_i);

        // set the inital block based on the offset
        uint8_t current_block = (uint8_t)(offset / BLOCK_SIZE);

        // update the offset, it only matters for the first block
        offset %= BLOCK_SIZE;

        // set the bytes to read, either up until the end of the current block
        // or the total len to read
        int bytes_to_read = ((BLOCK_SIZE - offset) < len) ? (BLOCK_SIZE - offset) : len;

        // copy the block data to the data buffer
        memcpy(data, get_block(blocks[current_block++]) + offset, bytes_to_read);

        // update return value
        rv = bytes_to_read;
        
        // while the total number of bytes to read is not the length, continue
        // to read data
        while (rv != len) {
            // bytes to read is either a full block or the partial amount left
            // of the length
            bytes_to_read = (((len - rv) >= BLOCK_SIZE) ? BLOCK_SIZE : (len - rv));

            // copy the data and update the return value
            memcpy(data + rv, get_block(blocks[current_block++]), bytes_to_read);
            rv += bytes_to_read;
        }
    }

    return rv;
}

// writes len bytes of data to the data for the given path at the given offset
int storage_write(const char* path, const char* data, size_t len, off_t offset, uint8_t* inode_ret) {
    uint8_t inode_i;

    // get access to the path's inode
    int rv = storage_access(path, &inode_i);
    if (rv == 0) {
        *inode_ret = inode_i;
        inode_t* inode = get_inode(inode_i);
        
        // truncate the inode so it can actually have all bytes written, if
        // possible
        if (offset + len > inode->size) {
            printf("more space needed...\n");
            rv = storage_truncate(offset + len, inode_i);
        }

        // on success of truncate (or by default), begin reading
        if (rv == 0) {
            // get the inodes blocks
            uint8_t* blocks = get_blocks(inode_i);

            // set the current block based on the offset
            uint8_t current_block = (uint8_t)(offset / BLOCK_SIZE);

            // update the offset for the first block, it doesnt matter elsewhere
            offset %= BLOCK_SIZE;

            // bytes to write is either the rest of the first block or the total
            // length to write
            int bytes_to_write = ((BLOCK_SIZE - offset) < len) ? (BLOCK_SIZE - offset) : len;

            // copy the data from the buffer to the current block
            memcpy(get_block(blocks[current_block++]) + offset, data, bytes_to_write);

            // update the return value
            rv = bytes_to_write;
            
            // continue writing until the total number of bytes written is the
            // length
            while (rv != len) {
                // bytes to write is either the full block or the remaining
                // length bytes
                bytes_to_write = (((len - rv) >= BLOCK_SIZE) ? BLOCK_SIZE : (len - rv));

                // copy the data and update total bytes written
                memcpy(get_block(blocks[current_block++]), data + rv, bytes_to_write);
                rv += bytes_to_write;
            }
        }
    }

    return rv;
}

// unlinks a path from its inode
int storage_unlink(const char* path) {
    uint8_t inode_i;

    // get access to the path's inode
    int rv = storage_access(path, &inode_i);
    if (rv == 0) {
        // remove the path name from the directory
        if ((rv = directory_remove(path)) == 0) {
            // update the inode's link count
            inode_t* inode = get_inode(inode_i);

            // if there are no links left, free the data
            if (--inode->links == 0) {
                // get the inode's blocks, loop over all blocks and update the
                // block bitmap accordingly
                uint8_t* blocks = get_blocks(inode_i);
                for (int i = 0; i < inode->block_count; i++) {
                    bitmap_set(g_Block_Bitmap, 0, blocks[i], BITMAP_SIZE);
                }

                // if using indirect block pointer, free that block as well
                if (inode->block_count > DIRECT_BLOCK_COUNT) {
                    // the indirect block cannot be the root
                    assert(inode->i_block != 0);
                    bitmap_set(g_Block_Bitmap, 0, inode->i_block, BITMAP_SIZE);
                }

                // update the block count and free the inode from use
                inode->block_count = 0;
                bitmap_set(g_Inode_Bitmap, 0, inode_i, BITMAP_SIZE);
            }
        }
    }

    return rv;
}

// links a given path's inode to another
// 'to's inode will be the same as that of 'from'
int storage_link(const char* from, const char* to) {
    uint8_t inode_i;
    
    // get access to froms inode
    int rv = storage_access(from, &inode_i);
    if (rv == 0) {
        uint8_t inode_ip;

        // get the parent directory of to
        char* parent = parent_directory(to);

        // get access to to's parent directory
        if ((rv = storage_access(parent, &inode_ip)) == 0) {
            // add 'to' to the directory with 'from's inode offset
            rv = directory_add(to + strlen(parent) + 1, inode_ip, 0, inode_i);

            // increase the inode's link count
            inode_t* inode = get_inode(inode_i);
            inode->links++;
        }

        // free the parent
        free_parent_directory(parent);
    }

    return rv;
}

// adds a new item to the file system
int storage_mknod(const char* path, mode_t mode, uint8_t* inode_ret) {
    char* parent = parent_directory(path);
    char* new_item = strdup(path + strlen(parent) + 1);
    uint8_t inode_i;
    
    // get access to the parent directory's inode
    int rv = storage_access(parent, &inode_i);
    if (rv == 0) {
        // ensure the inode is a directory and there are search/modification
        // permissions
        inode_t* inode = get_inode(inode_i);
        if ((mode_t)(inode->mode & S_IFDIR) != S_IFDIR) {
            rv = -ENOTDIR;
        }
        else if ((mode_t)(inode->mode & S_IXUSR) != S_IXUSR) {
            rv = -EACCES;
        }
        // add the new data
        else {
            uint8_t new_inode;

            // add the new item to the parent's directory, inode is set in
            // directory_add and bitmap updated
            if ((rv = directory_add(new_item, inode_i, &new_inode, -1)) == 0) {
                // initialize the inode's data and the ret inode
                *inode_ret = new_inode;
                inode = get_inode(new_inode);
                inode->mode = mode;
                inode->links = 1;
                inode->i_block = 0;

                // if the item is a directory
                if ((mode_t)(mode & S_IFDIR) == S_IFDIR) {
                    // all directorys have an initial size of BLOCK_SIZE and one
                    // block associated with it
                    inode->size = BLOCK_SIZE;

                    // get the next block
                    rv = bitmap_next(g_Block_Bitmap, BITMAP_SIZE);
                    if ((rv = bitmap_next(g_Block_Bitmap, BITMAP_SIZE)) >= 0) {
                        // update the inode stats
                        inode->block_count = 1;
                        inode->d_blocks[0] = (uint8_t)rv;

                        // null terminate the inital block so it is empty
                        *(char*)get_block((uint8_t)rv) = 0;

                        // update the bitmap and set rv to success
                        bitmap_set(g_Block_Bitmap, 1, (uint8_t)rv, BITMAP_SIZE);
                        rv = 0;
                    }
                }
                // the item is a file, it has no data associated with it
                else {
                    inode->block_count = 0;
                    inode->size = 0;
                }
            }
        }
    }
    
    // free the parent and the item
    free_parent_directory(parent);
    free(new_item);

    return rv;
}



// -------------------------- DIRECTORY MANIPULATION FUNCTIONS ----------

// adds the given item to the parent directory
// note: if inode_new is a null pointer, the inode associated with the item will
//       be inode_to_add
//       if inode_new is a valid pointer, a new inode will be allocated and its
//       value stored in inode_new, inode_to_add is ignored in this case
int directory_add(const char* item, uint8_t inode_parent, uint8_t* inode_new, uint8_t inode_to_add) {
    // assume success and the item's inode is the inode_to_add
    int rv = 0;
    uint8_t item_inode = inode_to_add;
    
    // if non null pointer, allocate a new inode
    if (inode_new != 0) {
        rv = bitmap_next(g_Inode_Bitmap, BITMAP_SIZE);
        item_inode = (uint8_t)rv;
    }

    // on success allocation of inode or by default, add item to directory
    if (rv >= 0) {
        // get the inode pf the parent
        inode_t* inode = get_inode(inode_parent);

        // the length of the data to add is the length of the item + null char +
        // inode uint8_t + null char
        int len = strlen(item) + 3;

        // allocate and initialize the data to add
        void* data = malloc(len * sizeof(char));
        memcpy(data, item, len - 2);
        memcpy(data + len - 2, &item_inode, 1);
        memcpy(data + len - 1, "\0", 1);

        // get the inode blocks, set found flag to false
        uint8_t* blocks = get_blocks(inode_parent);
        uint8_t found = 0;
        void* block;
        int blocklen;

        // assume unsuccessful, disk quota reached
        rv = -EDQUOT;
        for (int i = 0; i < inode->block_count; i++) {
            // get the directory block and loop to the end
            block = get_block(blocks[i]);
            while ((blocklen = strlen(block))) {
                block += blocklen + 2;
            }
            
            // if space exists for the new item
            if (block + len <= get_block(blocks[i]) + BLOCK_SIZE) {
                // add the data to the directory
                memcpy(block, data, len);

                // if allocating a new inode, update the given pointer and the
                // inode bitmap
                if (inode_new) {
                    bitmap_set(g_Inode_Bitmap, 1, item_inode, BITMAP_SIZE);
                    *inode_new = item_inode;
                }

                // set success and break loop
                rv = 0;
                break;
            }
        }

        // free the directory data
        free(data);
    }

    return rv;
}

// removes an item from its directory, path is the full path of the item
int directory_remove(const char* path) {
    uint8_t inode_parent;
    char* parent = parent_directory(path);
    char* item = strdup(path + strlen(parent) + 1);

    // get access to the parent directory's inode
    int rv = storage_access(parent, &inode_parent);
    if (rv == 0) {
        // if the mode is not directory there is a bug
        inode_t* inode = get_inode(inode_parent);
        assert((mode_t)(inode->mode & S_IFDIR) == S_IFDIR);

        // assume item cannot be found
        rv = -ENOENT;

        // get the blocks and the inode for the parent
        uint8_t* blocks = get_blocks(inode_parent);
        void* next_item;
        void* block;
        int len;

        // loop over all the blocks for the inode
        for (int i = 0; i < inode->block_count; i++) {
            // get the current block and loop until the item is found
            block = get_block(blocks[i]);
            while ((len = strlen(block)) && strcmp(block, item)) {
                block += len + 2;
            }

            // if the item was found, break outer loop
            if (strcmp(block, item) == 0) {
                break;
            }
        }

        // on success finding item
        if (strcmp(block, item) == 0) {
            // shift the rest of the data down to overwrite the item
            // remove it
            void* replace = block + strlen(block) + 2;
            while ((len = strlen(replace))) {
                // copy the next item to the current items slot
                memcpy(block, replace, len + 2);
                block += len + 2;
                replace += len + 2;
            }

            // null terminate the block to indicate end of directory data
            *((char*)block) = 0;

            // set success
            rv = 0;
        }
    }

    // free the item and the parent
    free(item);
    free_parent_directory(parent);

    return rv;
}



// -------------------------- BLOCK AND INODE FUNCTIONS -----------------

// returns the inode at the given offset
inode_t* get_inode(uint8_t inode_i) {
    assert(inode_i >= 0 && inode_i < BITMAP_SIZE);
    return g_Inode_Base + inode_i;
}

// returns the pointer to the block offset array for the given inode
uint8_t* get_blocks(uint8_t inode_i) {
    inode_t* inode = get_inode(inode_i);

    // if the block count exceeds the direct block count, an indirect block is
    // in use so return that block instead of the direct block array
    return ((inode->block_count > DIRECT_BLOCK_COUNT) ?
        (uint8_t*)get_block(inode->i_block) : inode->d_blocks);
}

// gets the data block at the given offset
void* get_block(uint8_t offset) {
    assert(offset < BITMAP_SIZE && offset >= 0);
    return g_Block_Base + (offset * BLOCK_SIZE);
}



// -------------------------- INIT / DESTRUCTOR FUNCTIONS----------------

// initializes the root directory when the program starts, if it has not alread
// been initialized
void root_init() {
    uint8_t block_offset;
    uint8_t inode_offset;
    inode_t* inode;

    // first block used must be 0 for root
    int rv = bitmap_next(g_Block_Bitmap, BITMAP_SIZE);
    assert(rv == 0);
    block_offset = rv;
    
    // first inode used must be 0 for root
    rv = bitmap_next(g_Inode_Bitmap, BITMAP_SIZE);
    assert(rv == 0);
    inode_offset = rv;

    // get the inode and update its data
    inode = get_inode(inode_offset);
    inode->mode = S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
    inode->links = 1;
    inode->size = BLOCK_SIZE;
    inode->block_count = 1;
    inode->d_blocks[0] = block_offset;
    inode->i_block = 0;
    
    // update the bitmaps
    bitmap_set(g_Block_Bitmap, 1, block_offset, BITMAP_SIZE);
    bitmap_set(g_Inode_Bitmap, 1, inode_offset, BITMAP_SIZE);

    // update the init flag
    *(uint8_t*)g_Disk_Base = c_Init_Flag;
}

// opens the given path as the 'disk' for the file system
void storage_init(const char* path) {
    // open the file
    g_Disk_FD = open(path, O_CREAT | O_RDWR, 0644);
    assert(g_Disk_FD != -1);

    // truncate to disk size
    int rv = ftruncate(g_Disk_FD, DISK_SPACE);
    assert(rv == 0);

    // mmap the base
    g_Disk_Base = mmap(0, DISK_SPACE, PROT_READ | PROT_WRITE, MAP_SHARED, g_Disk_FD, 0);
    assert(g_Disk_Base != MAP_FAILED);

    // initialize the global variables...
    // block bitmap starts after the init flag
    g_Block_Bitmap = g_Disk_Base + sizeof(uint8_t);

    // inode bitmap starts bitmap bytes after the block bitmap
    g_Inode_Bitmap = g_Block_Bitmap + BITMAP_BYTES;

    // inode base starts bitmap bytes after the inode bytmap
    g_Inode_Base = (inode_t*)(g_Inode_Bitmap + BITMAP_BYTES);

    // block base starts at the first BLOCK_SIZE alligned address after the
    // inode base
    g_Block_Base = (void*)((uint64_t)&g_Inode_Base[BITMAP_SIZE] & c_Block_Mask) +
        ((uint64_t)&g_Inode_Base[BITMAP_SIZE] % BLOCK_SIZE == 0 ? 0 : BLOCK_SIZE);

    // display the map information
    printf("bitmap size:\t%d\nbitmap bytes:\t%d\ndisk base:\t%p\ndata map:\t%p\ninode map:\t%p\ninode base:\t%p\tsize:%lu\nblock base:\t%p\ndisk end:\t%p\nblock end:\t%p\n",
            BITMAP_SIZE,
            BITMAP_BYTES,
            g_Disk_Base,
            g_Block_Bitmap,
            g_Inode_Bitmap,
            g_Inode_Base,
            sizeof(inode_t),
            g_Block_Base,
            g_Disk_Base + DISK_SPACE,
            g_Block_Base + (BITMAP_SIZE * BLOCK_SIZE));
    
    // ensure the values are initialized correctly
    assert((void*)(&g_Inode_Base[BITMAP_SIZE]) <= (void*)g_Block_Base);
    assert((void*)(g_Block_Base + (BITMAP_SIZE * BLOCK_SIZE)) <= g_Disk_Base + DISK_SPACE);

    // init debug print statements
    bitmap_init_print(g_Inode_Bitmap, g_Block_Bitmap);

    // if the first byte is not the init flag, initalize the root
    if (*(uint8_t*)g_Disk_Base != c_Init_Flag) {
        root_init();
    }
}

// unmaps the disk file and closes it
void storage_free() {
    int rv = munmap(g_Disk_Base, DISK_SPACE);
    assert(rv == 0);
    rv = close(g_Disk_FD);
    assert(rv == 0);
}



// -------------------------- END OF FILE -------------------------------







