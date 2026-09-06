#ifndef FORMAT_ELF_H
#define FORMAT_ELF_H

#include <stdint.h>

/* ELF32 as defined by the System V ABI, i386 supplement.
 *
 * Only what an executable loader needs is here: the file header and the
 * program header. Section headers describe the file for a linker and a
 * debugger, not for loading -- at run time the program headers are the whole
 * story, which is why a stripped binary still executes. */

typedef uint32_t Elf32_Addr;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Off;
typedef uint32_t Elf32_Word;
typedef int32_t  Elf32_Sword;

#define EI_NIDENT 16u

/* Indices into e_ident. */
#define EI_MAG0    0u
#define EI_MAG1    1u
#define EI_MAG2    2u
#define EI_MAG3    3u
#define EI_CLASS   4u
#define EI_DATA    5u
#define EI_VERSION 6u

/* The four magic bytes: 0x7F 'E' 'L' 'F'. */
#define ELFMAG0 0x7Fu
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

#define ELFCLASS32   1u /* 32-bit objects       */
#define ELFDATA2LSB  1u /* little endian        */
#define EV_CURRENT   1u /* the only ELF version */

#define ET_EXEC 2u /* an executable, already linked to fixed addresses */
#define EM_386  3u /* Intel 80386                                     */

typedef struct {
    uint8_t    e_ident[EI_NIDENT]; /* magic, class, endianness, version */
    Elf32_Half e_type;             /* ET_EXEC for what this loader runs  */
    Elf32_Half e_machine;          /* EM_386                             */
    Elf32_Word e_version;
    Elf32_Addr e_entry;            /* where execution starts             */
    Elf32_Off  e_phoff;            /* program header table, from file start */
    Elf32_Off  e_shoff;            /* section header table; unused here  */
    Elf32_Word e_flags;
    Elf32_Half e_ehsize;
    Elf32_Half e_phentsize;        /* bytes per program header           */
    Elf32_Half e_phnum;            /* how many program headers           */
    Elf32_Half e_shentsize;
    Elf32_Half e_shnum;
    Elf32_Half e_shstrndx;
} __attribute__((packed)) Elf32_Ehdr;

/* Segment types. Only PT_LOAD occupies memory; the rest describe the file or
 * the dynamic linker, neither of which this kernel has. */
#define PT_NULL    0u
#define PT_LOAD    1u
#define PT_DYNAMIC 2u
#define PT_INTERP  3u
#define PT_NOTE    4u
#define PT_PHDR    6u

/* Segment permissions. There is no NX bit in 32-bit non-PAE paging, so a
 * readable page is executable whether or not PF_X is set. */
#define PF_X 0x1u
#define PF_W 0x2u
#define PF_R 0x4u

typedef struct {
    Elf32_Word p_type;
    Elf32_Off  p_offset; /* where the segment's bytes start in the file      */
    Elf32_Addr p_vaddr;  /* where they must appear in memory                 */
    Elf32_Addr p_paddr;  /* physical address; meaningless with paging        */
    Elf32_Word p_filesz; /* bytes stored in the file                         */
    Elf32_Word p_memsz;  /* bytes occupied in memory; the excess is .bss     */
    Elf32_Word p_flags;  /* PF_R / PF_W / PF_X                               */
    Elf32_Word p_align;
} __attribute__((packed)) Elf32_Phdr;

/* The on-disk sizes are fixed by the ABI. If a struct ever grows padding the
 * loader would read every field at the wrong offset, so fail the build. */
_Static_assert(sizeof(Elf32_Ehdr) == 52, "Elf32_Ehdr must be 52 bytes");
_Static_assert(sizeof(Elf32_Phdr) == 32, "Elf32_Phdr must be 32 bytes");

#endif /* FORMAT_ELF_H */
