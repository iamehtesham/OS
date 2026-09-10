# A 32-bit x86 microkernel, from scratch

A small microkernel for i386, built from nothing. It boots via Multiboot 1,
drives the VGA text console and a PS/2 keyboard, handles CPU exceptions and
hardware interrupts, manages physical memory with a bitmap allocator and virtual
memory with two-level paging, has a kernel heap, preempts processes round-robin
on the PIT, loads ELF executables into address spaces of their own, and passes
synchronous messages between them.

**The filesystem is not in it.** The VFS and the initrd driver are an ordinary
ring-3 ELF binary that answers IPC requests, in an address space with no more
access to the kernel than any other program. The kernel does not know what a
file is: it starts the programs the boot loader handed it, tells one of them
which physical range holds the filesystem image, and idles.

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
| **Physical memory** | Bitmap allocator over 4 KiB frames, driven by the Multiboot memory map, with a reference count per frame so one frame can be mapped into several address spaces and is freed only by its last holder |
| **Paging** | Two-level page tables, identity-mapped low memory, `CR0.PG` + `CR0.WP`, and per-process address spaces that share the kernel's tables without sharing its user bit |
| **Kernel heap** | `kmalloc`/`kfree` over a 1 MiB region at `0xC0000000`: linked list of blocks, first fit, splitting, and coalescing in both directions |
| **Multitasking** | Preemptive round robin on a 100 Hz PIT, with an assembly context switch and forged first-run frames; the kernel thread is the idle task and runs only when nothing else can |
| **Ring 3** | User-mode segments, a TSS supplying `ss0:esp0`, and system calls through an `int 0x80` gate at DPL 3; a fault in ring 3 kills only that task |
| **IPC** | Synchronous message passing between ring-3 tasks: single-slot mailboxes in the TCB, a blocking `recv` that parks the task, and a scheduler that skips blocked tasks |
| **ELF loader** | Reads an `ET_EXEC` i386 binary straight out of a Multiboot module, maps each `PT_LOAD` segment into a fresh address space with `.bss` allocated and zero-filled, and spawns it as a ring-3 process |
| **Physical grants** | `sys_map_physical` lets a task map physical memory, gated on a kernel-set capability bit *and* a specific range recorded from the boot loader's module list. Every other process is refused |
| **VFS server** (ring 3) | The `fs_node` dispatch table and the read-only initrd driver, running as an unprivileged process. Answers `MSG_OPEN` and `MSG_READ` over IPC |
| **Client** (ring 3) | A second process with no filesystem code and no grant. It opens and reads a file entirely by asking the server |
| **Shared memory** | `sys_shm_map` creates a page and returns an id plus an address; `sys_shm_attach` maps the same frame into another process at an address of the kernel's choosing. Only the id crosses between processes |
| **Ring-3 mutex** | `lock cmpxchg` in user assembly, with the lock word living *inside* the shared frame so both processes contend on one physical word. A failed acquisition yields instead of spinning |
| **Reaping** | The idle task tears down dead processes, which drops a reference on every frame they mapped. Private pages are freed, shared pages lose one holder, and reserved memory is pinned so it can never be freed at all |

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
src/mm/shm.c            id -> physical frame registry for shared pages
src/task/task.c         task control blocks, process creation, physical grants
src/task/elf.c          ELF32 loader: validates a module, maps its PT_LOAD segments
src/task/switch.S       context switch and the first-run bootstrap
src/task/scheduler.c    round robin over runnable tasks; schedule()
src/sys/syscall.c       int 0x80 dispatcher: print, send, recv, yield,
                        map_physical, grant_info, shm_map, shm_attach
src/sys/uaccess.c       copy_from_user / copy_to_user with per-page validation
src/ipc/ipc.c           ipc_send / ipc_recv over per-task mailboxes

  --- everything below this line runs in ring 3, in its own address space ---

src/user/lib/ulib.c     the whole user runtime: syscall wrappers, strings,
                        number formatting. Linked into every program
src/user/lib/atomic.S   compare_and_swap via lock cmpxchg -- the one thing in
                        userland that cannot be written in C
src/user/lib/mutex.c    mutex_lock / mutex_unlock over that primitive
src/user/lib/contend.c  the contended write both mutex programs run
src/user/vfs_server/    the filesystem, evicted from the kernel:
  vfs.c                   fs_node dispatch through a function-pointer table
  initrd.c                read-only driver for the packed image
  main.c                  maps its grant, mounts, then serves IPC forever
src/user/client/        a program with no filesystem code and no grant
src/user/shm_writer/    creates a shared page, writes to it, sends only the id
src/user/shm_reader/    attaches by id, reads, and writes back through the page
src/user/mutex_a/       creates the contended page and races B for it
src/user/mutex_b/       attaches to it and races A

tools/make_initrd.py    host-side packer; writes the format initrd.h declares
initrd/                 files packed into the image, one per entry
linker.ld               link map for the kernel image, loaded at 1 MiB
user.ld                 link map for ring-3 binaries, at 0x40000000
src/utils/stdio.c       kprintf
src/utils/string.c      kstrlen, kstrncpy, kstrcmp, kmemcpy, kmemset
```

Three headers are the seams of the system. `include/sys/syscall_abi.h` is the
kernel/userland contract, `include/ipc/vfs_proto.h` is the client/server
contract, and `include/fs/vfs.h` is now a userland interface that the kernel
never sees. `include/format/elf.h` carries the ELF32 structs. The ABI headers
must stay free of kernel declarations, because ring-3 binaries include them.

Headers mirror the source tree under `include/` and are included by subsystem
path (`#include "drivers/vga.h"`). The Makefile discovers sources recursively, so
a new `src/<subsystem>/*.c` compiles with no Makefile edit, and `-MMD -MP`
dependency files mean header edits rebuild their dependents. Every object also
depends on the Makefile itself, so a compiler-flag change forces a full rebuild
rather than silently leaving stale objects behind.

## How it was built

Each phase below was a working system before the next one started, and each one
is summarised by the thing that was actually hard about it rather than by the
feature list. Several of these entries describe a bug that shipped and was
caught afterwards; those are the useful ones.

**1. Boot and a console.** A Multiboot 1 header, a stack, and `_start` handing
off to C at 1 MiB. The toolchain decision came first and shaped everything
after it: the host GCC is used purely as a 32-bit code generator and `ld` is
invoked directly, because going through the `gcc` driver drags in its Linux
specs — crt0, libc, PIE — and produces an image that will not boot. QEMU
implements the Multiboot loader itself, so there is no GRUB, no ISO and no disk
image anywhere in the loop.

**2. `kprintf`.** A VGA text console with a hardware cursor, scrolling and
deferred line wrap, and a formatter with its own integer conversion. That last
part is not incidental: a single 64-bit division would emit a call to
`__udivdi3`, which a freestanding link cannot resolve, so everything stays
32-bit and `nm -u` staying empty is part of the build's definition of success.

**3. Descriptor tables and exceptions.** A flat GDT, a 256-entry IDT, and all 32
CPU exceptions named and reported with their error codes. The invariant that
emerged here outlived the phase: `struct registers` in C is a direct view onto
the stack frame the assembly stubs build, so the push sequence and the struct
must be edited together or every register is silently misreported. Exception and
IRQ stubs share one tail so that layout exists in exactly one place.

**4. Interrupts from hardware.** Both 8259s remapped off vectors 8–15, a
keyboard on IRQ1, and later the PIT. The end-of-interrupt is sent centrally and
*before* the handler runs. That ordering looks wrong until a handler switches
stacks and never returns — which the scheduler does — at which point
acknowledging afterwards ties each tick's EOI to whichever task happens to
resume next.

**5. Physical memory.** A bitmap allocator over 4 KiB frames, driven by the
firmware's memory map and default-deny: every frame starts used and only what
the firmware calls available is handed back. Three traps, all of which bit:
reservations must run *after* the free pass, because the kernel sits inside a
region reported as available; the rounding is deliberately asymmetric in both
directions so a partially covered frame always stays used; and the boot loader
owns memory too, parking its module list exactly where the bitmap would
otherwise land.

**6. Virtual memory.** Two-level page tables, an identity map, `CR0.PG` and
`CR0.WP`. The governing constraint is reachability: everything the kernel must
still touch once translation is on — the bitmap, the directory, every page
table — has to be inside the identity map, and the window has to grow to cover
whatever the allocator has already used. Sizing that headroom with a constant
was wrong twice before it was derived from what the kernel actually intends to
allocate.

**7. A kernel heap.** `kmalloc`/`kfree` over an address-ordered block list with
splitting and coalescing. Coalescing depends on that ordering, so anything that
reorders the list quietly turns a merge into corruption.

**8. A filesystem.** A VFS dispatch table over function pointers and a read-only
initrd driver for an image delivered as a Multiboot module. This is the code
that later left the kernel entirely.

**9. Preemptive multitasking.** A 100 Hz PIT, an assembly context switch, and a
run list. The trick that makes it small: a brand-new task's stack is forged into
exactly the shape the switch expects to find, so a task that has never executed
is resumed by the identical code path as one that has.

**10. Ring 3.** User-mode segments, a TSS supplying `ss0:esp0`, and system calls
through the one IDT gate with DPL 3. The TSS is what makes the return trip
survivable: an interrupt from ring 3 cannot push onto the user stack, so the CPU
reads a kernel stack out of the TSS first, and getting that wrong is a triple
fault rather than an error message.

**11. Message passing.** Single-slot mailboxes living in the task control block,
with a `recv` that blocks and a scheduler that skips blocked tasks. A message
never moves user-to-user in one motion: the sender copies into the receiver's
kernel-owned mailbox and the receiver copies out when it wakes, so each half
validates exactly one task's memory.

**12. ELF loading and separate address spaces.** Programs became real binaries,
parsed from their program headers into page directories of their own. Two things
mattered. `p_memsz` minus `p_filesz` is `.bss` — memory that must exist and read
as zero while occupying nothing in the file — so frames are counted from the
former and a loader that used the latter faults on the first zero-initialised
global. And kernel page tables are *shared* into every address space, so the
check that keeps a process out of kernel memory is table ownership, not an
address range; the range version shipped first and a review demonstrated a
process writing into the kernel's own page table through it.

**13. The microkernel split.** The filesystem was evicted from the kernel and
became an ordinary ring-3 binary answering IPC. The kernel then had no way to
read the programs it must start, so everything arrives as a Multiboot module
instead. The server needs the initrd's physical memory, which produced
`sys_map_physical` — gated on a capability bit the kernel alone can set *and* on
a specific physical range recorded from the loader's module list, because a bit
on its own would make the server exactly as dangerous as the kernel.

**14. Reference counting, shared memory and reclamation.** Frames gained
reference counts so one frame can live in several address spaces. The insight
that makes it cheap is that the record of *who* holds a frame already exists —
every present user page is one reference — so tearing an address space down is a
walk that decrements, identical for a private page and a shared one. Refcounting
alone would only have made the leak refcounted, though: nothing ran that
teardown, so this phase also had to add the reaper, which runs in the idle task
because a process cannot free the page directory it is executing on. A review
then found the shared-memory ids were checked for existence but not entitlement,
which let any process read every shared page by counting upwards.

**15. A userspace mutex.** `lock cmpxchg` in ring-3 assembly, with the lock word
inside the shared frame so both processes contend on one physical word. On a
single core the `lock` prefix is nearly redundant — interrupts are taken at
instruction boundaries, so one instruction cannot be split — and what actually
provides safety is that the read-modify-write *is* one instruction where the C
equivalent is four. The prefix earns its keep on a second core.

Every phase was reviewed adversarially afterwards by agents that build variants
and boot them, and the reviews found real defects in most of them: a page
directory that was never zeroed, a buffer overflow in a directory listing, an
unbounded retry that let one client hang a system service, and the capability
hole above among them. Where a review refuted a claim, that is recorded too.

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
- **A reference count says how many, page tables say who.** The record of which
  process holds a frame already exists: every present user page in an address
  space is one reference. So tearing an address space down is a walk that
  decrements, and it is the same walk for a private stack page as for a shared
  one — the count decides which gets freed. That uniformity is the reason
  refcounting is worth having, not a side effect of it.
- **Refcounting alone does not stop a leak; something has to run the teardown.**
  Before this, a process killed at run time was marked dead and parked, and its
  frames were never reclaimed. Reference counts on their own would only have
  made that leak refcounted.
- **A process cannot free the address space it is executing on.** The page
  directory being torn down is the one translating the instruction doing the
  tearing, so reaping is deferred to the idle task, which runs on the kernel's
  own address space and always exists.
- **Reserved memory is pinned, not counted.** Firmware, the kernel image and
  the boot modules were never allocated, so a mapping of one must never be able
  to count down to zero. A process that maps the initrd and then dies decrements
  every page it held; the sentinel is what stops that walk from putting boot
  modules into the free pool. Verified by killing the VFS server mid-flight and
  checking the initrd's frame afterwards.
- **The count saturates rather than wrapping.** Rolling over from 255 to 0 frees
  a frame that every one of those address spaces is still mapping, which hands
  ring 3 a use-after-free. The share is refused instead.
- **A shared id must outlive its last holder's death.** The registry keeps a
  reference of its own, so an id can never name a frame the allocator has since
  handed to somebody else, and the entry retires only when the count falls back
  to that one reference. Ids are never reused, for the same reason pids are not.
- **A lock must live in the memory it protects.** The mutex word sits at offset
  0 of the shared frame, so both processes contend on one physical word. A lock
  in either process's private memory would be two locks, each of which always
  looks free to its owner, and the mutual exclusion would be imaginary.
- **What makes a compare-and-swap safe on one core is that it is one
  instruction**, not the `lock` prefix. Interrupts are taken at instruction
  boundaries, so a single instruction cannot be split by the scheduler; the
  equivalent C `if (x == 0) x = 1;` is four instructions with three places for
  the timer to land. The prefix is what makes it correct on a second core, and
  costs one byte, so it is written now rather than left as a trap for whoever
  adds SMP.
- **The compiler is the other adversary.** A shared word is written by a process
  this compiler cannot see, so a plain read may be hoisted out of the retry loop
  or cached in a register forever. Hence `volatile` on the lock word, a memory
  clobber in the asm, and a compiler barrier before the release store — x86 will
  not reorder the store itself, but the optimizer will happily sink a write from
  inside the critical section past it.
- **A failed acquisition must yield, not spin.** On one core the lock holder is
  by definition not running, so spinning re-reads a word that cannot change
  until the spinner stops. Measured: ~20 failed acquisitions per task when
  yielding, ~11,000,000 when spinning, for identical work and identical results.
- **A capability is a noun, not a verb.** `sys_map_physical` is gated on two
  things that answer different questions. A bit in the control block says *who*
  may call it, and ring 3 cannot reach that bit because the control block is on
  a supervisor page and no system call sets it. A recorded physical range says
  *what* they may map. The bit alone would make the VFS server exactly as
  dangerous as the kernel, since one parser bug in ring 3 would then reach
  kernel text. The range comes from the boot loader's module list and never
  from anything the caller says.
- **The kernel picks the virtual address a grant lands at.** A caller that
  chose its own could map over its own stack or its own code, which turns a
  mapping call into a way to corrupt itself.
- **Module order is a contract between the Makefile and `kernel.c`.** The
  kernel has no filesystem, so it identifies the server, the client and the
  filesystem image purely by position in the Multiboot module list. Reordering
  the list in the Makefile starts the wrong program and grants it the wrong
  memory, and nothing would report an error.
- **The server's pid is fixed by creation order.** A client has to name the
  server before it has spoken to anything, so `VFS_SERVER_PID` is compiled in.
  The kernel creates the server first to make that true, and checks the result
  rather than assuming it.
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
- **A server must not stake its liveness on a client.** Replies are sent with a
  bounded retry and then dropped. An unbounded retry means any process that
  stops collecting its replies parks the server inside the send forever, which
  is one unprivileged program ending a system service for everyone. Measured:
  unbounded, a client that never receives leaves the server trapped and no
  other client is ever served again; bounded, the same client only slows
  others down.
- **A reply is input, not truth.** The client checks that a message came from
  the server's pid, which the kernel stamps, and checks every length in it
  against the buffer it will be copied into. The read path takes a byte count
  from the payload, so without that check a peer could name a length of 255
  into a 32-byte stack buffer.
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
- **`pmm_free_block` cannot tell one holder's reference from another's.** It
  does know a reserved frame from an allocated one — reserved memory is pinned
  and a free of it is ignored — but it takes any caller's word that the
  reference being dropped is theirs, so a double free releases someone else's
  hold on a frame that is still mapped.
- **Keyboard is make codes only** — no shift, caps lock, modifiers, or extended
  (`0xE0`) keys.
- **Spurious IRQ 7/15 are not detected** via the in-service register. Not
  reachable while those lines stay masked.
- **Only IRQ0 and IRQ1 are unmasked.** Every other line stays masked, and a
  handler is what opens one.
- **The heap is a fixed 1 MiB and never grows.** `kmalloc` returns `NULL` once
  it is full; freed pages are not returned to the physical allocator.
- **First fit is O(n) in the number of blocks**, and there is no free list, so a
  heavily fragmented heap makes allocation slow before it makes it fail.
- **The initrd is flat and read-only.** No subdirectories, no path parsing, no
  writing, no creation or deletion. `readdir` returns a pointer to a single
  shared `dirent`, which is safe only while the kernel is single-threaded.
- **Only one filesystem can be mounted**, at `/`. There is no mount table.
- **The mutex has no timeout, no owner and no recursion.** Locking twice from
  the same process deadlocks it against itself, a process that dies holding the
  lock leaves it held forever with no way to break it, and there is no priority
  inheritance because there are no priorities.
- **A shared segment has exactly two possible holders.** The creator names one
  peer at creation, and no third process may attach however it comes by the id —
  which is the fix for a real hole, since ids are sequential and were previously
  checked only for existence. Sharing among three or more would need a grantee
  set rather than a single peer.
- **A shared segment is exactly one page.** There is no multi-page segment, no
  resize, and no explicit detach — a mapping lasts until the process dies.
- **The shared-memory window is a bump allocator.** Addresses are handed out
  forward and never reclaimed, which is what makes an attach unable to land on
  something already mapped, at the cost of a process that attaches repeatedly
  eventually exhausting its window.
- **The registry is a fixed 16 entries** and any process may fill it. There is
  no quota, so a hostile program can deny shared memory to everyone else.
- **Reaping only runs when the machine goes idle.** A dead process's memory is
  held until the scheduler has nothing else to do, which is immediate here and
  would not be under sustained load.
- **The filesystem has no timeouts and no recovery.** If the VFS server dies,
  every client blocks in `recv` forever: there is no supervisor, no restart, and
  no way for a client to notice. A real microkernel earns its keep by
  restarting a failed server, and this one cannot.
- **One outstanding request per client.** A client sends and then blocks for
  the next message, treating whatever arrives as the answer. With a single
  mailbox slot and no request identifiers that is sound only because each
  client has exactly one request in flight and only the server writes to it.
- **A greedy client can starve a polite one.** With one mailbox slot, no queue
  and no fairness, a process that sends as fast as it is scheduled keeps the
  server's slot occupied and others retry until they give up. Bounding the
  server's reply retry stops one client from ending the service outright, but
  nothing here promises progress to everybody; a queue with per-sender fairness
  is what would.
- **The server is single-slot and serial.** It handles one message to
  completion before receiving the next, so a slow request blocks every other
  client. There is no queue, and a second sender gets `IPC_ERR_FULL` and must
  retry.
- **A filename must fit one message.** Requests carry a fixed 32-byte payload,
  so a name longer than 31 bytes cannot be expressed. The server refuses such a
  request rather than truncating it into a lookup for a different file.
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
  another. It is reclaimed when it dies, but only by the idle task, and its
  pid is never reused.
- **A page directory created after a process exists would not reach it.**
  Address spaces share the kernel's page tables, so mappings added inside an
  existing table propagate everywhere, but a brand-new kernel directory entry
  would appear only in the kernel's own directory. Nothing creates one after
  boot today.
- **IPC is single-slot and untimed.** A mailbox holds one unread message; a
  second `send` returns `IPC_ERR_FULL` rather than queueing, and `recv` blocks
  with no timeout. Pids are assigned in creation order, never reused, and the
  demo hardcodes the receiver's.
- **Reaping is all-or-nothing, and only at idle.** A dead process is torn down
  completely — address space, kernel stack, control block, and one reference on
  every frame it mapped — but only once the scheduler has nothing else to run.
  Its pid is never reused.

## License

Unlicensed — do as you like with it.
