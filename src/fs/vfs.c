#include <stddef.h>
#include <stdint.h>

#include "fs/vfs.h"

fs_node_t *fs_root = NULL;

uint32_t vfs_read(fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer)
{
    if (node == NULL || node->read == NULL || buffer == NULL) {
        return 0;
    }

    return node->read(node, offset, size, buffer);
}

uint32_t vfs_write(fs_node_t *node, uint32_t offset, uint32_t size, const uint8_t *buffer)
{
    if (node == NULL || node->write == NULL || buffer == NULL) {
        return 0;
    }

    return node->write(node, offset, size, buffer);
}

void vfs_open(fs_node_t *node)
{
    if (node != NULL && node->open != NULL) {
        node->open(node);
    }
}

void vfs_close(fs_node_t *node)
{
    if (node != NULL && node->close != NULL) {
        node->close(node);
    }
}

struct dirent *vfs_readdir(fs_node_t *node, uint32_t index)
{
    /* The directory check lives here rather than in every driver: a regular
     * file simply leaves readdir null, and asking for its contents fails
     * cleanly instead of dispatching into something that cannot serve it. */
    if (node == NULL || node->readdir == NULL || (node->flags & FS_DIRECTORY) == 0) {
        return NULL;
    }

    return node->readdir(node, index);
}

fs_node_t *vfs_finddir(fs_node_t *node, const char *name)
{
    if (node == NULL || node->finddir == NULL || name == NULL ||
        (node->flags & FS_DIRECTORY) == 0) {
        return NULL;
    }

    return node->finddir(node, name);
}
