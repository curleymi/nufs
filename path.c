/*
 *  Michael Curley
 *  cs3650
 *  ch03
 */

#include "path.h"

#include <string.h>
#include <stdlib.h>
#include <assert.h>

// frees the delimited path created from delimit_path function
void free_delimited_path(char* path) {
    free(path - 1);
}

// frees the parent directory created from parent_directory function
void free_parent_directory(char* path) {
    free(path);
}

// replaces all '/' chars with null, used for searching directories
char* delimit_path(const char* original_path) {
    // allocate the null delimited path
    int len = strlen(original_path);
    char* path = malloc((len + 2) * sizeof(char));

    // copy the data
    memcpy(path, original_path, len + 1);

    // double null terminate the path
    path[len] = 0;
    path[len + 1] = 0;

    // loop over whole path, replacing directory '/' with null
    for (int i = 0; i < len; i++) {
        if (path[i] == '/') {
            path[i] = 0;
        }
    }

    // root directory is '/' so the first char should have been replaced by null
    assert(strlen(path) == 0);

    return path + 1;
}

// returns the parent directory of the given path
char* parent_directory(const char* original_path) {
    int last = 0;

    // loop over whole path to find last index of '/'
    // strlen - 1 used since last char could be '/'
    for (int i = 0; i < strlen(original_path) - 1; i++) {
        if (original_path[i] == '/') {
            last = i;
        }
    }

    // malloc the new parent path size, copy data and null terminate
    char* path = malloc((last + 1) * sizeof(char));
    memcpy(path, original_path, last);
    path[last] = 0;
    return path;
}
