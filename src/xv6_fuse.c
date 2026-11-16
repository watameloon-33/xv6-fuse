
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

static const struct fuse_operations operations = {
    .getattr = xv6_getattr,
    .readdir = xv6_readdir,
    .read = xv6_read,
};

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <fs.img> <mountpoint>\n",
                argv[0]);
        return 1;
    }

    image_fd = open(argv[1], O_RDONLY);

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
