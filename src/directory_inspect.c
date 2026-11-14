
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#include "../include/xv6_fs.h"

int main(int argc, char *argv[]) {

    if (argc != 2) {
        printf("Usage: %s <filesystem-image>\n", argv[0]);
        return 1;
    }

    int fd = open(argv[1], O_RDONLY);

    if (fd < 0) {
        perror("Cannot open image");
        return 1;
    }

    // Read superblock
    struct superblock sb;

    if (pread(fd, &sb, sizeof(sb), BSIZE) != sizeof(sb)) {
        printf("Cannot read superblock\n");
        close(fd);
        return 1;
    }

    // Read root inode
    uint32_t inum = 1;
    uint32_t block = inum / IPB + sb.inodestart;
    uint32_t offset = block * BSIZE +
                      (inum % IPB) * sizeof(struct dinode);

    struct dinode root;

    if (pread(fd, &root, sizeof(root), offset) != sizeof(root)) {
        printf("Cannot read root inode\n");
        close(fd);
        return 1;
    }

    // Check whether root inode is a directory
    if (root.type != 1) {
        printf("Root inode is not a directory\n");
        close(fd);
        return 1;
    }

    // Read root directory's first data block
    struct dirent entries[BSIZE / sizeof(struct dirent)];

    if (root.addrs[0] == 0 ||
        pread(fd, entries, BSIZE,
              (off_t)root.addrs[0] * BSIZE) != BSIZE) {
        printf("Cannot read root directory\n");
        close(fd);
        return 1;
    }

    printf("Root directory contents:\n\n");

    // Display directory entries
    for (size_t i = 0; i < BSIZE / sizeof(struct dirent); i++) {

        if (entries[i].inum != 0) {
            printf("Inode: %u  Name: %.*s\n",
                   entries[i].inum,
                   DIRSIZ,
                   entries[i].name);
        }
    }

    close(fd);
    return 0;
}
