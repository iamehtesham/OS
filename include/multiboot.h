#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

/* Multiboot Specification 0.6.96. */

/* Value the loader leaves in EAX before jumping to the entry point (section
 * 3.2). Note it differs from the 0x1BADB002 signature in our own header. */
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002u

/* Bits in multiboot_info.flags saying which of the following fields the loader
 * actually filled in. Reading a field whose bit is clear yields garbage. */
#define MULTIBOOT_INFO_MEMORY           (1u << 0) /* mem_lower / mem_upper valid   */
#define MULTIBOOT_INFO_CMDLINE          (1u << 2) /* cmdline valid                 */
#define MULTIBOOT_INFO_MODS             (1u << 3) /* mods_count / mods_addr valid  */
#define MULTIBOOT_INFO_MMAP             (1u << 6) /* mmap_length / mmap_addr valid */
#define MULTIBOOT_INFO_BOOT_LOADER_NAME (1u << 9) /* boot_loader_name valid        */

/* Memory map entry types (section 3.3). Only type 1 is usable RAM. */
#define MULTIBOOT_MEMORY_AVAILABLE        1
#define MULTIBOOT_MEMORY_RESERVED         2
#define MULTIBOOT_MEMORY_ACPI_RECLAIMABLE 3
#define MULTIBOOT_MEMORY_NVS              4
#define MULTIBOOT_MEMORY_BADRAM           5

struct multiboot_info {
    uint32_t flags;

    /* Present when MULTIBOOT_INFO_MEMORY is set, both in KiB. mem_upper counts
     * from 1 MiB and is capped at the first memory hole, so it is not a
     * reliable total -- the memory map is. */
    uint32_t mem_lower;
    uint32_t mem_upper;

    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;

    /* Union of the a.out symbol table and the ELF section header table. */
    uint32_t syms[4];

    /* Present when MULTIBOOT_INFO_MMAP is set. mmap_addr points at a buffer of
     * mmap_length bytes holding a chain of multiboot_mmap_entry. */
    uint32_t mmap_length;
    uint32_t mmap_addr;

    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
} __attribute__((packed));

/* One per loaded module, in an array of mods_count at mods_addr. mod_end is
 * exclusive, so the payload is [mod_start, mod_end). The loader typically parks
 * this array and the payloads immediately above the kernel image, which is
 * precisely where anything the kernel places by itself would otherwise land. */
struct multiboot_mod_list {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t cmdline;
    uint32_t pad;
} __attribute__((packed));

/* Memory map entries are variable length. `size` counts the bytes that FOLLOW
 * it, so advancing to the next entry means adding size + sizeof(size) -- not
 * sizeof(struct multiboot_mmap_entry), which would drift on any loader that
 * emits a larger entry. */
struct multiboot_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed));

#endif /* MULTIBOOT_H */
