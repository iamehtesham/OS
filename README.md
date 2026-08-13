# A 32-bit x86 kernel, from scratch

A small monolithic kernel for i386, built from nothing: it boots via Multiboot 1,
drives the VGA text console and a PS/2 keyboard, handles CPU exceptions and
hardware interrupts, tracks physical memory with a bitmap allocator, and runs
with the MMU enabled.

Everything here is freestanding — no libc, no libgcc, no runtime.

## Requirements

```
gcc (with -m32 / multilib)   binutils (as, ld)   qemu-system-i386   make
```

There is deliberately no `i686-elf` cross-compiler in the loop: the host GCC is
used purely as a 32-bit code generator, and linking goes through `ld` directly so
the driver's Linux specs (crt0, libc, PIE) never enter the picture. Assembly is
GNU as (AT&T syntax) in `.S` files rather than NASM.

`grub-file` is optional and only used by `make check`.

## Build and run

```sh
make build      # compile and link -> build/kernel.bin
make qemu       # boot it in QEMU
make check      # validate the Multiboot 1 header with grub-file
make screenshot # headless boot, dump the framebuffer to build/screen.ppm
make clean
```

QEMU implements the Multiboot loader itself, so `make qemu` boots the ELF
directly — no GRUB, no ISO, no disk image is involved.

## What it does

| Subsystem | Notes |
| --- | --- |
| **Boot** | Multiboot 1 header, loaded at 1 MiB, 16 KiB stack |
| **VGA console** | 80×25 text, 16 colours, hardware cursor via CRTC ports, scrolling, deferred line wrap, `\n` `\r` `\t` `\b` |
| **`kprintf`** | `%c %s %d %i %u %x %X %p %%`, with its own integer conversion — all 32-bit, so no libgcc division helper is ever referenced |
| **GDT** | 3 flat entries: null, ring-0 code (`0x08`), ring-0 data (`0x10`) |
| **IDT** | 256 gates; vectors 0–31 exceptions, 32–47 IRQs; shared entry stub |
| **Exceptions** | All 32 vectors named and reported, with error code, EIP, and CR2 + decoded cause for page faults |
| **PIC** | Both 8259s remapped off vectors 8–15, per-line masking, central EOI |
| **Keyboard** | PS/2 IRQ1, scancode set 1 → ASCII, make codes only |
| **Physical memory** | Bitmap allocator over 4 KiB frames, driven by the Multiboot memory map |
| **Paging** | Two-level page tables, identity-mapped low memory, `CR0.PG` + `CR0.WP` |
| **Kernel heap** | `kmalloc`/`kfree` over a 1 MiB region at `0xC0000000`: linked list of blocks, first fit, splitting, and coalescing in both directions |

## Layout

```
src/boot.S              Multiboot header, _start, stack, entry into kernel_main
src/kernel.c            kernel_main and boot-time wiring
src/cpu/gdt.c           flat GDT
src/cpu/gdt_flush.S     lgdt, segment reload, far jump to reload CS
src/cpu/idt.c           IDT construction
src/cpu/idt_load.S      lidt
src/cpu/isr_stubs.S     exception entry points + the shared interrupt tail
src/cpu/isr.c           dispatch and exception reporting
src/cpu/irq_stubs.S     IRQ 0-15 entry points
src/cpu/irq.c           handler registration, dispatch, EOI
src/cpu/pic.c           8259 remap and masking
src/drivers/vga.c       text console
src/drivers/keyboard.c  PS/2 keyboard
src/mm/pmm.c            physical frame allocator
src/mm/paging.c         page directory, page tables, map_page
src/mm/paging_enable.S  loads CR3, sets CR0.PG and CR0.WP
src/mm/kheap.c          kmalloc / kfree over a linked list of blocks
src/utils/stdio.c       kprintf
src/utils/string.c      kstrlen
```

Headers mirror the source tree under `include/` and are included by subsystem
path (`#include "drivers/vga.h"`). The Makefile discovers sources recursively, so
a new `src/<subsystem>/*.c` compiles with no Makefile edit, and `-MMD -MP`
dependency files mean header edits rebuild their dependents. Every object also
depends on the Makefile itself, so a compiler-flag change forces a full rebuild
rather than silently leaving stale objects behind.

## Design invariants

These are the things that break quietly if you change one half of a pair. Each
one caused, or would have caused, a real bug.

- **The interrupt frame layout lives in two places.** `struct registers` in
  `include/cpu/isr.h` is a direct view onto the stack that `src/cpu/isr_stubs.S`
  builds. Change the push sequence without changing the struct (or the reverse)
  and every register is silently misreported. Exception and IRQ stubs share one
  tail — `interrupt_common_stub` — precisely so that layout exists once.
- **EOI is sent centrally**, in `irq_handler` after the callback returns. An
  individual IRQ handler must *not* send it, and a line with no handler
  installed still gets acknowledged — otherwise the PIC never delivers it again.
- **Physical memory is default-deny.** Every frame starts *used*; only regions
  the firmware reports available are freed, and the low megabyte, the kernel
  image and all loader-owned ranges are then taken back. **The reservations must
  run after the free pass** — the kernel sits inside a region reported as
  available, so reserving first is silently undone.
- **Region rounding is asymmetric, and both directions err toward "used".**
  Freeing rounds the base up and the end down (only wholly-contained frames
  become free); reserving rounds the base down and the end up (any partially
  touched frame stays used). Reversing either hands out live memory.
- **The loader owns memory too.** GRUB and QEMU park module descriptors, the
  command line and the loader name immediately above the kernel image — exactly
  where the PMM bitmap would otherwise land.
- **Everything the kernel must reach after `CR0.PG` has to be inside the
  identity map**: the bitmap, the page directory, every page table. 4 MiB is the
  floor, not the answer; the window grows to cover what the allocator has
  already used.
- **`CR0.WP` matters even with no ring 3.** Without it the R/W bit is ignored for
  supervisor accesses, so read-only mappings would be silently writable.
- **A new page table must be zeroed before it is installed.** `pmm_alloc_block`
  returns whatever bytes were there, and a stray Present bit maps a garbage
  frame.
- **The heap's block list is address-ordered**, and coalescing depends on it:
  `kfree` merges with `block->next` on the assumption that a neighbour in the
  list is a neighbour in memory. `kmalloc`'s split preserves this by inserting
  the remainder directly after the block it came from. Anything that reorders
  the list silently turns coalescing into corruption.
- **`sizeof` the heap header must stay a multiple of `KHEAP_ALIGNMENT`**, or
  every payload drifts out of alignment as the chain grows. A `_Static_assert`
  fails the build rather than letting that happen quietly.
- **Every object depends on the Makefile itself**, so changing a compiler flag
  forces a full rebuild instead of leaving stale objects with mismatched ABI.

## Known limitations

These are deliberate boundaries, not oversights:

- **No virtual memory allocator.** `map_page` maps a frame at an address you
  choose; there is nothing that picks addresses for you, and no unmapping.
- **The identity map starts at `0x0`**, so a null-pointer dereference does not
  fault.
- **New page tables must land in identity-mapped memory.** After `CR0.PG` is set,
  `map_page` can only write a page table it can address. It returns false rather
  than faulting, and the window grows to cover what the allocator has already
  used — but recursive mapping or a higher-half kernel is what removes the
  constraint properly.
- **`pmm_free_block` cannot tell an allocated frame from a reserved one**, so
  freeing a frame that was never handed out will put it into circulation.
- **Keyboard is make codes only** — no shift, caps lock, modifiers, or extended
  (`0xE0`) keys.
- **Spurious IRQ 7/15 are not detected** via the in-service register. Not
  reachable while those lines stay masked.
- **Only IRQ1 is unmasked.** There is no timer driver yet.
- **The heap is a fixed 1 MiB and never grows.** `kmalloc` returns `NULL` once
  it is full; freed pages are not returned to the physical allocator.
- **First fit is O(n) in the number of blocks**, and there is no free list, so a
  heavily fragmented heap makes allocation slow before it makes it fail.

## License

Unlicensed — do as you like with it.
