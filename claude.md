# Custom OS Development Rules

## Tech Stack & Architecture
*   **Target Architecture:** x86 (i386, 32-bit protected mode)
*   **Bootloader:** GRUB / Multiboot 1. QEMU's own `-kernel` Multiboot loader is used for day-to-day boots, so no ISO or disk image is built.
*   **Languages:** Bare-metal C (freestanding environment) and GNU Assembly (GAS syntax, `.S` files assembled through the GCC driver).
*   **Build System:** Makefile.

## Toolchain
*   **Compiler:** host `gcc` with `-m32`. There is no `i686-elf` cross-compiler installed; NASM is not installed either, which is why assembly is GAS rather than Intel syntax.
*   **Linking:** `ld -m elf_i386 -T linker.ld` invoked **directly**, never through the `gcc` driver — the driver's Linux specs would pull in crt0/libc and force PIE.
*   **Required flags:** `-ffreestanding -fno-pie -fno-pic -fno-stack-protector`. Dropping any of these yields an image that will not boot.
*   **Load address:** 1 MiB (`0x100000`), set in `linker.ld`.

## Source Layout
Headers mirror the source tree and are included by subsystem path (`#include "drivers/vga.h"`), resolved through `-Iinclude`.

```
src/boot.S            Multiboot header, _start, stack, call into kernel_main
src/kernel.c          kernel_main
src/cpu/gdt.c         flat 3-entry GDT (null, kernel code 0x08, kernel data 0x10)
src/cpu/gdt_flush.S   lgdt + segment reload + far jump to reload CS
src/cpu/idt.c         256-entry IDT: vectors 0-31 exceptions, 32-47 IRQs
src/cpu/idt_load.S    lidt
src/cpu/isr_stubs.S   exception entry points + interrupt_common_stub (shared tail)
src/cpu/isr.c         interrupt_dispatch routing, and isr_handler for exceptions
src/cpu/irq_stubs.S   IRQ 0-15 entry points, joining the same shared tail
src/cpu/irq.c         handler registration, dispatch, and central EOI
src/cpu/pic.c         8259 remap to vectors 32-47, masking, EOI
src/drivers/keyboard.c PS/2 IRQ1 handler, scancode set 1 -> ASCII
src/drivers/vga.c     80x25 text buffer, colours, hardware cursor, scrolling
src/mm/pmm.c          bitmap physical frame allocator (4 KiB blocks)
src/mm/paging.c       two-level page tables, map_page, identity map
src/mm/paging_enable.S  loads CR3 and sets CR0.PG
include/multiboot.h   Multiboot 1 info and memory-map structures
src/utils/stdio.c     kprintf / kvprintf and integer conversion
src/utils/string.c    kstrlen and friends
include/arch/io.h     inb / outb port I/O primitives
```

`struct registers` in `include/cpu/isr.h` is a direct view onto the stack frame built by `isr_stubs.S`. **Changing the push sequence in the assembly without changing the struct (or vice versa) silently misreports registers** — the two must be edited together.

Both exception and IRQ stubs jump to the **same** `interrupt_common_stub`, so the save/restore sequence exists once. `interrupt_dispatch` then routes on vector number: below 32 to `isr_handler`, 32 and above to `irq_handler`.

`irq_handler` sends the EOI centrally, after the registered callback returns — individual IRQ handlers must **not** send it themselves, and an unhandled line still gets acknowledged so the PIC keeps delivering it.

`pic_init` masks every line; `irq_install_handler` unmasks the one it registers (plus the cascade for slave lines). Registering a handler is therefore the only thing that opens a line.

## Physical memory

The PMM is default-deny: `pmm_init` marks every frame **used**, `pmm_free_region` frees only what the Multiboot map reports AVAILABLE, and `pmm_reserve_region` then takes back the low 1 MiB, `[_kernel_start, _kernel_end)` and the bitmap's own frames.

**Reservations must run after the free pass.** The kernel sits inside a region the firmware reports as available, so the free pass clears its bits; reserving first would be silently undone.

Rounding is deliberately asymmetric and both directions err toward "used": freeing rounds the base **up** and the end **down** (only wholly-contained frames become free), reserving rounds the base **down** and the end **up** (any partially touched frame stays used). Reversing either hands out live memory.

`_kernel_start` / `_kernel_end` come from `linker.ld` so the protected extent tracks the real image, `.bss` and the boot stack included. Frame 0 is never allocatable, which is what makes a `NULL` return from `pmm_alloc_block` unambiguous — and `pmm_free_block(NULL)` is a no-op so that guarantee survives the usual failure-path idiom.

## Paging

Page directory and page tables are single frames from `pmm_alloc_block()` — the PMM's granularity *is* the 4 KiB alignment the hardware demands, so no `aligned` attribute is involved. A new table is **zeroed before it is installed**: `pmm_alloc_block` returns whatever bytes were there, and a stray Present bit maps a garbage frame.

The CPU **ANDs** permissions along the walk, so a PDE must be at least as permissive as any page under it — `map_page` always sets RW on the PDE and lets the PTE carry the real per-page permissions.

`paging_enable` sets **`CR0.WP` as well as `CR0.PG`**. Without WP the R/W bit is ignored for supervisor accesses (SDM 4.6.2), so read-only mappings would be silently writable — and this kernel runs entirely in ring 0.

**Reachability invariant:** everything the kernel must still reach after `CR0.PG` — the PMM bitmap, the page directory, every page table — has to be inside the identity map. 4 MiB is the floor, not the answer: `paging_init` queries `pmm_highest_used_address()` and extends the window in 4 MiB steps, iterating to a fixed point because mapping itself allocates tables. A Multiboot module of a few MiB is the realistic trigger; without this the boot hard-faults on the bitmap. `map_page`'s guard covers the post-enable case and returns false rather than faulting.

Identity-mapping from `0x0` means a null dereference does not fault. Long term, recursive mapping (PD entry 1023 → itself) or a higher-half kernel removes the reachability constraint entirely.

**The loader owns memory too.** GRUB and QEMU park the module descriptor array, the command line and the loader name immediately above `_kernel_end` — exactly where the bitmap would otherwise land. `multiboot_for_each_owned_range` in `kernel.c` walks every loader-owned range twice: once to place the bitmap above all of it, once to reserve it after the free pass. Without that, an initrd is destroyed by the bitmap fill *and* handed out as free memory.

The Makefile discovers sources recursively with `find`, so a new `src/<subsystem>/*.c` compiles with no Makefile edit. `-MMD -MP` generates `.d` files alongside each object so header edits rebuild their dependents, and every object and the final link also depend on the Makefile itself — editing a compiler flag forces a full rebuild rather than silently leaving stale objects behind.

## Strict Coding Constraints
*   **NO STANDARD LIBRARY:** This is a kernel. Do not use `<stdio.h>`, `<stdlib.h>`, or `<string.h>`. You must use or create our own kernel-level equivalents (e.g., `kprintf`, `kmemset`).
*   **Types:** Always use fixed-width integer types (`<stdint.h>` and `<stddef.h>`). 
*   **Commenting:** Only add comments when the "WHY" is non-obvious (e.g., explaining why a specific memory address, IO port, or hardware register is being accessed).

## Workflow & Commands
*   **Build Command:** `make build`
*   **Run/Emulator Command:** `make qemu` (runs QEMU emulator)
*   **Header Check:** `make check` — validates the Multiboot 1 header with `grub-file`.
*   **Headless Run:** `make screenshot` — boots with `-display none` and dumps the framebuffer to `build/screen.ppm`, for verifying output without a display.
*   **Testing:** After writing a new feature, run the build command and test in QEMU to ensure the system doesn't triple-fault or kernel panic.
