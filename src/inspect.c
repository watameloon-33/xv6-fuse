#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
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
        perror("Error opening filesystem image");
        return 1;
    }

    struct superblock sb;

    ssize_t bytes_read = pread(fd, &sb, sizeof(sb), BSIZE);

    if (bytes_read != sizeof(sb)) {
        printf("Could not read superblock completely\n");
        close(fd);
        return 1;
    }

    if (sb.magic != FSMAGIC) {
        printf("Invalid xv6 filesystem image\n");
        close(fd);
        return 1;
    }

    printf("Xv6 filesystem detected!\n");
    printf("Magic number: 0x%x\n", sb.magic);
    printf("Total blocks: %u\n", sb.size);
    printf("Data blocks: %u\n", sb.nblocks);
    printf("Inodes: %u\n", sb.ninodes);
    printf("Log blocks: %u\n", sb.nlog);
    printf("Log start: %u\n", sb.logstart);
    printf("Inode start: %u\n", sb.inodestart);
    printf("Bitmap start: %u\n", sb.bmapstart);

    close(fd);
    return 0;
}