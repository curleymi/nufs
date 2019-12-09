/*
 *  Michael Curley
 *  cs3650
 *  ch03
 *
 *  notes:
 *   - most functionality is handled from storage.c functions
 *   - if a function could be accomplished in a relatively short amount of lines
 *     it was written here to try and reduce complexity and size of storage.c
 *     file
 *   - see storage.h and storage.c for information about directory structure
 *   - based on cs3650 course code
 */

#include "storage.h"
#include "path.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <bsd/string.h>
#include <assert.h>

#define FUSE_USE_VERSION 26
#include <fuse.h>

// helper function sets the stats of given stat pointer from the index of the
// given inode
void set_stat(uint8_t inode_i, struct stat *st) {
    // get the inode
    inode_t* inode = get_inode(inode_i);

    // set all possible stats
    st->st_mode = inode->mode;
    st->st_nlink = inode->links;
    st->st_atime = inode->a_time;
    st->st_mtime = inode->m_time;
    st->st_size = inode->size;
    st->st_blocks = inode->block_count;
    st->st_blksize = BLOCK_SIZE;
    st->st_uid = getuid();
}

// helper function updates the inode's access time
void update_access_time(uint8_t inode_i, time_t tv_sec) {
    // update current times for access
    inode_t* inode = get_inode(inode_i);
    inode->a_time = tv_sec;
    printf("updating access time\n\n");
}

// helper function updates the inode's access time
void update_modified_time(uint8_t inode_i, time_t tv_sec) {
    // update current times for access
    inode_t* inode = get_inode(inode_i);
    inode->m_time = tv_sec;
    printf("updating modified time\n\n");
}

// updates last access and last modified times to the same values
void update_all_time(uint8_t inode_i, time_t tv_sec) {
    update_access_time(inode_i, tv_sec);
    update_modified_time(inode_i, tv_sec);
}

// implementation for: man 2 access
// Checks if a file exists.
int nufs_access(const char *path, int mask) {
    int rv = storage_access(path, 0);
    printf("access(%s, %04o) -> %d\n\n", path, mask, rv);
    return rv;
}

// implementation for: man 2 stat
// gets an object's attributes (type, permissions, size, etc)
int nufs_getattr(const char *path, struct stat *st) {
    uint8_t inode_i;
    int rv = storage_access(path, &inode_i);

    // set the stats on successful acquisition of the path's inode
    if (rv == 0) {
        set_stat(inode_i, st);
    }
    printf("getattr(%s) -> (%d) {mode: %04o, size: %ld}\n\n", path, rv, st->st_mode, st->st_size);
    return rv;
}

// implementation for: man 2 readdir
// lists the contents of a directory
int nufs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    struct stat st;
    uint8_t inode_i;

    // get the directory's inode
    int rv = storage_access(path, &inode_i);

    // on success
    if (rv == 0) {
        inode_t* inode = get_inode(inode_i);

        // if the inode is not a directory...
        if ((mode_t)(inode->mode & S_IFDIR) != S_IFDIR) {
            rv = -ENOTDIR;
        }
        // if the inode does not have search permissions
        else if ((mode_t)(inode->mode & S_IXUSR) != S_IXUSR) {
            rv = -EACCES;
        }
        // set the stats for the current working directory and its
        // subdirectories and files
        else {
            // set working directory stat
            set_stat(inode_i, &st);
            filler(buf, ".", &st, 0);

            // get the data associated with the directory's inode
            uint8_t* blocks = get_blocks(inode_i);
            int block_count = get_inode(inode_i)->block_count;
            void* block;
            int pathlen;

            // loop over all of the directory's data
            for (int i = 0; i < block_count; i++) {
                // get each individual block
                block = get_block(blocks[i]);

                // set the stats for each path item in the directory
                while ((pathlen = strlen(block))) {
                    set_stat(*(uint8_t*)(block + pathlen + 1), &st);
                    filler(buf, block, &st, 0);
                    block += pathlen + 2;
                }
            }
        }
    }

    printf("readdir(%s) -> %d\n\n", path, rv);
    return rv;
}

// mknod makes a filesystem object like a file or directory
// called for: man 2 open, man 2 link
int nufs_mknod(const char *path, mode_t mode, dev_t rdev) {
    uint8_t inode_i;
    int rv = storage_mknod(path, mode, &inode_i);

    // on success update times
    if (rv == 0) {
        update_all_time(inode_i, time(0));
    }
    printf("mknod(%s, %04o) -> %d\n\n", path, mode, rv);
    return rv;
}

// most of the following callbacks implement
// another system call; see section 2 of the manual
int nufs_mkdir(const char *path, mode_t mode) {
    int rv = nufs_mknod(path, mode | S_IFDIR, 0);
    printf("mkdir(%s) -> %d\n\n", path, rv);
    return rv;
}

// unlinks the path to its inode, if the inode has 0 links after its data is
// completely deleted
int nufs_unlink(const char *path) {
    int rv = storage_unlink(path);
    printf("unlink(%s) -> %d\n\n", path, rv);
    return rv;
}

// links one path inode to another path
int nufs_link(const char *from, const char *to) {
    int rv = storage_link(from, to);
    printf("link(%s => %s) -> %d\n\n", from, to, rv);
	return rv;
}

// removes a directory
int nufs_rmdir(const char *path) {
    uint8_t inode_i;

    // access the path's inode
    int rv = storage_access(path, &inode_i);
    if (rv == 0) {
        // check the inode corresponds to a directory
        inode_t* inode = get_inode(inode_i);
        if ((mode_t)(inode->mode & S_IFDIR) != S_IFDIR) {
            rv = -ENOTDIR;
        }
        // unlink the directory.. directory is removed if refs == 1, if
        // directory is not deleted there exists a bug
        else {
            rv = nufs_unlink(path);
        }
    }
    printf("rmdir(%s) -> %d\n\n", path, rv);
    return rv;
}

// implements: man 2 rename
// called to move a file within the same filesystem
int nufs_rename(const char *from, const char *to) {
    uint8_t inode_i;

    // access the original path's inode
    int rv = storage_access(from, &inode_i);
    if (rv == 0) {
        uint8_t inode_ip;

        // get the directory inode of the to item
        char* parent = parent_directory(to);

        // on successful access of parent inode remove the original path name
        if ((rv = storage_access(parent, &inode_ip)) == 0) {

            // on success, add the new item to its parent directory with the old
            // path's inode as data
            if ((rv = directory_remove(from)) == 0) {
                rv = directory_add(to + strlen(parent) + 1, inode_ip, 0, inode_i);
            }
        }

        // free the allocated parent
        free_parent_directory(parent);
    }

    printf("rename(%s => %s) -> %d\n\n", from, to, rv);
    return rv;
}

// changes the path's inode permissions
int nufs_chmod(const char *path, mode_t mode) {
    uint8_t inode_i;

    // access the path's inode
    int rv = storage_access(path, &inode_i);
    if (rv == 0) {
        // update the permissions
        inode_t* inode = get_inode(inode_i);
        inode->mode = mode;
    }

    printf("chmod(%s, %04o) -> %d\n\n", path, mode, rv);
    return rv;
}

// truncates a file
int nufs_truncate(const char *path, off_t size) {
    uint8_t inode_i;
    int rv = storage_access(path, &inode_i);
    if (rv == 0) {
        // check the inode corresponds to a directory
        inode_t* inode = get_inode(inode_i);
        if ((mode_t)(inode->mode & S_IFDIR) == S_IFDIR) {
            rv = -EISDIR;
        }
        // check the inode has write permissions
        else if ((mode_t)(inode->mode & S_IWUSR) != S_IWUSR) {
            rv = -EACCES;
        }
        // file can be truncated
        else {
            rv = storage_truncate(size, inode_i);
            update_all_time(inode_i, time(0));
        }
    }

    printf("truncate(%s, %ld bytes) -> %d\n\n", path, size, rv);
    return rv;
}

// this is called on open, but doesn't need to do much
// since FUSE doesn't assume you maintain state for
// open files.
int nufs_open(const char *path, struct fuse_file_info *fi) {
    // get access to the path's inode, can only open something that exists
    uint8_t inode_i;
    int rv = storage_access(path, &inode_i);
    if (rv == 0) {
        update_access_time(inode_i, time(0));
    }
    printf("open(%s) -> %d\n\n", path, rv);
    return rv;
}

// Actually read data
int nufs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    int rv = storage_read(path, buf, size, offset);
    printf("read(%s, %ld bytes, @+%ld) -> %d\n\n", path, size, offset, rv);
    return rv;
}

// Actually write data
int nufs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    uint8_t inode_i;
    int rv = storage_write(path, buf, size, offset, &inode_i);

    // on success update time stamps
    if (rv >= 0) {
        update_all_time(inode_i, time(0));
    }

    printf("write(%s, %ld bytes, @+%ld) -> %d\n\n", path, size, offset, rv);
    return rv;
}

// Update the timestamps on a file or directory.
int nufs_utimens(const char* path, const struct timespec ts[2]) {
    uint8_t inode_i;
    
    // get access to the path's inode
    int rv = storage_access(path, &inode_i);
    if (rv == 0) {
        // on success, update times
        update_access_time(inode_i, ts[0].tv_sec);
        update_modified_time(inode_i, ts[1].tv_sec);
    }

    printf("utimens(%s, [%ld, %ld; %ld %ld]) -> %d\n\n",
           path, ts[0].tv_sec, ts[0].tv_nsec, ts[1].tv_sec, ts[1].tv_nsec, rv);
	return rv;
}

// Extended operations
int nufs_ioctl(const char* path, int cmd, void* arg, struct fuse_file_info* fi, unsigned int flags, void* data) {
    // not implemented for challenge
    int rv = -1;
    printf("ioctl(%s, %d, ...) -> %d\n\n", path, cmd, rv);
    return rv;
}

// initializes the callbacks for controlling fuse
void nufs_init_ops(struct fuse_operations* ops) {
    memset(ops, 0, sizeof(struct fuse_operations));
    ops->access   = nufs_access;
    ops->getattr  = nufs_getattr;
    ops->readdir  = nufs_readdir;
    ops->mknod    = nufs_mknod;
    ops->mkdir    = nufs_mkdir;
    ops->link     = nufs_link;
    ops->unlink   = nufs_unlink;
    ops->rmdir    = nufs_rmdir;
    ops->rename   = nufs_rename;
    ops->chmod    = nufs_chmod;
    ops->truncate = nufs_truncate;
    ops->open	  = nufs_open;
    ops->read     = nufs_read;
    ops->write    = nufs_write;
    ops->utimens  = nufs_utimens;
    ops->ioctl    = nufs_ioctl;
};

// the structure to initialize the fuse ops
struct fuse_operations nufs_ops;

// main entry point
int main(int argc, char *argv[]) {
    // check the program was called correctly
    assert(argc > 2 && argc < 6);

    // initialize the storage with the given file
    storage_init(argv[--argc]);

    // init the ops
    nufs_init_ops(&nufs_ops);

    // run fuse main
    return fuse_main(argc, argv, &nufs_ops, NULL);
}

