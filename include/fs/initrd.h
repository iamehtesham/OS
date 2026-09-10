#ifndef FS_INITRD_H
#define FS_INITRD_H

#include <stdint.h>

#include "fs/vfs.h"

/* On-disk format. tools/make_initrd.py writes exactly this layout, so the two
 * must be changed together -- the script asserts its struct sizes against the
 * numbers below.
 *
 * Layout:   [superblock][file_count * file_header][file data ...]
 *
 * Offsets in a file header are measured from the START of the image, so the
 * driver needs nothing but the image's base address to resolve them. */

#define INITRD_MAGIC    0x494E5244u /* 'INRD' */
#define INITRD_NAME_MAX 64u

/* A name fills at most INITRD_NAME_MAX bytes including its terminator, so it
 * must fit the VFS name field or every node would be truncated on mount. */
_Static_assert(INITRD_NAME_MAX <= VFS_NAME_MAX,
               "initrd names must fit in a VFS node name");

struct initrd_superblock {
    uint32_t magic;
    uint32_t file_count;
} __attribute__((packed));

struct initrd_file_header {
    uint32_t magic;                  /* repeated per entry so a truncated or
                                      * misaligned image is caught per file */
    char     name[INITRD_NAME_MAX];
    uint32_t offset;                 /* from the start of the image */
    uint32_t length;
} __attribute__((packed));

/* Parses an image already in memory and returns its root directory node, or
 * null if the image is malformed or nodes could not be allocated. The caller
 * supplies the size so every offset can be bounds-checked -- the image comes
 * from the boot loader and is not trusted to be well formed. */
fs_node_t *initrd_init(uint32_t location, uint32_t size);

uint32_t initrd_file_count(void);

/* Resolves the inode a previous lookup handed out. The server gives a client
 * an inode rather than a pointer, because a pointer into the server's address
 * space would mean nothing in the client's -- and would be a capability the
 * client could forge if it did. */
fs_node_t *initrd_node_by_inode(uint32_t inode);

uint32_t initrd_image_size(void);

#endif /* FS_INITRD_H */
