#ifndef TASK_ELF_H
#define TASK_ELF_H

#include <stdbool.h>
#include <stdint.h>

/* What a successful load produced. The address space is complete but empty of
 * a stack: create_user_process adds that, because a stack is a property of a
 * thread rather than of the image. */
typedef struct {
    uint32_t entry;     /* e_entry, validated to lie inside a mapped segment */
    uint32_t directory; /* physical address of the process's page directory  */
    uint32_t segments;  /* PT_LOAD headers actually mapped                   */
    uint32_t pages;     /* 4 KiB frames allocated for them                   */
    uint32_t file_bytes;/* bytes copied out of the image                     */
    uint32_t bss_bytes; /* p_memsz - p_filesz, summed: memory with no file   */
} elf_image_t;

/* Parses an ELF32 executable already in memory and builds a fresh address
 * space for it: every PT_LOAD segment gets its own frames, zero-filled and
 * then overwritten with the file's bytes, mapped user-accessible at p_vaddr.
 *
 * `size` bounds the image. Nothing in the file is trusted -- every offset,
 * length and virtual address is checked against it and against the user
 * window before a byte is read or a page is mapped.
 *
 * Returns false and leaves nothing allocated on any failure. On success the
 * caller owns out->directory and must hand it to create_user_process or
 * paging_destroy_address_space. */
bool elf_load(const void *image, uint32_t size, elf_image_t *out);

#endif /* TASK_ELF_H */
