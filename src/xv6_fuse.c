
#define FUSE_USE_VERSION 31

#include <sys/sysmacros.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fuse3/fuse.h>

#include "../include/xv6_fs.h"

static int image_fd;
static struct superblock sb;

// Read an inode using its inode number
static int read_inode(uint32_t inum, struct dinode *inode) {
    if (inum >= sb.ninodes)
        return -1;

    uint32_t block = inum / IPB + sb.inodestart;
    off_t offset = (off_t)block * BSIZE +
                   (inum % IPB) * sizeof(struct dinode);

    if (pread(image_fd, inode, sizeof(*inode), offset)
        != sizeof(*inode))
        return -1;

    return 0;
}

#define BPB (BSIZE * 8)

// Write a modified inode back to the filesystem image
static int write_inode(uint32_t inum, struct dinode *inode) {
    if (inum >= sb.ninodes)
        return -1;

    uint32_t block = inum / IPB + sb.inodestart;
    off_t offset = (off_t)block * BSIZE +
                   (inum % IPB) * sizeof(struct dinode);

    if (pwrite(image_fd, inode, sizeof(*inode), offset)
        != sizeof(*inode))
        return -1;

    return 0;
}

// Allocate a free disk block
static int balloc(uint32_t *result) {
    uint32_t data_start = sb.size - sb.nblocks;

    for (uint32_t b = data_start; b < sb.size; b++) { 
        uint32_t bitmap_block = b / BPB + sb.bmapstart;
        uint32_t bit = b % BPB;
        uint8_t byte;

        off_t offset = (off_t)bitmap_block * BSIZE + bit / 8;

        if (pread(image_fd, &byte, 1, offset) != 1)
            return -1;

        if ((byte & (1U << (bit % 8))) == 0) {
            byte |= (uint8_t)(1U << (bit % 8));

            if (pwrite(image_fd, &byte, 1, offset) != 1)
                return -1;

            uint8_t zero[BSIZE] = {0};

            if (pwrite(image_fd, zero, BSIZE,
                       (off_t)b * BSIZE) != BSIZE)
                return -1;

            *result = b;
            return 0;
        }
    }

    return -1;
}

// Free a disk block
static int bfree(uint32_t b) {
    uint32_t data_start = sb.size - sb.nblocks;

     if (b < data_start || b >= sb.size)
       return -1;

    uint32_t bitmap_block = b / BPB + sb.bmapstart;
    uint32_t bit = b % BPB;
    uint8_t byte;

    off_t offset = (off_t)bitmap_block * BSIZE + bit / 8;

    if (pread(image_fd, &byte, 1, offset) != 1)
        return -1;

    if ((byte & (1U << (bit % 8))) == 0)
        return -1;

    byte &= (uint8_t)~(1U << (bit % 8));

    return pwrite(image_fd, &byte, 1, offset) == 1 ? 0 : -1;
}

// Allocate a free inode
static int ialloc(uint16_t type, uint16_t *result) {
    for (uint32_t i = 1; i < sb.ninodes; i++) {
        struct dinode inode;

        if (read_inode(i, &inode) < 0)
            return -1;

        if (inode.type == 0) {
            memset(&inode, 0, sizeof(inode));
            inode.type = type;

            if (write_inode(i, &inode) < 0)
                return -1;

            *result = (uint16_t)i;
            return 0;
        }
    }

    return -1;
}
// Free all data blocks belonging to an inode
static int itrunc(struct dinode *inode) {
    // Free direct blocks
    for (int i = 0; i < NDIRECT; i++) {
        if (inode->addrs[i] != 0) {
            if (bfree(inode->addrs[i]) < 0)
                return -1;

            inode->addrs[i] = 0;
        }
    }

    // Free indirect data blocks
    if (inode->addrs[NDIRECT] != 0) {
        uint32_t indirect[NINDIRECT];

        if (pread(image_fd, indirect, BSIZE,
                  (off_t)inode->addrs[NDIRECT] * BSIZE) != BSIZE)
            return -1;

        for (int i = 0; i < NINDIRECT; i++) {
            if (indirect[i] != 0) {
                if (bfree(indirect[i]) < 0)
                    return -1;
            }
        }

        // Free the indirect block itself
        if (bfree(inode->addrs[NDIRECT]) < 0)
            return -1;

        inode->addrs[NDIRECT] = 0;
    }

    inode->size = 0;
    return 0;
}
// Get the physical disk block containing a file's data
static int get_data_block(struct dinode *inode,
                          uint32_t file_block,
                          uint32_t *disk_block) {
    // First 12 blocks are stored directly in the inode
    if (file_block < NDIRECT) {
        *disk_block = inode->addrs[file_block];
        return 0;
    }

    // Remaining blocks use the single-indirect block
    file_block -= NDIRECT;

    if (file_block >= NINDIRECT)
        return -1;

    if (inode->addrs[NDIRECT] == 0) {
        *disk_block = 0;
        return 0;
    }

    uint32_t indirect[NINDIRECT];

    if (pread(image_fd, indirect, BSIZE,
              (off_t)inode->addrs[NDIRECT] * BSIZE) != BSIZE)
        return -1;

    *disk_block = indirect[file_block];
    return 0;
}

// Get a data block, allocating it if necessary
static int get_data_block_alloc(struct dinode *inode,
                                uint32_t file_block,
                                uint32_t *disk_block) {
    // Direct blocks
    if (file_block < NDIRECT) {
        if (inode->addrs[file_block] == 0) {
            if (balloc(&inode->addrs[file_block]) < 0)
                return -1;
        }

        *disk_block = inode->addrs[file_block];
        return 0;
    }

    // Single-indirect blocks
    file_block -= NDIRECT;

    if (file_block >= NINDIRECT)
        return -1;

    uint32_t indirect[NINDIRECT];

    if (inode->addrs[NDIRECT] == 0) {
        if (balloc(&inode->addrs[NDIRECT]) < 0)
            return -1;

        memset(indirect, 0, sizeof(indirect));
    } else {
        if (pread(image_fd, indirect, BSIZE,
                  (off_t)inode->addrs[NDIRECT] * BSIZE) != BSIZE)
            return -1;
    }

    if (indirect[file_block] == 0) {
        if (balloc(&indirect[file_block]) < 0)
            return -1;

        if (pwrite(image_fd, indirect, BSIZE,
                   (off_t)inode->addrs[NDIRECT] * BSIZE) != BSIZE)
            return -1;
    }

    *disk_block = indirect[file_block];
    return 0;
}


// Find an entry inside any directory inode
static int find_entry_in_dir(uint16_t dir_inum,
                             const char *name,
                             uint16_t *found_inum) {
    struct dinode dir;

    if (read_inode(dir_inum, &dir) < 0 || dir.type != 1)
        return -1;

    size_t name_len = strlen(name);

    if (name_len == 0 || name_len > DIRSIZ)
        return -1;

    uint32_t blocks = (dir.size + BSIZE - 1) / BSIZE;

    for (uint32_t b = 0; b < blocks; b++) {
        uint32_t disk_block;

        if (get_data_block(&dir, b, &disk_block) < 0)
            return -1;

        if (disk_block == 0)
            continue;

        struct dirent entries[BSIZE / sizeof(struct dirent)];

        if (pread(image_fd, entries, BSIZE,
                  (off_t)disk_block * BSIZE) != BSIZE)
            return -1;

        for (size_t i = 0; i < BSIZE / sizeof(struct dirent); i++) {
            if (entries[i].inum == 0)
                continue;

            if (memcmp(entries[i].name, name, name_len) == 0 &&
                (name_len == DIRSIZ ||
                 entries[i].name[name_len] == '\0')) {
                *found_inum = entries[i].inum;
                return 0;
            }
        }
    }

    return -1;
}
// Add a new entry to a directory
static int dirlink(uint16_t dir_inum,
                   const char *name,
                   uint16_t inum) {
    struct dinode dir;

    if (read_inode(dir_inum, &dir) < 0 || dir.type != 1)
        return -1;

    size_t name_len = strlen(name);

    if (name_len == 0 || name_len > DIRSIZ)
        return -1;

    uint16_t existing;

    if (find_entry_in_dir(dir_inum, name, &existing) == 0)
        return -1;

    struct dirent entry;
    memset(&entry, 0, sizeof(entry));

    entry.inum = inum;
    memcpy(entry.name, name, name_len);

    uint32_t offset = dir.size;
    int found_empty = 0;

    // Look for an unused directory entry
    for (uint32_t pos = 0; pos + sizeof(struct dirent) <= dir.size;
         pos += sizeof(struct dirent)) {

        uint32_t disk_block;

        if (get_data_block(&dir, pos / BSIZE, &disk_block) < 0)
            return -1;

        struct dirent current;

        if (pread(image_fd, &current, sizeof(current),
                  (off_t)disk_block * BSIZE + pos % BSIZE)
            != sizeof(current))
            return -1;

        if (current.inum == 0) {
            offset = pos;
            found_empty = 1;
            break;
        }
    }

    // Allocate a new directory data block if necessary
    uint32_t disk_block;

    if (get_data_block_alloc(&dir, offset / BSIZE, &disk_block) < 0)
        return -1;

    if (pwrite(image_fd, &entry, sizeof(entry),
               (off_t)disk_block * BSIZE + offset % BSIZE)
        != sizeof(entry))
        return -1;

    // Increase directory size only when appending
    if (!found_empty)
        dir.size += sizeof(struct dirent);

    return write_inode(dir_inum, &dir);
}

// Resolve a complete path to its inode number
static int resolve_path(const char *path, uint16_t *inum) {
    if (path[0] != '/')
        return -1;

    if (strlen(path) >= 1024)
        return -1;

    if (strcmp(path, "/") == 0) {
        *inum = ROOTINO;
        return 0;
    }

    char copy[1024];
    strcpy(copy, path);

    uint16_t current = ROOTINO;
    char *saveptr;
    char *component = strtok_r(copy, "/", &saveptr);

    while (component != NULL) {
        uint16_t next;
        struct dinode dir;

        if (read_inode(current, &dir) < 0 || dir.type != 1)
            return -1;

        if (find_entry_in_dir(current, component, &next) < 0)
            return -1;

        current = next;
        component = strtok_r(NULL, "/", &saveptr);
    }

    *inum = current;
    return 0;
}

static int split_path(const char *path,
                      char *parent, char *name) {
    if (!path || path[0] != '/' || strlen(path) >= 1024)
        return -1;

    char copy[1024];
    strcpy(copy, path);

    char *slash = strrchr(copy, '/');

    if (!slash || slash[1] == '\0')
        return -1;

    if (strlen(slash + 1) > DIRSIZ)
        return -1;

    strcpy(name, slash + 1);

    if (slash == copy) {
        strcpy(parent, "/");
    } else {
        *slash = '\0';
        strcpy(parent, copy);
    }

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return -1;

    return 0;
}
// Create a new regular file
static int xv6_create(const char *path, mode_t mode,
                      struct fuse_file_info *fi) {
    (void)mode;
    (void)fi;

    char parent_path[1024];
    char name[DIRSIZ + 1];

    if (split_path(path, parent_path, name) < 0)
        return -EINVAL;

    uint16_t parent_inum;

    if (resolve_path(parent_path, &parent_inum) < 0)
        return -ENOENT;

    struct dinode parent;

    if (read_inode(parent_inum, &parent) < 0 || parent.type != 1)
        return -ENOTDIR;

    uint16_t existing;

    if (find_entry_in_dir(parent_inum, name, &existing) == 0)
        return -EEXIST;

    uint16_t new_inum;

    if (ialloc(2, &new_inum) < 0)
        return -ENOSPC;

    struct dinode inode;

    if (read_inode(new_inum, &inode) < 0)
        return -EIO;

    inode.type = 2;
    inode.nlink = 1;
    inode.size = 0;

    if (write_inode(new_inum, &inode) < 0)
        return -EIO;

    if (dirlink(parent_inum, name, new_inum) < 0) {
        inode.type = 0;
        inode.nlink = 0;
        write_inode(new_inum, &inode);
        return -EIO;
    }

    return 0;
}
// Create a new directory
static int xv6_mkdir(const char *path, mode_t mode) {
    (void)mode;

    char parent_path[1024];
    char name[DIRSIZ + 1];

    if (split_path(path, parent_path, name) < 0)
        return -EINVAL;

    uint16_t parent_inum;

    if (resolve_path(parent_path, &parent_inum) < 0)
        return -ENOENT;

    uint16_t existing;

    if (find_entry_in_dir(parent_inum, name, &existing) == 0)
        return -EEXIST;

    uint16_t new_inum;

    if (ialloc(1, &new_inum) < 0)
        return -ENOSPC;

    struct dinode dir;

    if (read_inode(new_inum, &dir) < 0)
        return -EIO;

    dir.type = 1;
    dir.nlink = 1;
    dir.size = 0;

    if (write_inode(new_inum, &dir) < 0)
        return -EIO;

    // Add "." and ".." entries
    struct dirent entries[2] = {0};

    entries[0].inum = new_inum;
    memcpy(entries[0].name, ".", 1);

    entries[1].inum = parent_inum;
    memcpy(entries[1].name, "..", 2);

    uint32_t block;

    if (get_data_block_alloc(&dir, 0, &block) < 0)
        return -EIO;

    if (pwrite(image_fd, entries, sizeof(entries),
               (off_t)block * BSIZE) != sizeof(entries))
        return -EIO;

    dir.size = sizeof(entries);

    if (write_inode(new_inum, &dir) < 0)
        return -EIO;

    if (dirlink(parent_inum, name, new_inum) < 0)
        return -EIO;

    return 0;
}
// Delete a regular file
static int xv6_unlink(const char *path) {
    char parent_path[1024];
    char name[DIRSIZ + 1];

    if (split_path(path, parent_path, name) < 0)
        return -EINVAL;

    uint16_t parent_inum;

    if (resolve_path(parent_path, &parent_inum) < 0)
        return -ENOENT;

    uint16_t target_inum;

    if (find_entry_in_dir(parent_inum, name, &target_inum) < 0)
        return -ENOENT;

    struct dinode inode;

    if (read_inode(target_inum, &inode) < 0)
        return -EIO;

    // Only regular files can be unlinked here
    if (inode.type != 2)
        return -EISDIR;

    struct dinode parent;

    if (read_inode(parent_inum, &parent) < 0)
        return -EIO;

    // Find and clear the directory entry
    uint32_t blocks = (parent.size + BSIZE - 1) / BSIZE;
    int found = 0;

    for (uint32_t b = 0; b < blocks && !found; b++) {
        uint32_t disk_block;

        if (get_data_block(&parent, b, &disk_block) < 0)
            return -EIO;

        struct dirent entries[BSIZE / sizeof(struct dirent)];

        if (pread(image_fd, entries, BSIZE,
                  (off_t)disk_block * BSIZE) != BSIZE)
            return -EIO;

        for (size_t i = 0; i < BSIZE / sizeof(struct dirent); i++) {
            if (entries[i].inum == target_inum &&
                memcmp(entries[i].name, name, strlen(name)) == 0) {

                struct dirent empty = {0};

                off_t offset = (off_t)disk_block * BSIZE
                             + i * sizeof(struct dirent);

                if (pwrite(image_fd, &empty, sizeof(empty), offset)
                    != sizeof(empty))
                    return -EIO;

                found = 1;
                break;
            }
        }
    }

    if (!found)
        return -ENOENT;

    // Free the file's data blocks
    if (itrunc(&inode) < 0)
        return -EIO;

    // Release the inode
    memset(&inode, 0, sizeof(inode));

    if (write_inode(target_inum, &inode) < 0)
        return -EIO;

    return 0;
}
// Read file contents from the xv6 disk image
static int xv6_read(const char *path, char *buf,
                    size_t size, off_t offset,
                    struct fuse_file_info *fi) {
    (void)fi;

    if (offset < 0)
        return -EINVAL;

    uint16_t inum;

    if (resolve_path(path, &inum) < 0)
        return -ENOENT;

    struct dinode inode;

    if (read_inode(inum, &inode) < 0)
        return -EIO;

    if (inode.type != 2)
        return -EISDIR;

    if ((uint64_t)offset >= inode.size)
        return 0;

    if (size > inode.size - (uint32_t)offset)
        size = inode.size - (uint32_t)offset;

    size_t bytes_read = 0;

    while (bytes_read < size) {
        uint32_t file_block = (uint32_t)(offset / BSIZE);
        uint32_t block_offset = (uint32_t)(offset % BSIZE);
        uint32_t disk_block;

        if (get_data_block(&inode, file_block, &disk_block) < 0)
            return -EIO;

        char block_data[BSIZE];

        if (disk_block == 0) {
            memset(block_data, 0, BSIZE);
        } else if (pread(image_fd, block_data, BSIZE,
                         (off_t)disk_block * BSIZE) != BSIZE) {
            return -EIO;
        }

        size_t available = BSIZE - block_offset;
        size_t remaining = size - bytes_read;
        size_t to_copy = available < remaining ? available : remaining;

        memcpy(buf + bytes_read, block_data + block_offset, to_copy);

        bytes_read += to_copy;
        offset += to_copy;
    }

    return (int)bytes_read;
}

static int xv6_write(const char *path, const char *buf,
                     size_t size, off_t offset,
                     struct fuse_file_info *fi) {
    (void)fi;

    if (offset < 0)
        return -EINVAL;

    uint16_t inum;

    if (resolve_path(path, &inum) < 0)
        return -ENOENT;

    struct dinode inode;

    if (read_inode(inum, &inode) < 0)
        return -EIO;

    if (inode.type != 2)
        return -EISDIR;

    if ((uint64_t)offset + size > MAXFILE * BSIZE)
        return -EFBIG;

    size_t written = 0;

    while (written < size) {
        uint32_t file_block = (offset + written) / BSIZE;
        uint32_t block_offset = (offset + written) % BSIZE;
        size_t bytes = BSIZE - block_offset;

        if (bytes > size - written)
            bytes = size - written;

        uint32_t disk_block;

        if (get_data_block_alloc(&inode, file_block,
                                 &disk_block) < 0)
            return written ? (int)written : -ENOSPC;

        if (pwrite(image_fd, buf + written, bytes,
                   (off_t)disk_block * BSIZE + block_offset)
            != (ssize_t)bytes)
            return written ? (int)written : -EIO;

        written += bytes;
    }

    if ((uint64_t)offset + written > inode.size)
        inode.size = offset + written;

    if (write_inode(inum, &inode) < 0)
        return written ? (int)written : -EIO;

    return (int)written;
}

// Change the size of an existing xv6 file
static int xv6_truncate(const char *path, off_t new_size,
                        struct fuse_file_info *fi) {
    (void)fi;

    if (new_size < 0 || (uint64_t)new_size > MAXFILE * BSIZE)
        return -EFBIG;

    uint16_t inum;

    if (resolve_path(path, &inum) < 0)
        return -ENOENT;

    struct dinode inode;

    if (read_inode(inum, &inode) < 0)
        return -EIO;

    if (inode.type != 2)
        return -EISDIR;

    uint32_t old_blocks = (inode.size + BSIZE - 1) / BSIZE;
    uint32_t new_blocks = ((uint32_t)new_size + BSIZE - 1) / BSIZE;

    // Load the indirect block, if present
    uint32_t indirect[NINDIRECT] = {0};

    if (inode.addrs[NDIRECT] != 0) {
        if (pread(image_fd, indirect, BSIZE,
                  (off_t)inode.addrs[NDIRECT] * BSIZE) != BSIZE)
            return -EIO;
    }

    // Free data blocks that are no longer needed
    for (uint32_t i = new_blocks; i < old_blocks; i++) {
        uint32_t block = 0;

        if (i < NDIRECT) {
            block = inode.addrs[i];
            inode.addrs[i] = 0;
        } else {
            block = indirect[i - NDIRECT];
            indirect[i - NDIRECT] = 0;
        }

        if (block != 0 && bfree(block) < 0)
            return -EIO;
    }

    // Update or free the indirect block
    if (inode.addrs[NDIRECT] != 0) {
        int has_indirect_data = 0;

        for (uint32_t i = 0; i < NINDIRECT; i++) {
            if (indirect[i] != 0) {
                has_indirect_data = 1;
                break;
            }
        }

        if (has_indirect_data) {
            if (pwrite(image_fd, indirect, BSIZE,
                       (off_t)inode.addrs[NDIRECT] * BSIZE) != BSIZE)
                return -EIO;
        } else {
            if (bfree(inode.addrs[NDIRECT]) < 0)
                return -EIO;

            inode.addrs[NDIRECT] = 0;
        }
    }

    // Zero the unused tail of the final retained block
    if ((uint32_t)new_size < inode.size &&
        new_size > 0 &&
        (uint32_t)new_size % BSIZE != 0) {

        uint32_t last_block;
        uint32_t index = (uint32_t)new_size / BSIZE;

        if (get_data_block(&inode, index, &last_block) == 0) {
            char zeros[BSIZE] = {0};
            uint32_t start = (uint32_t)new_size % BSIZE;

            if (pwrite(image_fd, zeros, BSIZE - start,
                       (off_t)last_block * BSIZE + start)
                != BSIZE - start)
                return -EIO;
        }
    }

    // If extending, zero any newly exposed bytes in already allocated blocks
    if (new_size > inode.size) {
        uint32_t start = inode.size;
        uint32_t end = (uint32_t)new_size;

        while (start < end) {
            uint32_t index = start / BSIZE;
            uint32_t block_offset = start % BSIZE;
            uint32_t disk_block;

            if (get_data_block(&inode, index, &disk_block) == 0) {
                uint32_t count = BSIZE - block_offset;

                if (count > end - start)
                    count = end - start;

                char zeros[BSIZE] = {0};

                if (pwrite(image_fd, zeros, count,
                           (off_t)disk_block * BSIZE + block_offset)
                    != count)
                    return -EIO;

                start += count;
            } else {
                // An unallocated block is already a logical zero-filled hole
                start = (index + 1) * BSIZE;
            }
        }
    }

    inode.size = (uint32_t)new_size;

    if (write_inode(inum, &inode) < 0)
        return -EIO;

    return 0;
}
// Tell Linux about file metadata
static int xv6_getattr(const char *path, struct stat *st,
                       struct fuse_file_info *fi) {
    (void)fi;
    memset(st, 0, sizeof(*st));

    struct dinode inode;

    uint16_t inum;

    if (resolve_path(path, &inum) < 0)
        return -ENOENT;

    if (read_inode(inum, &inode) < 0)
        return -EIO;

    if (inode.type == 1) {
    st->st_mode = S_IFDIR | 0755;
    st->st_nlink = inode.nlink;
} else if (inode.type == 2) {
    st->st_mode = S_IFREG | 0644;
    st->st_nlink = inode.nlink;
} else if (inode.type == 3) {
    st->st_mode = S_IFCHR | 0666;
    st->st_nlink = inode.nlink;
    st->st_rdev = makedev(inode.major, inode.minor);
} else {
    return -ENOENT;
}

st->st_uid = getuid();
st->st_gid = getgid();

    st->st_size = inode.size;
    return 0;
}

// Give Linux the directory entries
static int xv6_readdir(const char *path, void *buf,
                       fuse_fill_dir_t filler, off_t offset,
                       struct fuse_file_info *fi,
                       enum fuse_readdir_flags flags) {
    (void)offset;
    (void)fi;
    (void)flags;

    uint16_t inum;

     if (resolve_path(path, &inum) < 0)
    return -ENOENT;

    struct dinode dir;

    if (read_inode(inum, &dir) < 0)
    return -EIO;

    if (dir.type != 1)
    return -ENOTDIR;

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    uint32_t num_blocks = (dir.size + BSIZE - 1) / BSIZE;

for (uint32_t b = 0; b < num_blocks; b++) {
    uint32_t disk_block;

    if (get_data_block(&dir, b, &disk_block) < 0)
        return -EIO;

    if (disk_block == 0)
        continue;

    struct dirent entries[BSIZE / sizeof(struct dirent)];

    if (pread(image_fd, entries, BSIZE,
              (off_t)disk_block * BSIZE) != BSIZE)
        return -EIO;

    for (size_t i = 0; i < BSIZE / sizeof(struct dirent); i++) {
        if (entries[i].inum == 0)
            continue;

        char name[DIRSIZ + 1];
        memcpy(name, entries[i].name, DIRSIZ);
        name[DIRSIZ] = '\0';

        if (strcmp(name, ".") != 0 &&
            strcmp(name, "..") != 0)
            filler(buf, name, NULL, 0, 0);
           }
        } 

    return 0;
}

static struct fuse_operations operations = {
    .getattr = xv6_getattr,
    .readdir = xv6_readdir,
    .read = xv6_read,
    .write = xv6_write,
    .create = xv6_create,
    .mkdir = xv6_mkdir,
    .unlink = xv6_unlink,
    .truncate = xv6_truncate,
};

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <fs.img> <mountpoint>\n",
                argv[0]);
        return 1;
    }

    image_fd = open(argv[1], O_RDWR);

    if (image_fd < 0) {
        perror("Cannot open filesystem image");
        return 1;
    }

    if (pread(image_fd, &sb, sizeof(sb), BSIZE) != sizeof(sb) ||
        sb.magic != FSMAGIC) {
        fprintf(stderr, "Invalid xv6 filesystem image\n");
        close(image_fd);
        return 1;
    }

    char *fuse_argv[] = {
        argv[0], argv[2], "-f", NULL
    };

    int result = fuse_main(3, fuse_argv, &operations, NULL);

    close(image_fd);
    return result;
}
