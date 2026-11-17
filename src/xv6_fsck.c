
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include "../include/xv6_fs.h"

#define BPB (BSIZE * 8)

static int fd;
static struct superblock sb;
static int errors = 0;
static unsigned char *used;

static int read_block(uint32_t block, void *buf) {
    if (block >= sb.size)
        return -1;

    return pread(fd, buf, BSIZE, (off_t)block * BSIZE) == BSIZE
           ? 0 : -1;
}

static int read_inode(uint32_t inum, struct dinode *inode) {
    if (inum >= sb.ninodes)
        return -1;

    uint32_t block = sb.inodestart + inum / IPB;
    off_t offset = (off_t)block * BSIZE +
                   (inum % IPB) * sizeof(struct dinode);

    return pread(fd, inode, sizeof(*inode), offset) ==
           sizeof(*inode) ? 0 : -1;
}

static int bitmap_allocated(uint32_t block) {
    uint32_t bitmap_block = block / BPB + sb.bmapstart;
    unsigned char bitmap[BSIZE];

    if (read_block(bitmap_block, bitmap) < 0)
        return 0;

    uint32_t bit = block % BPB;
    return (bitmap[bit / 8] >> (bit % 8)) & 1;
}

static int check_block(uint32_t block, uint32_t inum) {
    if (block == 0)
        return 0;

    uint32_t data_start = sb.size - sb.nblocks;

    if (block < data_start || block >= sb.size) {
        printf("ERROR: inode %u references invalid block %u\n",
               inum, block);
        errors++;
        return -1;
    }

    if (used[block]) {
        printf("ERROR: block %u is referenced more than once\n",
               block);
        errors++;
    } else {
        used[block] = 1;
    }

    if (!bitmap_allocated(block)) {
        printf("ERROR: block %u is used but marked free\n", block);
        errors++;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <xv6-fs-image>\n", argv[0]);
        return 1;
    }

    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    if (pread(fd, &sb, sizeof(sb), BSIZE) != sizeof(sb)) {
        fprintf(stderr, "ERROR: cannot read superblock\n");
        close(fd);
        return 1;
    }

    if (sb.magic != FSMAGIC || sb.size == 0 ||
        sb.nblocks == 0 || sb.ninodes == 0 ||
        sb.nblocks > sb.size ||
        sb.inodestart >= sb.size ||
        sb.bmapstart >= sb.size) {
        fprintf(stderr, "ERROR: invalid superblock\n");
        close(fd);
        return 1;
    }

    printf("Checking xv6 filesystem...\n");
    printf("Blocks: %u | Inodes: %u\n", sb.size, sb.ninodes);

    used = calloc(sb.size, sizeof(unsigned char));
    if (!used) {
        perror("calloc");
        close(fd);
        return 1;
    }

    uint32_t data_start = sb.size - sb.nblocks;

    for (uint32_t inum = 1; inum < sb.ninodes; inum++) {
        struct dinode inode;

        if (read_inode(inum, &inode) < 0) {
            printf("ERROR: cannot read inode %u\n", inum);
            errors++;
            continue;
        }

        if (inode.type == 0)
            continue;

        if (inode.type < 1 || inode.type > 3) {
            printf("ERROR: inode %u has invalid type %d\n",
                   inum, inode.type);
            errors++;
            continue;
        }

        if (inode.size > MAXFILE * BSIZE) {
            printf("ERROR: inode %u has invalid size\n", inum);
            errors++;
            continue;
        }

        uint32_t blocks = (inode.size + BSIZE - 1) / BSIZE;

        for (uint32_t i = 0; i < blocks && i < NDIRECT; i++)
            check_block(inode.addrs[i], inum);

        if (blocks > NDIRECT) {
            uint32_t indirect_block = inode.addrs[NDIRECT];

            if (indirect_block == 0) {
                printf("ERROR: inode %u is missing its indirect block\n",
                       inum);
                errors++;
            } else if (check_block(indirect_block, inum) == 0) {
                uint32_t indirect[NINDIRECT];

                if (read_block(indirect_block, indirect) < 0) {
                    printf("ERROR: cannot read indirect block of inode %u\n",
                           inum);
                    errors++;
                } else {
                    for (uint32_t i = 0;
                         i < blocks - NDIRECT && i < NINDIRECT; i++)
                        check_block(indirect[i], inum);
                }
            }
        }
    }
     // Check for allocated blocks that are not referenced by any inode
    for (uint32_t block = data_start; block < sb.size; block++) {
    if (bitmap_allocated(block) && !used[block]) {
        printf("ERROR: block %u is allocated but not used\n", block);
        errors++;
    }
}
    printf("\nFilesystem check complete.\n");

    if (errors == 0)
        printf("No consistency errors detected.\n");
    else
        printf("Found %d consistency error(s).\n", errors);

    free(used);
    close(fd);

    return errors ? 1 : 0;
}
