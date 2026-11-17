# xv6 FUSE Filesystem

A userspace filesystem that mounts an xv6 filesystem image on Linux using FUSE3. It allows users to inspect and modify files and directories stored in an xv6 disk image.

## Features

- Reads xv6 superblock, inode and directory structures.
- FUSE operations: getattr, readdir and read.
- File operations: write, create, unlink and truncate.
- Directory creation using mkdir.
- Direct and single-indirect block support.
- File transfer between Linux and the mounted xv6 filesystem.
- Basic FSCK-style consistency checker.

## Requirements

- Linux
- GCC
- FUSE3 development library
- pkg-config

On Ubuntu, install dependencies with:

    sudo apt update
    sudo apt install build-essential fuse3 libfuse3-dev pkg-config

## Build

From the project directory:

    gcc -Wall -Wextra -o xv6_fuse src/xv6_fuse.c $(pkg-config fuse3 --cflags --libs)
    gcc -Wall -Wextra -o xv6_fsck src/xv6_fsck.c
    gcc -Wall -Wextra -o inspect src/inspect.c
    gcc -Wall -Wextra -o inode_inspect src/inode_inspect.c
    gcc -Wall -Wextra -o directory_inspect src/directory_inspect.c

## Usage

Create a mount point:

    mkdir -p ~/xv6-mount

Mount an xv6 filesystem image:

    ./xv6_fuse /path/to/fs.img ~/xv6-mount

Access files through the mount point using normal Linux commands.

Unmount:

    fusermount3 -u ~/xv6-mount

## Filesystem checker

Run the basic consistency checker:

    ./xv6_fsck /path/to/fs.img

The checker validates the superblock, inode information, block references, duplicate block usage and allocated-but-unreferenced data blocks.

## Project Structure

- include/xv6_fs.h - xv6 filesystem structure definitions.
- src/xv6_fuse.c - FUSE filesystem implementation.
- src/xv6_fsck.c - filesystem consistency checker.
- src/inspect.c - superblock inspection utility.
- src/inode_inspect.c - inode inspection utility.
- src/directory_inspect.c - directory inspection utility.

## Limitations

- xv6 directory names are limited by DIRSIZ (14 characters).
- Supports direct and single-indirect blocks; double-indirect blocks are not implemented.
- The consistency checker performs basic checks and is not a complete filesystem repair utility.
- File timestamps and some advanced filesystem operations are not implemented.
- Directory link-count bookkeeping requires further validation.

