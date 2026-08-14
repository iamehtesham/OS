#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fs/initrd.h"
#include "fs/vfs.h"
#include "mm/kheap.h"
#include "utils/stdio.h"
#include "utils/string.h"

static const struct initrd_file_header *headers;
static uint32_t                         file_count;
static uint32_t                         image_base;
static fs_node_t                       *root_node;
static fs_node_t                       *file_nodes;

/* readdir hands back a pointer rather than filling a caller buffer, so the
 * entry lives here. Safe only because the kernel is single-threaded and never
 * re-enters the VFS from an interrupt; a second concurrent reader would see
 * this overwritten. */
static struct dirent readdir_entry;

static uint32_t initrd_read(fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer)
{
    if ((node->flags & FS_FILE) == 0 || node->inode >= file_count) {
        return 0;
    }

    const struct initrd_file_header *const header = &headers[node->inode];

    if (offset >= header->length) {
        return 0;
    }

    /* Clamp rather than refuse: a caller reading in fixed-size chunks should
     * get the short final chunk, not an error. The return value says how much
     * was actually delivered. */
    const uint32_t available = header->length - offset;

    if (size > available) {
        size = available;
    }

    const uint8_t *const src =
        (const uint8_t *)(uintptr_t)(image_base + header->offset + offset);

    for (uint32_t i = 0; i < size; i++) {
        buffer[i] = src[i];
    }

    return size;
}

/* The image is a fixed read-only blob in RAM; there is nowhere to write. Zero
 * bytes written is the error, which is why callers must check the return value
 * rather than assume success. */
static uint32_t initrd_write(fs_node_t *node, uint32_t offset, uint32_t size,
                             const uint8_t *buffer)
{
    (void)node;
    (void)offset;
    (void)size;
    (void)buffer;

    return 0;
}

/* Nothing to reference-count, lock or flush for a fixed image. */
static void initrd_open(fs_node_t *node)
{
    (void)node;
}

static void initrd_close(fs_node_t *node)
{
    (void)node;
}

static struct dirent *initrd_readdir(fs_node_t *node, uint32_t index)
{
    (void)node; /* the initrd is flat: the root is the only directory */

    if (index >= file_count) {
        return NULL;
    }

    kstrncpy(readdir_entry.name, file_nodes[index].name, VFS_NAME_MAX);
    readdir_entry.inode = index;

    return &readdir_entry;
}

static fs_node_t *initrd_finddir(fs_node_t *node, const char *name)
{
    (void)node;

    for (uint32_t i = 0; i < file_count; i++) {
        if (kstrcmp(file_nodes[i].name, name) == 0) {
            return &file_nodes[i];
        }
    }

    return NULL;
}

fs_node_t *initrd_init(uint32_t location, uint32_t size)
{
    if (size < sizeof(struct initrd_superblock)) {
        kprintf("initrd: image too small (%u B)\n", size);
        return NULL;
    }

    const struct initrd_superblock *const super =
        (const struct initrd_superblock *)(uintptr_t)location;

    if (super->magic != INITRD_MAGIC) {
        kprintf("initrd: bad magic 0x%x\n", super->magic);
        return NULL;
    }

    /* Bound the header array before walking it. Written as a division rather
     * than a multiply so a huge file_count cannot overflow into a small
     * product and pass the check. */
    const uint32_t header_space = size - (uint32_t)sizeof(struct initrd_superblock);

    if (super->file_count > header_space / (uint32_t)sizeof(struct initrd_file_header)) {
        kprintf("initrd: file count %u does not fit the image\n", super->file_count);
        return NULL;
    }

    headers = (const struct initrd_file_header *)(uintptr_t)(
        location + sizeof(struct initrd_superblock));
    file_count = super->file_count;
    image_base = location;

    /* The image came from the boot loader, so every span it claims is checked
     * against the image before any of it is dereferenced. */
    for (uint32_t i = 0; i < file_count; i++) {
        if (headers[i].magic != INITRD_MAGIC) {
            kprintf("initrd: entry %u has bad magic\n", i);
            return NULL;
        }

        if (headers[i].offset > size || headers[i].length > size - headers[i].offset) {
            kprintf("initrd: entry %u runs past the end of the image\n", i);
            return NULL;
        }

        /* The name is copied into a fixed-width VFS field, so one that fills
         * all INITRD_NAME_MAX bytes without a terminator would be truncated to
         * fit. Two entries differing only in that last byte would then collapse
         * onto the same node name and finddir would hand back the first for
         * both -- a lookup using the name readdir advertised would silently
         * return the wrong file. Require the terminator to be in the image. */
        bool terminated = false;

        for (uint32_t n = 0; n < INITRD_NAME_MAX; n++) {
            if (headers[i].name[n] == '\0') {
                terminated = true;
                break;
            }
        }

        if (!terminated) {
            kprintf("initrd: entry %u has an unterminated name\n", i);
            return NULL;
        }
    }

    root_node = kmalloc((uint32_t)sizeof(fs_node_t));

    if (root_node == NULL) {
        kprintf("initrd: could not allocate the root node\n");
        return NULL;
    }

    /* One allocation for all the file nodes rather than one each: they share a
     * lifetime, and a single block costs one header instead of file_count. */
    file_nodes = NULL;

    if (file_count > 0) {
        file_nodes = kmalloc(file_count * (uint32_t)sizeof(fs_node_t));

        if (file_nodes == NULL) {
            kprintf("initrd: could not allocate %u file nodes\n", file_count);
            kfree(root_node);
            root_node = NULL;
            return NULL;
        }
    }

    kstrncpy(root_node->name, "/", VFS_NAME_MAX);
    root_node->flags   = FS_DIRECTORY;
    root_node->length  = 0;
    root_node->inode   = 0;
    root_node->read    = NULL; /* a directory has no byte stream to read */
    root_node->write   = NULL;
    root_node->open    = initrd_open;
    root_node->close   = initrd_close;
    root_node->readdir = initrd_readdir;
    root_node->finddir = initrd_finddir;

    for (uint32_t i = 0; i < file_count; i++) {
        kstrncpy(file_nodes[i].name, headers[i].name, VFS_NAME_MAX);
        file_nodes[i].flags  = FS_FILE;
        file_nodes[i].length = headers[i].length;
        file_nodes[i].inode  = i;
        file_nodes[i].read   = initrd_read;
        file_nodes[i].write  = initrd_write;
        file_nodes[i].open   = initrd_open;
        file_nodes[i].close  = initrd_close;

        /* Files are not directories, so these stay null and the VFS wrappers
         * turn a readdir on a file into a clean null rather than a bad call. */
        file_nodes[i].readdir = NULL;
        file_nodes[i].finddir = NULL;
    }

    return root_node;
}

uint32_t initrd_file_count(void)
{
    return file_count;
}
