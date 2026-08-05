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
src/utils/stdio.c       kprintf
src/utils/string.c      kstrlen
```

Headers mirror the source tree under `include/` and are included by subsystem
path (`#include "drivers/vga.h"`). The Makefile discovers sources recursively, so
a new `src/<subsystem>/*.c` compiles with no Makefile edit, and `-MMD -MP`
dependency files mean header edits rebuild their dependents. Every object also
depends on the Makefile itself, so a compiler-flag change forces a full rebuild
rather than silently leaving stale objects behind.

`claude.md` records the design invariants that are easy to break — the interrupt
frame layout, the memory-reservation ordering, the paging reachability rule.
Worth reading before changing any of those.

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

## License

Unlicensed — do as you like with it.
