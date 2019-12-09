/*
 *  Michael Curley
 *  cs3650
 *  ch03
 *
 *  notes:
 *   - utility functions to manipulate directory and file paths
 */

#ifndef PATH_H
#define PATH_H

void free_delimited_path(char* path);
void free_parent_directory(char* path);
char* delimit_path(const char* original_path);
char* parent_directory(const char* original_path);

#endif
