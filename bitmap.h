/*
 *  Michael Curley
 *  cs3650
 *  ch03
 *
 *  notes:
 *   - utility functions to manipulate a data bitmap 
 */

#ifndef BITMAP_H
#define BITMAP_H

#include <stdint.h>

void bitmap_init_print(uint8_t* inode, uint8_t* block);

int bitmap_next(uint8_t* bitmap, int size);
void bitmap_set(uint8_t* bitmap, int val, int offset, int size);

#endif
