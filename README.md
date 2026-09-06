# A 32-bit x86 kernel, from scratch

A small monolithic kernel for i386, built from nothing. It boots via Multiboot 1,
drives the VGA text console and a PS/2 keyboard, handles CPU exceptions and
hardware interrupts, manages physical memory with a bitmap allocator and virtual
memory with two-level paging, has a kernel heap and a read-only initrd behind a
VFS, preempts tasks round-robin on the PIT, runs user programs in ring 3, and
lets them talk to each other through a synchronous message-passing system call.
It also loads ELF executables out of the initrd and runs each one as a process
in a page directory of its own.

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
| **GDT** | 6 flat entries: null, ring-0 code/data (`0x08`/`0x10`), ring-3 code/data (`0x18`/`0x20`, DPL 3), and a TSS (`0x28`) |
| **IDT** | 256 gates; vectors 0–31 exceptions, 32–47 IRQs, `0x80` system calls (the only DPL-3 gate); one shared entry stub |
| **Exceptions** | All 32 vectors named and reported, with error code, EIP, and CR2 + decoded cause for page faults |
| **PIC** | Both 8259s remapped off vectors 8–15, per-line masking, central EOI sent before dispatch |
| **Keyboard** | PS/2 IRQ1, scancode set 1 → ASCII, make codes only |
| **Physical memory** | Bitmap allocator over 4 KiB frames, driven by the Multiboot memory map |
| **Paging** | Two-level page tables, identity-mapped low memory, `CR0.PG` + `CR0.WP`, and per-process address spaces that share the kernel's tables without sharing its user bit |
| **Kernel heap** | `kmalloc`/`kfree` over a 1 MiB region at `0xC0000000`: linked list of blocks, first fit, splitting, and coalescing in both directions |
| **VFS** | `fs_node` with a function-pointer table (`read`/`write`/`open`/`close`/`readdir`/`finddir`) and wrappers that dispatch only when a driver implements the slot |
| **Initrd** | Read-only driver for a flat image packed by `tools/make_initrd.py` and delivered as a Multiboot module |
| **Multitasking** | Preemptive round robin on a 100 Hz PIT, with an assembly context switch and forged first-run frames; the kernel thread is the idle task and runs only when nothing else can |
| **Ring 3** | User-mode segments, a TSS supplying `ss0:esp0`, and system calls through an `int 0x80` gate at DPL 3; a fault in ring 3 kills only that task |
| **IPC** | Synchronous message passing between ring-3 tasks: single-slot mailboxes in the TCB, a blocking `recv` that parks the task, and a scheduler that skips blocked tasks |
| **ELF loader** | Reads an `ET_EXEC` i386 binary from the initrd through the VFS, maps each `PT_LOAD` segment into a fresh address space with `.bss` allocated and zero-filled, and spawns it as a ring-3 process |

## Layout

```
src/boot.S              Multiboot header, _start, stack, entry into kernel_main
src/kernel.c            kernel_main and boot-time wiring
src/cpu/gdt.c           flat GDT: kernel and user segments, TSS descriptor
src/cpu/gdt_flush.S     lgdt, segment reload, far jump to reload CS
src/cpu/tss.c           TSS holding the ring-0 stack for privilege changes
src/cpu/idt.c           IDT construction, including the DPL-3 int 0x80 gate
src/cpu/idt_load.S      lidt
src/cpu/isr_stubs.S     exception entry points + the shared interrupt tail
src/cpu/isr.c           dispatch and exception reporting
src/cpu/irq_stubs.S     IRQ 0-15 entry points
src/cpu/irq.c           handler registration, dispatch, EOI
src/cpu/pic.c           8259 remap and masking
src/drivers/vga.c       text console
src/drivers/keyboard.c  PS/2 keyboard
src/drivers/pit.c       programmable interval timer, 100 Hz tick hook
src/mm/pmm.c            physical frame allocator
src/mm/paging.c         page directory, page tables, map_page, address spaces
src/mm/paging_enable.S  loads CR3, sets CR0.PG and CR0.WP
src/mm/kheap.c          kmalloc / kfree over a linked list of blocks
src/fs/vfs.c            fs_node dispatch through a function-pointer table
src/fs/initrd.c         read-only driver for the packed initrd image
src/task/task.c         task control blocks; ring-0, ring-3 and process creation
src/task/elf.c          ELF32 loader: validates a file, maps its PT_LOAD segments
src/task/switch.S       context switch and the two first-run bootstraps
src/task/scheduler.c    round robin over runnable tasks; schedule()
src/task/user.S         switch_to_user_mode (unused now that tasks start in ring 3)
src/sys/syscall.c       int 0x80 dispatcher: sys_print, sys_send, sys_recv, sys_yield
src/sys/uaccess.c       copy_from_user / copy_to_user with per-page validation
src/ipc/ipc.c           ipc_send / ipc_recv over per-task mailboxes
src/user/ipc_demo.c     two ring-3 programs, linked into their own .utext/.urodata
src/user/programs/      standalone ring-3 binaries, linked by user.ld, NOT part
                        of the kernel image; packed into the initrd and loaded
                        at run time by the ELF loader
tools/make_initrd.py    host-side packer; writes the format initrd.h declares
initrd/                 files packed into the image, one per entry
linker.ld               link map for the kernel image, loaded at 1 MiB
user.ld                 link map for standalone ring-3 binaries, at 0x40000000
src/utils/stdio.c       kprintf
src/utils/string.c      kstrlen, kstrncpy, kstrcmp, kmemcpy, kmemset
```

`include/format/elf.h` carries the ELF32 file and program header structs, and
`include/sys/syscall_abi.h` the system call numbers. That second one exists
because standalone ring-3 binaries include it too, so it must stay free of
kernel declarations.

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
- **EOI is sent centrally, and *before* the callback.** A callback can switch
  stacks and never return, and a task can park itself from a system call whose
  frame contains no EOI at all — so acknowledging *after* the callback would tie
  each tick's EOI to whichever task happens to resume next. Acknowledging first
  means every tick acknowledges itself. Individual handlers must *not* send it,
  and an unhandled line is still acknowledged so the PIC keeps delivering it.
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
- **A message never moves user-to-user in one motion.** `send` copies the
  sender's buffer into the *receiver's* kernel-owned mailbox; `recv` copies that
  mailbox into the receiver's own buffer when it wakes. Each half validates
  exactly one task's memory, and the kernel overwrites `sender_pid` rather than
  trusting it.
- **`p_memsz` sizes the memory, `p_filesz` sizes the copy.** The difference is
  `.bss`: bytes that must exist and read as zero but occupy nothing in the
  file. Allocating frames from `p_filesz` gives a program that faults on its
  first zero-initialised global. The loader zeroes each frame in full and then
  copies only `p_filesz` bytes over it, which makes the `[p_filesz, p_memsz)`
  gap, the slack past the end of the last page, and the leftover bytes of a
  recycled frame all handled by the same act.
- **An address space may only write page tables it owns.** Kernel tables are
  shared into every address space rather than copied, so a mapping written into
  one of them does not shadow the kernel's entry, it overwrites it, everywhere
  at once. The check that enforces this lives in `map_into` and compares the
  table's frame against the kernel directory's, because ownership is the real
  question. An address-range check is the wrong shape and was the original bug
  here: it silently assumed the kernel maps nothing inside the process window,
  and the paging self-test did exactly that. A segment aimed there was written
  into the kernel's own table, which handed the process a kernel frame, handed
  two processes the same frames as each other, and leaked a frame when the
  load was later refused.
- **A kernel mapping inside the process window costs that whole 4 MiB span.**
  Its table is shared into every address space, so nothing can be mapped there
  for a process afterwards. The self-test address is asserted at compile time
  to sit outside the window.
- **Validating a page means testing both levels, always.** A page table entry
  can say user-accessible while the directory entry reaching it does not, which
  is exactly the state a shared kernel table is left in. Reading the page table
  entry alone accepted entry points the MMU then refused, so the process was
  built, given a pid and stacks, and died on its first instruction fetch.
- **Kernel directory entries are shared with the user bit cleared.** A CPL 0
  access ignores that bit, so the kernel loses nothing, while a process loses
  the ability to reach any ring-3 page that lives in identity-mapped low
  memory. Without it, a loaded process could read the code and stacks of the
  ring-3 tasks linked into the kernel image, since they sit in tables every
  address space shares.
- **User pointers are validated against the ACTIVE page directory.** A system
  call runs with the caller's `CR3` still loaded, so checking the kernel's
  directory would accept addresses the caller cannot reach and reject ones it
  can. Reading `CR3` works as a directory pointer only because every directory
  is inside the identity map.
- **Anything the kernel must write before mapping it has to be identity
  mapped.** Page tables, a frame being zero-filled, a segment being copied in:
  all are reached by physical address, because the address space being built
  is not the one that is active.
- **The identity window's headroom must cover everything that follows it, not
  just its own page tables.** The heap, every process page directory and table,
  every segment frame and every ring-3 stack frame come out of that reserve. At
  64 KiB it only looked sufficient because a small initrd left megabytes of
  slack below the 4 MiB rounding boundary; a 2 MiB initrd consumed the slack
  and the heap alone exhausted the window, so no user task could start on a
  machine with 128 MiB free.
- **The idle task is a fallback, not a peer.** The scheduler skips it while
  any other task is runnable and hands it the CPU only when the caller has
  blocked or died. Scheduled as an equal, it took every other tick while the
  receiver sat blocked, so the CPU halted for half of all wall time, a `yield`
  handed the CPU to `hlt` instead of to the task just woken, and IPC under
  load was capped at one message per tick. Fixing that doubled the demo's
  message rate without touching IPC.
- **Every send target must be able to collect the message.** A missing pid, a
  dead task, a user task that has faulted, and the idle task (which never calls
  `recv`) are all refused with `IPC_ERR_NO_TASK`. Accepting the send instead
  reports success for a message that is parked forever, and for the idle task
  it parks 44 user-chosen bytes inside the kernel's own control block. This is
  why a ring-3 fault marks the task `TASK_DEAD` rather than merely halting it.
- **Pids are never reused.** Dead tasks stay linked in the ring and `task_find`
  has no state filter, so a recycled pid could resolve to the corpse or the new
  task depending on who is asking; and `sender_pid` is a bare integer, so a
  reply to a pid that died and was reissued would reach the new holder
  undetectably. Reuse needs unlinking and a generation counter first.
- **Ring-3 string constants must be laundered through an `asm` barrier.** GCC
  will fold a small `const` array into immediates and emit its own copy of the
  bytes in the ordinary `.rodata` — the section attribute only governs the
  object it no longer references — and ring 3 then faults on a supervisor page.
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
- **The initrd is flat and read-only.** No subdirectories, no path parsing, no
  writing, no creation or deletion. `readdir` returns a pointer to a single
  shared `dirent`, which is safe only while the kernel is single-threaded.
- **Only one filesystem can be mounted**, at `/`. There is no mount table.
- **Two kinds of ring-3 program coexist, and only one is isolated.** A task
  from `create_user_task` is compiled into the kernel image, runs from
  `.utext`/`.urodata` and shares the kernel's address space, so it can reach
  the other such tasks' pages. A process from the ELF loader gets its own page
  directory and cannot. The first kind exists because it predates the loader.
- **The loader is eager and non-relocating.** The whole image is read into the
  heap and copied into frames up front: no demand paging, no `mmap`, and no
  page cache. Only `ET_EXEC` is accepted, so there is no relocation
  processing, no dynamic linking, and no interpreter.
- **A process's frames must be identity-mapped.** The kernel fills them by
  physical address, so an image large enough to push the allocator past the
  identity window fails to load rather than falling back to a temporary
  mapping.
- **No `exec`, no `fork`, no `exit`, and no arguments.** A process is created
  by the kernel at boot, gets no `argv` or environment, and cannot start
  another. Its address space is never reclaimed: a process that faults is
  marked dead, but its frames, page tables and page directory stay allocated
  because there is no reaping.
- **A page directory created after a process exists would not reach it.**
  Address spaces share the kernel's page tables, so mappings added inside an
  existing table propagate everywhere, but a brand-new kernel directory entry
  would appear only in the kernel's own directory. Nothing creates one after
  boot today.
- **IPC is single-slot and untimed.** A mailbox holds one unread message; a
  second `send` returns `IPC_ERR_FULL` rather than queueing, and `recv` blocks
  with no timeout. Pids are assigned in creation order, never reused, and the
  demo hardcodes the receiver's.
- **Dead tasks are never reaped.** A task that returns or faults is marked
  `TASK_DEAD`, skipped by the scheduler and refused by `send`, but its control
  block and stacks stay allocated and it stays in the ring.

## License

Unlicensed — do as you like with it.
