/*
 *  Michael Curley
 *  cs3650
 *  ch03
 */

#include "bitmap.h"

#include <errno.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>

// binary value 0b10000000 used for finding free bits
const uint8_t c_MSB_8_High = 0x80;

// constant value for a uint8_t set to zero
const uint8_t c_0x00 = 0x00;

// initialize null pointers for the different bitmaps
static uint8_t* inode_bitmap = 0x00;
static uint8_t* block_bitmap = 0x00;

// sets the pointers for the different bitmaps
void bitmap_init_print(uint8_t* inode, uint8_t* block) {
    inode_bitmap = inode;
    block_bitmap = block;
}

// prints the given bitmap
void print_bitmap(const char* title, uint8_t* bitmap, int size){
    printf("%s", title);

    // print the bitmap type if non-null and it matches the bitmap_init_print
    // values
    if (inode_bitmap && bitmap == inode_bitmap) {
        printf(" inode");
    }
    else if (block_bitmap && bitmap == block_bitmap) {
        printf(" block");
    }
    printf(" bitmap content:\n");
    for (int i = 0; i < size; i++) {
        // if the current bit is 0 print 0
        if ((uint8_t)((c_MSB_8_High >> (i % 8)) & bitmap[i / 8]) == c_0x00) {
            printf("0");
        }
        // print 1
        else {
            printf("1");
        }

        // group bits in sets of 4
        if ((i + 1) % 4 == 0) {
            printf(" ");
        }

        // group lines in sets of 64
        if ((i + 1) % 64 == 0) {
            printf("\n");
        }
    }
    printf("\n\n");
}

// finds the next available bit in the map
int bitmap_next(uint8_t* bitmap, int size) {
    // loop over whole map
    for (int i = 0; i < size; i++) {
        // if the current bit is 0 return the index
        if ((uint8_t)((c_MSB_8_High >> (i % 8)) & bitmap[i / 8]) == c_0x00) {
            return i;
        }
    }
    
    // all bits in use, return disk quota reached
    return -EDQUOT;
}

// sets the offset of the bitmap to the value
void bitmap_set(uint8_t* bitmap, int val, int offset, int size) {

    // val must be a 1 or a 0
    assert((val == 0 || val == 1) && offset >= 0 && offset < size);
    
    // print the initial bitmap state
    print_bitmap("pre set", bitmap, size);

    // set bit to 1
    if (val) {
        bitmap[offset / 8] |= c_MSB_8_High >> (offset % 8);
    }
    // set bit to 0
    else {
        bitmap[offset / 8] &= ~(c_MSB_8_High >> (offset % 8));
    }
    
    // print the updated bitmap state
    print_bitmap("post set", bitmap, size);
}
