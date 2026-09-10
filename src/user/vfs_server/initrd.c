#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fs/initrd.h"
#include "fs/vfs.h"
#include "user/ulib.h"

/* The read-only initrd driver, now running at CPL 3 inside the VFS server.
 *
 * Two things changed when it left the kernel. There is no kmalloc out here, so
 * the nodes live in the server's own .bss -- which the ELF loader allocates and
 * zero-fills, so a fixed array costs nothing until it is touched. And there is
 * no kprintf, so failures are reported through the print system call. Nothing
 * else about the driver had to move: it was already reading a flat image out of
 * memory and answering through a function-pointer table, and that works exactly
 * the same in ring 3 once the image has been mapped. */

/* A flat image with a fixed node array needs a ceiling. Sized generously
 * against an initrd that is packed by hand at build time. */
#define INITRD_MAX_FILES 32u

static const struct initrd_file_header *headers;
static uint32_t                         file_count;
static uint32_t                         image_base;
static uint32_t                         image_size;

static fs_node_t root_node;
static fs_node_t file_nodes[INITRD_MAX_FILES];

/* readdir hands back a pointer rather than filling a caller buffer, so the
 * entry lives here. Safe only because the server answers one request at a time;
 * a second concurrent reader would see this overwritten. */
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

    u_memcpy(buffer, src, size);

    return size;
}

/* The image is a fixed read-only blob in RAM, and the pages it was mapped into
 * carry no write permission at all, so there is nowhere to write even if the
 * driver wanted to. Zero bytes written is the error. */
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

    u_strncpy(readdir_entry.name, file_nodes[index].name, VFS_NAME_MAX);
    readdir_entry.inode = index;

    return &readdir_entry;
}

static fs_node_t *initrd_finddir(fs_node_t *node, const char *name)
{
    (void)node;

    for (uint32_t i = 0; i < file_count; i++) {
        if (u_strcmp(file_nodes[i].name, name) == 0) {
            return &file_nodes[i];
        }
    }

    return NULL;
}

fs_node_t *initrd_init(uint32_t location, uint32_t size)
{
    if (size < sizeof(struct initrd_superblock)) {
        u_print("  [vfs] initrd image is too small\n");
        return NULL;
    }

    const struct initrd_superblock *const super =
        (const struct initrd_superblock *)(uintptr_t)location;

    if (super->magic != INITRD_MAGIC) {
        u_print("  [vfs] initrd has bad magic\n");
        return NULL;
    }

    /* Bound the header array before walking it. Written as a division rather
     * than a multiply so a huge file_count cannot overflow into a small
     * product and pass the check. */
    const uint32_t header_space = size - (uint32_t)sizeof(struct initrd_superblock);

    if (super->file_count > header_space / (uint32_t)sizeof(struct initrd_file_header)) {
        u_print("  [vfs] initrd file count does not fit the image\n");
        return NULL;
    }

    /* The node array is fixed now that there is no heap, so the image has to
     * fit it. Refusing is the honest answer: mounting the first 32 and
     * pretending the rest do not exist would make finddir lie. */
    if (super->file_count > INITRD_MAX_FILES) {
        u_print("  [vfs] initrd holds more files than the server can mount\n");
        return NULL;
    }

    headers = (const struct initrd_file_header *)(uintptr_t)(
        location + sizeof(struct initrd_superblock));
    file_count = super->file_count;
    image_base = location;
    image_size = size;

    /* The image came from the boot loader by way of a physical mapping the
     * kernel granted, so every span it claims is still checked against the
     * image before any of it is dereferenced. Being in ring 3 does not make
     * the data more trustworthy -- it makes a mistake cheaper. */
    for (uint32_t i = 0; i < file_count; i++) {
        if (headers[i].magic != INITRD_MAGIC) {
            u_print("  [vfs] initrd entry has bad magic\n");
            return NULL;
        }

        if (headers[i].offset > size || headers[i].length > size - headers[i].offset) {
            u_print("  [vfs] initrd entry runs past the end of the image\n");
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
            u_print("  [vfs] initrd entry has an unterminated name\n");
            return NULL;
        }
    }

    u_strncpy(root_node.name, "/", VFS_NAME_MAX);
    root_node.flags   = FS_DIRECTORY;
    root_node.length  = 0;
    root_node.inode   = 0;
    root_node.read    = NULL; /* a directory has no byte stream to read */
    root_node.write   = NULL;
    root_node.open    = initrd_open;
    root_node.close   = initrd_close;
    root_node.readdir = initrd_readdir;
    root_node.finddir = initrd_finddir;

    for (uint32_t i = 0; i < file_count; i++) {
        u_strncpy(file_nodes[i].name, headers[i].name, VFS_NAME_MAX);
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

    return &root_node;
}

uint32_t initrd_file_count(void)
{
    return file_count;
}

fs_node_t *initrd_node_by_inode(uint32_t inode)
{
    if (inode >= file_count) {
        return NULL;
    }

    return &file_nodes[inode];
}

uint32_t initrd_image_size(void)
{
    return image_size;
}
