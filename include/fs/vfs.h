#ifndef FS_VFS_H
#define FS_VFS_H

#include <stdint.h>

/* Longest filename the VFS carries, including the terminator. Fixed rather than
 * dynamically sized so a node is one flat allocation. */
#define VFS_NAME_MAX 64

/* Node kinds. Kept as flags rather than an enum because a node may later be
 * more than one thing (a mount point is a directory too). */
#define FS_FILE      0x01u
#define FS_DIRECTORY 0x02u

struct fs_node;

/* One entry returned by readdir. Deliberately thin: it names a child and gives
 * its identity, and the caller uses finddir to obtain the node itself. */
struct dirent {
    char     name[VFS_NAME_MAX];
    uint32_t inode;
};

/* Every operation takes the node it acts on, which is what lets one
 * implementation serve many nodes -- the initrd backs every file in the image
 * with a single read function. */
typedef uint32_t (*fs_read_t)(struct fs_node *node, uint32_t offset, uint32_t size,
                              uint8_t *buffer);
typedef uint32_t (*fs_write_t)(struct fs_node *node, uint32_t offset, uint32_t size,
                               const uint8_t *buffer);
typedef void (*fs_open_t)(struct fs_node *node);
typedef void (*fs_close_t)(struct fs_node *node);
typedef struct dirent *(*fs_readdir_t)(struct fs_node *node, uint32_t index);
typedef struct fs_node *(*fs_finddir_t)(struct fs_node *node, const char *name);

typedef struct fs_node {
    char     name[VFS_NAME_MAX];
    uint32_t flags;  /* FS_FILE or FS_DIRECTORY */
    uint32_t length; /* bytes; zero for a directory */
    uint32_t inode;  /* driver-private identity, opaque to the VFS */

    fs_read_t    read;
    fs_write_t   write;
    fs_open_t    open;
    fs_close_t   close;
    fs_readdir_t readdir;
    fs_finddir_t finddir;
} fs_node_t;

/* The root of the mounted hierarchy. Null until a driver mounts something. */
extern fs_node_t *fs_root;

/* Wrappers around the function-pointer table. Each checks the node and the
 * relevant slot before dispatching, so a driver that does not implement an
 * operation leaves it null and callers get a clean failure instead of a jump
 * through a null pointer. This is what lets one call site work against any
 * filesystem. */
uint32_t vfs_read(fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);
uint32_t vfs_write(fs_node_t *node, uint32_t offset, uint32_t size, const uint8_t *buffer);
void     vfs_open(fs_node_t *node);
void     vfs_close(fs_node_t *node);

/* Returns the index-th entry of a directory, or null past the end or when the
 * node is not a directory. */
struct dirent *vfs_readdir(fs_node_t *node, uint32_t index);

/* Returns the named child of a directory, or null if absent. */
fs_node_t *vfs_finddir(fs_node_t *node, const char *name);

#endif /* FS_VFS_H */
