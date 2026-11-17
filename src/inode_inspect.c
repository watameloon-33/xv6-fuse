
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include "../include/xv6_fs.h"

int main(int argc, char *argv[]) {

    if (argc != 3) {
    printf("Usage: %s <filesystem-image> <inode-number>\n", argv[0]);
    return 1;
}

    int fd = open(argv[1], O_RDONLY);

    if (fd < 0) {
        perror("Cannot open image");
        return 1;
    }

    // Read the superblock
    struct superblock sb;

    if (pread(fd, &sb, sizeof(sb), BSIZE) != sizeof(sb)) {
        printf("Cannot read superblock\n");
        close(fd);
        return 1;
    }

    if (sb.magic != FSMAGIC) {
        printf("Invalid filesystem\n");
        close(fd);
        return 1;
    }

    // Inode number supplied by the user
uint32_t inum = (uint32_t)strtoul(argv[2], NULL, 10);

if (inum == 0 || inum >= sb.ninodes) {
    printf("Invalid inode number\n");
    close(fd);
    return 1;
}

    // Calculate inode location
    uint32_t block = inum / IPB + sb.inodestart;
    uint32_t offset = block * BSIZE + (inum % IPB) * sizeof(struct dinode);

    // Read inode
    struct dinode inode;

    if (pread(fd, &inode, sizeof(inode), offset) != sizeof(inode)) {
        printf("Cannot read inode\n");
        close(fd);
        return 1;
    }

    printf("Inode successfully read!\n\n");
    printf("Inode number: %u\n", inum);
    printf("Inode block: %u\n", block);
    printf("Type: %d\n", inode.type);
    printf("Link count: %d\n", inode.nlink);
    printf("File size: %u bytes\n", inode.size);

    printf("\nData block addresses:\n");

    for (int i = 0; i < NDIRECT + 1; i++) {
        printf("Address[%d]: %u\n", i, inode.addrs[i]);
    }

    close(fd);
    return 0;
}
