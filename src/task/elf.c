#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "format/elf.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "task/elf.h"
#include "utils/stdio.h"
#include "utils/string.h"

/* Loading an executable the kernel did not produce.
 *
 * Everything in the file is attacker-controlled as far as this code is
 * concerned -- it arrived in the initrd, and the initrd arrived from the boot
 * loader. So every offset is checked against the image size before it is
 * dereferenced, every virtual address against the user window before it is
 * mapped, and every arithmetic sum against overflow before it is compared. A
 * header claiming p_offset near 2^32 must fail a bounds test, not wrap past
 * it. */

static void reject(const char *reason)
{
    kprintf("elf: %s\n", reason);
}

/* True when [offset, offset + length) lies wholly inside a file of `size`
 * bytes, with the wrap case rejected rather than accepted by accident. */
static bool within_file(uint32_t offset, uint32_t length, uint32_t size)
{
    if (offset > size) {
        return false;
    }

    return length <= size - offset;
}

static bool header_is_sane(const Elf32_Ehdr *header, uint32_t size)
{
    if (header->e_ident[EI_MAG0] != ELFMAG0 || header->e_ident[EI_MAG1] != ELFMAG1 ||
        header->e_ident[EI_MAG2] != ELFMAG2 || header->e_ident[EI_MAG3] != ELFMAG3) {
        reject("not an ELF file (bad magic)");
        return false;
    }

    if (header->e_ident[EI_CLASS] != ELFCLASS32) {
        reject("not a 32-bit object");
        return false;
    }

    if (header->e_ident[EI_DATA] != ELFDATA2LSB) {
        reject("not little endian");
        return false;
    }

    if (header->e_type != ET_EXEC) {
        /* A shared object would need relocation processing and an interpreter,
         * neither of which exists here. */
        reject("not a fixed-address executable (ET_EXEC)");
        return false;
    }

    if (header->e_machine != EM_386) {
        reject("not an i386 object");
        return false;
    }

    if (header->e_version != EV_CURRENT) {
        reject("unknown ELF version");
        return false;
    }

    if (header->e_phnum == 0) {
        reject("no program headers, so nothing to load");
        return false;
    }

    /* The loader indexes the table with this stride, so it has to be the
     * stride the file was written with. */
    if (header->e_phentsize != sizeof(Elf32_Phdr)) {
        reject("unexpected program header size");
        return false;
    }

    if (!within_file(header->e_phoff, (uint32_t)header->e_phnum * header->e_phentsize, size)) {
        reject("program header table runs past the end of the file");
        return false;
    }

    return true;
}

/* Maps one PT_LOAD segment into `directory` and fills it in.
 *
 * The zero-then-copy order is what makes .bss free: every frame is cleared in
 * full when it is allocated and only p_filesz bytes are written over it, so
 * the [p_filesz, p_memsz) gap and the slack past the end of the last page are
 * already zero without being special cases. It is also what stops a recycled
 * frame from handing the process whatever its previous owner left there. */
static bool load_segment(uint32_t directory, const uint8_t *file, uint32_t size,
                         const Elf32_Phdr *ph, elf_image_t *out)
{
    if (ph->p_memsz == 0) {
        return true; /* occupies no memory; nothing to map */
    }

    /* The file cannot claim to hold more bytes than the segment occupies. */
    if (ph->p_filesz > ph->p_memsz) {
        reject("segment claims more file bytes than memory bytes");
        return false;
    }

    if (!within_file(ph->p_offset, ph->p_filesz, size)) {
        reject("segment contents run past the end of the file");
        return false;
    }

    /* Overflow first, range second: the sum below is meaningless if it wraps. */
    if (ph->p_vaddr > UINT32_MAX - ph->p_memsz) {
        reject("segment wraps the address space");
        return false;
    }

    if (ph->p_vaddr < USER_IMAGE_BASE || ph->p_vaddr + ph->p_memsz > USER_IMAGE_LIMIT) {
        /* Not merely a policy check. Kernel page tables are shared into this
         * directory, so a mapping at a kernel address would edit the kernel's
         * own table -- in every address space at once. */
        reject("segment lies outside the user window");
        return false;
    }

    const uint32_t flags =
        PAGE_USER | (((ph->p_flags & PF_W) != 0) ? PAGE_WRITABLE : 0u);

    const uint32_t first_page = ph->p_vaddr & PAGE_FRAME_MASK;
    const uint32_t last_page  = (ph->p_vaddr + ph->p_memsz - 1u) & PAGE_FRAME_MASK;

    for (uint32_t page = first_page;; page += PAGE_SIZE) {
        const uint32_t existing = paging_entry_in(directory, page);
        uint32_t       frame;

        if ((existing & PAGE_PRESENT) != 0) {
            /* Another segment already owns this page -- two segments sharing
             * one page happens when they are not page aligned. Reuse its
             * frame: allocating a second one would map away the bytes already
             * written, and zeroing it would erase them. Permissions widen to
             * satisfy both segments, since one page cannot have two. */
            frame = existing & PAGE_FRAME_MASK;

            if (!paging_map_in(directory, frame, page, flags | (existing & PAGE_WRITABLE))) {
                reject("could not widen a shared page");
                return false;
            }
        } else {
            void *const allocated = pmm_alloc_block();

            if (allocated == NULL) {
                reject("out of physical memory");
                return false;
            }

            frame = (uint32_t)(uintptr_t)allocated;

            /* The kernel fills this frame by physical address, so it has to be
             * inside the identity map. */
            if (!paging_frame_is_reachable(frame)) {
                pmm_free_block(allocated);
                reject("allocated frame is outside the identity map");
                return false;
            }

            kmemset(allocated, 0, PAGE_SIZE);

            if (!paging_map_in(directory, frame, page, flags)) {
                pmm_free_block(allocated);
                reject("could not map a segment page");
                return false;
            }

            out->pages++;
        }

        /* Copy whatever part of this page the file actually backs. Writes go
         * through the identity map to the frame's physical address: the
         * address space being built is not the one that is active, so p_vaddr
         * is not addressable from here. */
        const uint32_t file_end   = ph->p_vaddr + ph->p_filesz;
        const uint32_t copy_start = (page > ph->p_vaddr) ? page : ph->p_vaddr;
        const uint32_t page_end   = page + PAGE_SIZE;
        const uint32_t copy_end   = (page_end < file_end) ? page_end : file_end;

        if (copy_end > copy_start) {
            kmemcpy((void *)(uintptr_t)(frame + (copy_start - page)),
                    file + ph->p_offset + (copy_start - ph->p_vaddr),
                    copy_end - copy_start);
        }

        if (page >= last_page) {
            break;
        }
    }

    out->segments++;
    out->file_bytes += ph->p_filesz;
    out->bss_bytes += ph->p_memsz - ph->p_filesz;

    return true;
}

bool elf_load(const void *image, uint32_t size, elf_image_t *out)
{
    if (image == NULL || out == NULL) {
        return false;
    }

    if (size < sizeof(Elf32_Ehdr)) {
        reject("file is smaller than an ELF header");
        return false;
    }

    const uint8_t *const    file   = (const uint8_t *)image;
    const Elf32_Ehdr *const header = (const Elf32_Ehdr *)image;

    if (!header_is_sane(header, size)) {
        return false;
    }

    const uint32_t directory = paging_create_address_space();

    if (directory == 0) {
        reject("could not create an address space");
        return false;
    }

    out->entry      = header->e_entry;
    out->directory  = directory;
    out->segments   = 0;
    out->pages      = 0;
    out->file_bytes = 0;
    out->bss_bytes  = 0;

    for (uint32_t i = 0; i < header->e_phnum; i++) {
        const Elf32_Phdr *const ph =
            (const Elf32_Phdr *)(file + header->e_phoff + i * header->e_phentsize);

        /* Everything else describes the file, the stack, or a dynamic linker
         * this kernel does not have. Only PT_LOAD occupies memory. */
        if (ph->p_type != PT_LOAD) {
            continue;
        }

        if (!load_segment(directory, file, size, ph, out)) {
            paging_destroy_address_space(directory);
            return false;
        }
    }

    if (out->segments == 0) {
        reject("no PT_LOAD segments");
        paging_destroy_address_space(directory);
        return false;
    }

    /* Ask the page tables rather than the header: the entry point has to be a
     * page this load actually mapped user-accessible, whatever e_entry claims.
     * Otherwise the first instruction fetch faults in ring 3 and the process
     * is dead before it starts.
     *
     * Both levels are tested, exactly as the MMU tests them. Reading the page
     * table entry alone would accept an entry point sitting in a table shared
     * from the kernel, whose user bit says yes while the directory entry
     * reaching it says no. */
    if (!paging_user_can_read_in(directory, out->entry)) {
        reject("entry point is not in a mapped user segment");
        paging_destroy_address_space(directory);
        return false;
    }

    return true;
}
