# A 32-bit x86 microkernel, from scratch

A small microkernel for i386, built from nothing. It boots via Multiboot 1,
drives the VGA text console and a PS/2 keyboard, handles CPU exceptions and
hardware interrupts, manages physical memory with a bitmap allocator and virtual
memory with two-level paging, has a kernel heap, preempts processes round-robin
on the PIT, loads ELF executables into address spaces of their own, and passes
synchronous messages between them.

**The filesystem is not in it, and neither is the keyboard driver.** The VFS
and the initrd driver are an ordinary ring-3 ELF binary that answers IPC
requests, in an address space with no more access to the kernel than any other
program. The keyboard driver is another: the kernel takes IRQ1, masks it, and
forwards it as a message; the driver reads the scancode from port `0x60` itself,
under an IOPL the kernel raised for it, and re-opens the line when it is done.
The kernel does not know what a file or a keystroke is: it starts the programs
the boot loader handed it, tells one of them which physical range holds the
filesystem image, tells another it may touch I/O ports and owns a line, and
idles. A third server, with no privilege at all, decides which application each
keystroke reaches. And the console is not in the kernel either: the kernel does
not print, it logs, and a fourth server that maps the VGA text buffer shows that
log and every process's output on virtual terminals it keeps in its own memory.
The kernel writes to the screen for exactly one reason, a panic. And the last
of its servers drives an RTL8139 network card from ring 3 — finding it on the
PCI bus, allocating a DMA buffer, reading packets the card writes straight into
memory, naming the Ethernet addresses and protocol inside each one,
answering ARP for its own address, answering a ping — an ICMP echo request,
checksums verified coming in and computed going out — and echoing UDP sent to
port 7, whose checksum covers a pseudo-header of IP fields as well as the
datagram — on an address it did not have when it booted, but leased by DHCP,
with a kernel alarm to retransmit by — and with the kernel never learning what a
packet is. The kernel
starts six processes at boot and no more; a shell among them starts everything
else from files in the filesystem image with `sys_spawn`, and the kernel
validates every image before a byte of it runs.

Everything here is freestanding — no libc, no libgcc, no runtime.

## Requirements

```
gcc (with -m32 / multilib)   binutils (as, ld)   qemu-system-i386   make
```

The network test targets also need `python3` (the tools under `tools/`) and
`tcpdump` (`make ping`, `make udp`, `make udp-host` and `make dhcp` re-check
every checksum the guest sent with it; reading a capture needs no privilege). `make udp-host`
and the `nc` step below need OpenBSD `netcat`, for its `-W` option.

There is deliberately no `i686-elf` cross-compiler in the loop: the host GCC is
used purely as a 32-bit code generator, and linking goes through `ld` directly so
the driver's Linux specs (crt0, libc, PIE) never enter the picture. Assembly is
GNU as (AT&T syntax) in `.S` files rather than NASM.

`grub-file` is optional and only used by `make check`.

## Build and run

```sh
make build      # compile and link -> build/kernel.bin
make qemu       # boot it in QEMU
make netdemo    # boot it and feed the network card real Ethernet frames
make ping       # boot it headless, ping it, and check every reply byte
make udp        # boot it headless, send UDP to port 7, and check every echo
make udp-host   # the host's own nc through QEMU's user network and a forward
make dhcp       # play the DHCP server, and try to break the guest's client
make warnings   # the strict warning gate at -O0 -O1 -O2 -O3 -Os, as errors
make check      # validate the Multiboot 1 header with grub-file
make screenshot # headless boot, dump the framebuffer to build/screen.ppm
make clean
```

QEMU implements the Multiboot loader itself, so `make qemu` boots the ELF
directly — no GRUB, no ISO, no disk image is involved.

`make qemu` attaches the network card to QEMU's user-mode network. The machine
boots with no address: the driver broadcasts a DHCP DISCOVER, QEMU's own DHCP
server offers 10.0.2.15, and after the REQUEST and ACK the console shows
`DHCP Lease Acquired: 10.0.2.15` — press Esc to see it — and then an ARP request
for the router the lease named, and QEMU's stack answering it. It also forwards
UDP from the host to the guest's echo service, so while it runs, in another
terminal:

```bash
nc -u 127.0.0.1 7007
```

Type a line and it comes back; press Esc in the QEMU window to watch it arrive
(`UDP Packet | Port 53691 -> 7 | Length: 14`, then the text). Ctrl-C ends `nc`;
Ctrl-D does not. Not `nc -u 10.0.2.15 7`: user-mode networking is NAT, the host
routes 10.0.2.15 out through its own LAN router, and host port 7 needs
privilege — so QEMU forwards `127.0.0.1:7007` to `10.0.2.15:7`, bound to
loopback so nothing else on the LAN can reach it. (The forward names 10.0.2.15
while the guest leases its address; they agree because QEMU's DHCP server gives
10.0.2.15 to the first client it sees.) Use `127.0.0.1` rather than
`localhost`, which may resolve to `::1`; leave out `-v`, which makes OpenBSD
`nc` send probe datagrams first; and keep lines under 1472 bytes, beyond which
the forward fragments them and the guest drops fragments. If port 7007 is taken
QEMU will not start: `make qemu UDP_ECHO_HOST_PORT=<other port>`.

That gateway never sends an ARP request or a ping, though, and nothing
malformed, and those reply paths need a peer that does. `make netdemo` boots
the same machine with the card on a socket netdev instead, plays the DHCP server
so the guest has an address, then writes ARP, an ICMP echo request, a UDP echo,
IPv4, IPv6 and unrecognised frames into it (`tools/inject_frames.py`), and reads back and decodes whatever the guest
transmits — so each reply is checked as bytes on the wire rather than as a line
on the guest's own console.

`make ping` is how the machine is pinged, and it is not the host's own `ping`,
which cannot reach it: user-mode networking is NAT, its `hostfwd` forwards TCP
and UDP but never ICMP, and the TAP device that would let the host route to
10.0.2.15 needs root. So `tools/ping.py` plays the host. It builds real ICMP
echo requests, delivers them to the card over a socket netdev, and checks every
reply field by field against checksums from its own RFC 1071 implementation.
Then it sends requests the guest must refuse — bad checksums, other addresses,
broadcast and multicast sources, fragments, malformed and oversized headers,
non-echo ICMP — and after each one proves the guest is still answering. QEMU records every frame the guest transmits into `build/ping.pcap`, and
`tcpdump`, a decoder this project did not write, re-checks every checksum in
it. The target is headless
and its exit status is the verdict.

`make udp` does the same for the echo service (`tools/udp_echo.py`): datagrams
of 0 to 1472 bytes and every byte value, requests with and without a checksum,
the one whose checksum comes out as zero, and then everything the guest must
refuse — bad checksums, checksums over a wrong pseudo-header, lengths shorter
than the header or longer than the datagram, other ports, service source ports
— each built so that only the check it is named for can stop it. It also reads
the guest's console out of VGA memory through the QEMU monitor. Its `tcpdump`
gate is stricter than ping's, because a UDP checksum of 0 is legal and means
"none": every UDP frame the guest sent must read `udp sum ok`. `make udp-host`
is the `nc` step above, headless: the host's own netcat through QEMU's
user-mode network and a port forward of its own (7008), the guest's console
checked first for the lease QEMU's DHCP server granted and then for the datagram
arriving, and a size sweep. Every socket-netdev harness now starts QEMU paused,
connects, and only then lets the guest run — the listening socket drops whatever
the guest says before a peer is there, and its first word is a DISCOVER — and
then plays the DHCP server before testing anything, because an unleased guest
answers nothing. The lease is the first named case of `make ping` and `make
udp`: the guest must take QEMU's checksummed OFFER and ACK. One that refuses
them — as a broken checksum verifier would — fails that case and is leased again
with no checksum (UDP's 0), so the rest of the run still reaches the cases
written for what is broken. One that cannot be leased at all — a port read in
the wrong byte order, 68 as 17408 — fails that case, and the run ends there.

`make dhcp` is the client's own test (`tools/dhcp_test.py`): the harness is the
only DHCP server on the guest's segment, and it runs three boots. The first
walks a lease's whole life. It checks the DISCOVER field by field and times its
backoff; proves the unleased machine answers nothing, not even at 0.0.0.0; sends
OFFERs, ACKs and NAKs each broken in exactly one way — xid, hardware address,
cookie, options that overrun, never end or run to 255 bytes, bad masks and
addresses, a message the state is not waiting for, a lease already over — and
requires every one refused, starting a client that wrongly takes one afresh
(NAKed, or left to lose that ACK's two-second lease) so the next case meets a
client in the state it is named for; NAKs the REQUEST the way QEMU does and
times the restart; stops answering and times the client giving up; and grants
leases of a few seconds — with and without a mask and a router, with options
padded and split across repeats, with routers that must be ignored — and times
their ends: one counted from its REQUEST while its ACK is held back, one while a
flood of frames keeps the receive ring full. The second leases 10.0.2.42/16 with
a list of 63 routers, 10.0.2.3 first, and checks the machine answers there and
not at 10.0.2.15. The third runs alongside them on the wrap kernel, whose tick counter
starts ten seconds short of 2^32, and times the DISCOVER backoff out to its
64-second cap, across the wrap. `tcpdump` re-decodes every DHCP message the
guest sent, one message at a time. `make warnings` builds everything at five
optimisation levels with the warning gate as errors — at `-O0` a 64-bit
division becomes a call into libgcc, which is not linked, and only that level
shows it.

## What it does

| Subsystem | Notes |
| --- | --- |
| **Boot** | Multiboot 1 header, loaded at 1 MiB, 16 KiB stack |
| **Formatter** | `%c %s %d %i %u %x %X %p %%` into a pluggable sink — the log or the screen — with its own 32-bit integer conversion, so no libgcc helper is ever referenced |
| **GDT** | 6 flat entries: null, ring-0 code/data (`0x08`/`0x10`), ring-3 code/data (`0x18`/`0x20`, DPL 3), and a TSS (`0x28`) |
| **IDT** | 256 gates; vectors 0–31 exceptions, 32–47 IRQs, `0x80` system calls (the only DPL-3 gate); one shared entry stub |
| **Exceptions** | All 32 vectors named and reported, with error code, EIP, and CR2 + decoded cause for page faults |
| **PIC** | Both 8259s remapped off vectors 8–15, per-line masking, central EOI sent before dispatch |
| **Physical memory** | Bitmap allocator over 4 KiB frames, driven by the Multiboot memory map, with a reference count per frame so one frame can be mapped into several address spaces and is freed only by its last holder |
| **Paging** | Two-level page tables, identity-mapped low memory, `CR0.PG` + `CR0.WP`, and per-process address spaces that share the kernel's tables without sharing its user bit |
| **Kernel heap** | `kmalloc`/`kfree` over a 1 MiB region at `0xC0000000`: linked list of blocks, first fit, splitting, and coalescing in both directions |
| **Multitasking** | Preemptive round robin on a 100 Hz PIT, with an assembly context switch and forged first-run frames; the kernel thread is the idle task and runs only when nothing else can |
| **Ring 3** | User-mode segments, a TSS supplying `ss0:esp0`, and system calls through an `int 0x80` gate at DPL 3; a fault in ring 3 kills only that task |
| **IPC** | Synchronous message passing between ring-3 tasks: single-slot mailboxes in the TCB, a blocking `recv` that parks the task, and a scheduler that skips blocked tasks. A task may reserve a second slot for the one sender it trusts, so a driver's messages cannot be crowded out by a process flooding the ordinary slot |
| **ELF loader** | Reads an `ET_EXEC` i386 binary straight out of a Multiboot module, maps each `PT_LOAD` segment into a fresh address space with `.bss` allocated and zero-filled, and spawns it as a ring-3 process |
| **Physical grants** | `sys_map_physical` lets a task map physical memory, gated on a kernel-set capability bit *and* a specific range recorded from the boot loader's module list. Every other process is refused |
| **VFS server** (ring 3) | The `fs_node` dispatch table and the read-only initrd driver, running as an unprivileged process. Answers `MSG_OPEN` and `MSG_READ` over IPC |
| **Client** (ring 3) | A second process with no filesystem code and no grant. It opens and reads a file entirely by asking the server |
| **Shared memory** | `sys_shm_map` creates a segment of one to sixteen pages and returns an id, an address and the size; `sys_shm_attach` maps the same frames, contiguously, into another process at an address of the kernel's choosing. Only the id crosses between processes, and only the creator and the one peer it named may attach |
| **Reaping** | `sys_waitpid` collects a process's own children. The timer tick collects orphans — a zombie whose parent has died — which is the role Unix gives to `init`. Either way tearing down an address space drops a reference on every frame it mapped: private pages are freed, shared pages lose one holder, reserved memory is pinned and never freed |
| **Ring-3 mutex** | `lock cmpxchg` in user assembly, with the lock word living *inside* the shared frame so both processes contend on one physical word. A failed acquisition yields instead of spinning |
| **Keyboard** (ring 3) | The PS/2 driver is a user process. IRQ1 is masked by the kernel and forwarded to it as a `MSG_HARDWARE_INTERRUPT`; the driver reads port `0x60` itself under IOPL=3, decodes scancode set 1 with shift, sends each character to the input server as a `MSG_KEYPRESS`, and re-opens the line with `sys_unmask_irq` |
| **I/O and IRQ grants** | `sys_grant_io` edits the caller's saved EFLAGS so it resumes with IOPL=3 — one OR of `0x3000`, leaving IF untouched — and is refused to every task the kernel did not mark as a driver. `sys_unmask_irq` re-opens a line and is refused to every task but the one the kernel routes that line to |
| **Input server** (ring 3) | A focus manager with no privilege of its own. Applications subscribe by message, the driver sends it every key, and it forwards each to the one subscriber holding focus; Tab is consumed and rotates focus. A dead subscriber is detected by the send that fails and unsubscribed, with focus moving on |
| **Applications** (ring 3) | Two identical programs that subscribe and print what they are given, plus `calc.elf`, which prints and exits. All start from the shell; none at boot |
| **Kernel log** | The kernel has no `kprintf`. Its diagnostics go to an 8 KiB ring buffer through `klog`, read out by a ring-3 process with `sys_klog_read`; `panic_print` writes to the screen and is reserved for ring-0 fatal errors |
| **Console server** (ring 3) | Owns the screen: the only mapping of `0xB8000`, granted by the kernel, and IOPL for the cursor ports. Four virtual terminals, each a 4000-byte backing buffer with its own cursor; output arrives as `MSG_PRINT_STR` messages of up to 31 characters, each placed whole on the sender's terminal, and on the hardware if that terminal is showing. The system console (terminal 0) shows the kernel log and every process without a terminal of its own, line-buffered per sender |
| **Shell** (ring 3) | Prompt, line editing with destructive backspace, `help`, `ls`, `clear`, and any other word is a file to run: opened through the VFS server, copied by it into a shared segment the shell owns, and handed to `sys_spawn`. Holds no privilege |
| **Spawn / exit / wait** | `sys_spawn(shm id, length)`: the caller must own or be peer to the segment; the kernel copies the image out before parsing it, bounds the parse by the stated length, and runs `elf_load`'s full validation. Nothing runs on failure. The child records its parent. `sys_exit(status)` frees the process's user memory and leaves a zombie holding the status; `sys_waitpid(pid, *status)` blocks until a child exits, reads its status and frees the corpse. A fault ends a process the same way, so waiting on a crash returns rather than hangs |
| **Network driver** (ring 3) | An RTL8139, driven entirely from user space. The driver walks the PCI bus (ports `0xCF8`/`0xCFC`) to find vendor `0x10EC` device `0x8139`, reads its I/O base and IRQ, allocates a contiguous DMA receive ring, programs the card over I/O ports under IOPL=3, and claims the discovered IRQ; each receive interrupt is forwarded to it as a `MSG_HARDWARE_INTERRUPT`, and it reads the packet straight out of the DMA ring |
| **DMA / dynamic IRQ** | `sys_alloc_dma(pages, *phys)` hands a driver contiguous, zeroed, pinned physical frames and their physical address, which a device's DMA engine needs (the only call that *allocates* memory to hand back its physical address; `sys_grant_info` reports one the loader placed). `sys_claim_irq(irq)` routes a line discovered at run time to the caller. Both are driver-only, gated on the same capability as I/O |
| **Ethernet (layer 2)** | Each received frame is parsed where it lies in the DMA ring: a `__attribute__((packed))` 14-byte header cast over the first byte past the card's own 4-byte prefix, source and destination MAC formatted as six zero-padded hex bytes, and the EtherType read through `ntohs` and named — IPv4, ARP, IPv6, or left as the bare number. A frame too short to hold a header is reported and skipped without breaking the drain |
| **Transmit / ARP** | Four transmit descriptors over two DMA pages split into 1536-byte buffers — room for a full 1514-byte frame — used strictly in order: the driver copies a frame in, hands the card that buffer's physical address, and writes the length to start the send, padding anything under 60 bytes with zeroes. On top of it, ARP: a request for the address DHCP leased this machine is answered — and none at all before a lease — with a reply built in its own buffer from the request — opcode flipped, sender and target swapped, every multi-byte field through `htons`/`htonl` — and a request for any other address is ignored |
| **IPv4 / ICMP echo** | A received IPv4 datagram is validated in an order where every check reads only bytes an earlier one proved present: version and IHL, total length against the header and against the frame (`ntohs`, so Ethernet padding is never mistaken for payload), a 1500-byte cap, the header checksum over the whole header, no fragment, addressed to the leased address at the IP layer and to this card's MAC at the Ethernet layer (or, for UDP only, to the limited broadcast, which reaches nothing but the DHCP client), and from a sane unicast source. An ICMP echo request that passes is answered with a reply built fresh: Ethernet and IP addressed from this machine's own identity, a rebuilt 20-byte IPv4 header, the request's identifier, sequence and data returned unchanged, and both checksums computed with their fields zeroed first (RFC 1071) |
| **UDP / echo on port 7** | A UDP segment (RFC 768) is checked in the same read-only-what-is-proven order: room for the 8-byte header, the brief's `UDP Packet \| Port S -> D \| Length: L` line, a length at least the header and at most the datagram (anything between the two is ignored), and a non-zero checksum verified over a 12-byte pseudo-header — both IP addresses, protocol 17, the UDP length copied byte for byte from the header — laid in a static 1,492-byte scratch buffer ahead of the segment; the buffer is sized by the 1500-byte IPv4 cap, so no allocation is needed. The data is previewed on its own line with every non-printable byte shown as `.`. Port 7 echoes it back with the ports swapped, from this machine's own address, through the IPv4 reply builder ICMP shares, and a checksum that comes out as 0 is sent as `0xFFFF`. Source ports below 1024 and 2049 are refused, as OpenBSD's inetd does |
| **Kernel alarm** | `sys_alarm(ticks)` gives a task one alarm at the 100 Hz tick rate (`SYS_ALARM_HZ`, the rate the timer is programmed at): when it expires, the task's next `recv` returns a `MSG_TIMER` from the kernel, ahead of interrupts and messages. It is a flag in the control block, like a forwarded interrupt, so nothing another process sends can crowd it out; the tick expires alarms with interrupts off and wakes a task blocked in `recv`. Arming or cancelling discards an expiry not yet collected; 2^31 ticks or more is refused and changes nothing. `sys_ticks` reads the clock alarms count in, so a task kept out of `recv` can still see that its alarm is due. Tested on its own by `alarmtest.elf` |
| **DHCP client** (RFC 2131) | The machine boots at 0.0.0.0 and leases its address, mask and router: DISCOVER, OFFER, REQUEST, ACK on UDP 68/67, one xid throughout, broadcast both ways. Every reply is checked — source port 67, op, htype, hlen, the xid, the client's own hardware address, the magic cookie through `htonl`/`ntohl` — and its options walked with every byte proved inside the message before it is read, End required, repeats concatenated (RFC 3396), overload refused. The offered address, mask (contiguous, /8 to /30) and router (on the subnet, not the network or broadcast address, not the lease) are judged before anything is used, and the ACK judged again. DISCOVER is retransmitted at 4, 8, 16, 32 and then every 64 s, REQUEST four times before the client starts over, all ±1 s on the kernel alarm; every restart — after a NAK, a give-up, a lease's end — waits a second or two; the lease is counted from the REQUEST and ends when it runs out, even while a flood keeps the receive loop busy |

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
src/cpu/irq.c           handler registration, dispatch, EOI; which ring-3 task
                        owns a line, and forwarding a line to its owner
src/cpu/pic.c           8259 remap and masking
src/drivers/vga.c       the kernel's text driver: clears the screen at boot,
                        then used by panic alone
src/drivers/keyboard.c  the kernel half of the keyboard: drains the controller at
                        boot, hands IRQ1 to the ring-3 driver, drains again if
                        no driver is alive. No scancode table
src/drivers/pit.c       programmable interval timer, 100 Hz tick hook; the
                        counter's start is a build knob for the wrap kernel
src/mm/pmm.c            physical frame allocator
src/mm/paging.c         page directory, page tables, map_page, address spaces
src/mm/paging_enable.S  loads CR3, sets CR0.PG and CR0.WP
src/mm/kheap.c          kmalloc / kfree over a linked list of blocks
src/mm/shm.c            id -> physical frame registry for shared pages
src/task/task.c         task control blocks, process creation, physical grants
src/task/elf.c          ELF32 loader: validates a module, maps its PT_LOAD segments
src/task/switch.S       context switch and the first-run bootstrap
src/task/scheduler.c    round robin over runnable tasks; schedule(); alarms expire
                        on the tick
src/sys/syscall.c       int 0x80 dispatcher: send, recv, yield, map_physical,
                        grant_info, shm_map, shm_attach, grant_io, unmask_irq,
                        trust_sender, map_hw_buffer, klog_read, spawn, parent_of,
                        exit, waitpid, alloc_dma, claim_irq, alarm, ticks. No print
src/sys/uaccess.c       copy_from_user / copy_to_user with per-page validation
src/ipc/ipc.c           ipc_send / ipc_recv over per-task mailboxes, the
                        pending-interrupt bits recv turns into messages, and the
                        slot reserved for a task's one trusted sender

  --- everything below this line runs in ring 3, in its own address space ---

src/user/lib/ulib.c     the user runtime: syscall wrappers, strings, number
                        formatting. Linked into every program
src/user/lib/stdio.c    u_print and u_printf: format, then MSG_PRINT_STR
                        chunks of up to 31 characters to the console server
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
src/user/kbd_server/    the keyboard driver: raises its IOPL, waits for the
                        kernel's interrupt message, reads port 0x60, decodes,
                        sends each key to the input server, unmasks IRQ1
src/user/input_server/  the focus manager: subscriptions by message, one focused
                        pid, Tab rotates, a dead subscriber is unsubscribed by
                        the send that finds it gone
src/user/apps/app_a/    two identical applications that subscribe and print
src/user/apps/app_b/    what they are given; only focus tells them apart
src/user/vga_server/    the console: maps 0xB8000, four virtual terminals, the
                        hardware cursor, the kernel log on terminal 0
src/user/apps/shell/    the shell: line editing, built-ins, open + load through
                        the VFS server, sys_spawn
src/user/apps/calc/     the smallest spawnable program: prints and exits
src/user/apps/alarmtest/ the kernel alarm's own test, run from the shell
src/user/net_server/    the RTL8139 driver: PCI scan, DMA ring, IRQ claim, RX,
                        Ethernet parsing, transmit (rtl8139.c), ARP (arp.c),
                        IPv4 validation and the reply header (ipv4.c), ICMP
                        echo (icmp.c), UDP with the echo service (udp.c), the
                        DHCP client (dhcp.c) and the leased configuration
                        (netcfg.c)
src/user/lib/net.c      byte order (htons/ntohs/htonl/ntohl), the RFC 1071
                        checksum, the Ethernet header writer, and the formatters
                        for MAC addresses, IPv4 addresses and EtherTypes
include/arch/pci.h      PCI config mechanism #1 (0xCF8/0xCFC) readers
include/arch/rtl8139.h  RTL8139 register map and bits
include/net/ethernet.h  the packed 14-byte Ethernet header and EtherTypes
include/net/arp.h       the packed 28-byte ARP header and opcodes
include/net/ipv4.h      the packed IPv4 header, its field helpers, and the
                        header builders every datagram goes out through
include/net/icmp.h      the packed ICMP echo header and its types
include/net/udp.h       the packed UDP header and pseudo-header, and the ports
include/net/dhcp.h      the packed 236-byte BOOTP/DHCP header, the cookie, options
include/net/netcfg.h    the leased configuration: address, mask, router, lease
include/net/checksum.h  the Internet checksum's contract, and why one's complement
include/net/rtl8139.h   the driver module's own interface: transmit and the
                        card's MAC (include/arch/rtl8139.h is the register map)
include/net/byteorder.h host and network byte order, and why they differ

tools/make_initrd.py    host-side packer; writes the format initrd.h declares
tools/inject_frames.py  writes real Ethernet frames into QEMU's network card and
                        decodes what comes back; the one definition of a test
                        frame, and the RFC 768 reference, which `make netdemo`,
                        `tools/ping.py` and `tools/udp_echo.py` use
tools/ping.py           plays the host for `make ping`: echo requests in, every
                        reply checked, refusals tested, liveness after each
tools/udp_echo.py       plays the host for `make udp`: datagrams to port 7,
                        every echo checked, refusals, the console read back
tools/dhcp_test.py      `make dhcp`: the only DHCP server on the guest's segment,
                        three boots (one on the wrap kernel), every refusal
                        and timer of the client
tools/udp_host.py       `make udp-host`: the host's own nc through QEMU's
                        user-mode network and its port forward
tools/check_checksum.py builds net.c natively and checks net_checksum against
                        RFC 1071's example, a reference, and its own properties
initrd/                 files packed into the image, one per entry
linker.ld               link map for the kernel image, loaded at 1 MiB
user.ld                 link map for ring-3 binaries, at 0x40000000
src/utils/stdio.c       the formatter, panic_print and panic
src/utils/klog.c        the kernel log: a ring buffer read out by ring 3
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
teardown, so this phase also had to add the reaper, which ran in the idle task
because a process cannot free the page directory it is executing on (phase 19
moved it to the timer tick, once `sys_spawn` let any program start a task that
never blocks and so starve the idle loop forever). A review
then found the shared-memory ids were checked for existence but not entitlement,
which let any process read every shared page by counting upwards.

**15. A userspace mutex.** `lock cmpxchg` in ring-3 assembly, with the lock word
inside the shared frame so both processes contend on one physical word. On a
single core the `lock` prefix is nearly redundant — interrupts are taken at
instruction boundaries, so one instruction cannot be split — and what actually
provides safety is that the read-modify-write *is* one instruction where the C
equivalent is four. The prefix earns its keep on a second core.

**16. A ring-3 keyboard driver.** The scancode table left the kernel. IRQ1 now
masks its own line, marks the interrupt pending on the driver process and
switches to it; the driver reads port `0x60` itself and re-opens the line when
it is done. It can read the port because the kernel raised its IOPL — one OR of
`0x3000` into the EFLAGS image saved by a system call, which sets bits 13:12 and
nothing else, so IF at bit 9 survives and the `iret` that ends the call loads
the result. Two decisions were about what *not* to do. The interrupt is a bit in
the control block rather than a message in the driver's single mailbox slot,
because any process can fill that slot and would otherwise be able to make the
driver miss keystrokes. And IOPL goes only to the task the kernel started as the
driver, because IOPL=3 is every port on the machine and `cli` besides.

**17. An input server as focus manager.** The keyboard driver stopped printing
and started sending: every decoded key goes as a `MSG_KEYPRESS` to a third
server, which forwards it to whichever subscribed application holds focus and
consumes Tab to move focus along. The three core servers moved to the front of
the module list so they hold pids 1–3. The design question was a dead
subscriber. The kernel offers no notification, so death is detected where it is
visible — a send that fails with `IPC_ERR_NO_TASK`, deterministic both before
and after reaping because pids are never reused — and the recovery drops the
keystroke that exposed it rather than handing it to whichever application
comes next. A full mailbox is the other failure and gets the other answer: a
bounded retry, then a drop, with the subscription kept. The review then found
the hole the design had reopened: one unprivileged process looping a send at
the server's well-known pid took every keystroke on the machine, 0 of 10
delivered. The answer is a second mailbox slot a task reserves for the one
sender it names — the pending-bit idea from phase 16, one hop further from the
hardware — so the driver's keys and the server's forwards contend with nobody.
Same flooder afterwards: 10 of 10.

**18. A ring-3 console.** The kernel lost `kprintf`. What it had to say became
a log — a ring buffer with a read syscall — and a fourth server, holding the
only mapping of the VGA text buffer and IOPL for the cursor ports, shows that
log and every process's output on four virtual terminals in its own memory.
A process no longer prints; it sends the console one character per message,
and the kernel-stamped sender is what decides which terminal the character
lands on. Focus and screen move together: Tab shows the focused process's
terminal, Escape shows the system console without moving focus. Two details
mattered more than the rest: the hardware cursor is a cell index, `row*80 +
column`, not a byte offset, split across two CRTC registers through an
index/data port pair; and a shared terminal must be line-buffered per sender,
or one-character messages from ten processes arrive as one line of alternating
characters.

**19. A shell, and `sys_spawn`.** The kernel now starts five processes and no
more; everything else is a file the shell asks it to run. The shell opens the
file through the VFS server, which copies it into a shared segment the shell
created with the server as its one permitted peer, and then calls `sys_spawn`
with the segment id and the length. The kernel trusts neither: the caller must
own or be peer to the segment; the image is copied into kernel memory before
a byte is parsed, so nothing still mapped writable elsewhere is what gets
validated; the parse is bounded by the stated length so a short file cannot
reach stale bytes from the last program loaded there; and the same `elf_load`
that vets boot modules vets this, refusing bad magic, the wrong class or
machine, a header or segment past the end, an entry outside the image, or a
segment too large to be honest. Shared segments grew to sixteen pages for
this, a process table cap went in so a spawn loop hits a wall, `sys_exit`
lets a program end, and a child prints where its parent does because the
console asks the kernel who spawned it. Output became up to 31 characters per
message the moment a terminal had two writers.

**20. Process exit and parent/child wait.** A process ends with `sys_exit(status)`
rather than merely being marked dead: it frees its own user memory immediately —
the bulk of what it held — and becomes a zombie holding its status, so a parent
can learn how it fared. `sys_waitpid(pid, *status)` is the rendezvous: a parent
blocks until the named child exits, reads its status, and frees the corpse's
page tables, directory and control block. The shell waits on every command, so
the prompt returns only when the program is done. A ring-3 fault now ends a task
the same way — a zombie with a status carrying the trap vector — because the
shell waits on a child that might crash, and a crash that did not wake the
waiter would hang the shell forever. The orphan case, a child whose parent dies
before waiting, is handled by the tick reaper doubling as `init`: it collects
any zombie whose parent is gone, so a corpse is never left uncollected.

**21. A network card, over DMA.** An RTL8139 driven from ring 3. The driver
finds the card on the PCI bus, and the kernel gains the two things a device
driver needs beyond I/O ports: `sys_alloc_dma`, which allocates contiguous
physical frames and returns their physical address, because a DMA engine
addresses physical memory the process's virtual addresses mean nothing to
(`sys_grant_info` also hands back a physical address, but of a boot module the
loader placed, not freshly allocated memory); and
`sys_claim_irq`, which routes a line the driver discovered at run time, since
the kernel cannot know a PCI device's interrupt before the bus is scanned. The
PMM learned to find a contiguous run of free frames (first-fit over runs, not
frames) and to pin them, because a card keeps the physical pointer it was
handed and must never DMA into memory the kernel has since reused. Everything
that could be a hardware fact rather than kernel knowledge is: the kernel never
learns what a packet is, only that a line the driver claimed fired. Verified by
injecting Ethernet frames into the emulated card and watching the driver read
them out of the ring.

**22. Reading the frame.** Until now a packet was a length. Layer 2 makes it a
structure: a 14-byte Ethernet header cast in place over the DMA ring, six bytes
of destination MAC, six of source, and an EtherType naming what is inside.

Two things stand between a C struct and a wire format, and both are silent when
they go wrong. The first is padding — GCC lays out a struct for the 32-bit ABI,
aligning members and rounding the total size up, which is right for a struct the
compiler owns and wrong for fourteen bytes another machine wrote.
`__attribute__((packed))` is what makes the cast an identity rather than a hope,
and `_Static_assert` on the size, the alignment and every member's offset turns
the promise into a build failure instead of a run-time misread. The alignment
assert is there because measuring showed the others were not enough: deleting
the attribute leaves this struct's size and offsets identical and only its
alignment changes, so the offset checks alone would have waved the edit through. The second is byte order:
the wire is big-endian, x86 is little-endian, and an EtherType read without
`ntohs` is 0x0008 rather than 0x0800 — not a protocol at all, and nothing
faults. Both conversions and both formatters live in the user runtime, not the
driver, because byte order is a property of the architecture and a MAC address
formats the same way whoever prints it.

The parsing is bounds-checked before it is a cast: the card's length field is 16
bits and the buffer is 12 KiB, so a frame longer than Ethernet allows is never
read. Getting the *recovery* right took a second attempt. The first version
stopped the drain on a bad length — which sounds careful and was in fact a
permanent, silent denial of service, because stopping without moving the read
offset left the driver parked on that same header for the rest of the machine's
uptime. A review injected one 1600-byte frame and watched every valid packet
after it vanish. Now an over-long frame is stepped over by the length the card
reported (one packet lost), and a header that cannot be believed at all
resynchronises to the card's own write head (what was in flight is lost). Both
move `CAPR`, which is the property that actually matters.

**23. Saying something.** Until this point the machine only ever listened. TX
is four descriptors — not a ring, just four slots used in rotation, each pairing
a register holding a buffer's physical address with one that is write-the-length
-to-send and read-it-back-for-status. The handshake bit reads backwards from its
name: the driver *clears* `OWN` by writing a length, meaning the card owns the
buffer now, and the card *sets* it when the DMA is done. Waiting for a free slot
is waiting for `OWN` to become 1.

On top of that, ARP, which is the smallest protocol worth implementing and the
first one where this system answers rather than observes. A request for
`10.0.2.15` comes in; the reply is the request turned around, with the opcode
changed and the sender and target swapped, and every multi-byte field run
through `htons`/`htonl` on the way out.

Byte order is where this would have failed silently. An IPv4 address written as
`0x0A00020F` reads left to right like the address it is, but x86 stores it in
memory as `0F 02 00 0A` — backwards from the wire — so `htonl` is not decoration,
it is the difference between claiming to be `10.0.2.15` and claiming to be
`15.2.0.10`. The MAC addresses need none of it, because a byte array has no
endianness. And `arp_header_t` is the first struct here where `__attribute__
((packed))` changes the layout rather than just the alignment: a `uint32_t` lands
at offset 14, so without packing the compiler inserts two bytes, shifts every
field after it, and grows 28 bytes into 32.

Proven both ways. Against QEMU's own stack, the driver ARPs the gateway at
startup and the gateway replies — a real peer answering, which it would not do
if a single field were byte-swapped wrong. Against an injected request, the
frame the guest puts on the wire is read back and decoded byte by byte: opcode
2, sender `52:54:00:12:34:56` / `10.0.2.15`, target the asker, padded 42 to 60.
A request for an address this machine does not own draws no reply at all.

**24. Answering a ping.** Above ARP, the first IP. A received IPv4 datagram is
validated in an order where every check reads only bytes an earlier one proved
are there — the frame long enough for a header, then the header's own length,
then the datagram's length against the header and the frame, and only then a
checksum over bytes now known to be present. What passes and is an ICMP echo
request gets an echo reply.

The checksum is RFC 1071's: the one's complement of the one's-complement sum of
16-bit words. One's-complement addition feeds the carry out of bit 15 back into
bit 0, which makes it arithmetic modulo 65535 rather than 65536, and that single
difference is the design. 256 × 256 = 65536, which is 1 modulo 65535, so
swapping a word's bytes is the same as multiplying it by 256 — the sum of swapped
words is the swapped sum, and machines of either byte order compute identical
checksums. No carry is ever thrown away, so every bit position is
protected alike: two top-bit errors in the same direction change a two's-
complement sum by 65536, which is 0, but a one's-complement sum by 1. (Errors in
opposite directions in the same bit still cancel under either — an earlier
version of this paragraph claimed they did not, and `tools/check_checksum.py`
now tests both directions.) And the check needs no knowledge of where
the checksum lives: sum everything, checksum included, and a correct message
comes to zero. The one rule that matters in code is the brief's: zero the field
before computing, because the checksum is defined over the header as if it were
zero.

The reply is built fresh rather than by swapping the request's fields in place.
For a request unicast to this machine a swap gives the same answer; the
difference is in what a swap copies blindly — a broadcast destination MAC
becoming the reply's source, the request's options and TTL and checksum riding
back. The transmit buffers grew to 1536 bytes for it, because an echo reply is
exactly as long as its request and a full-size ping is a 1514-byte frame.

The host's own `ping` never ran, and could not: user-mode networking is NAT with
no inbound ICMP, and the TAP device that would give the host a route needs root.
So `make ping` plays the host over a socket netdev, and the evidence is built to
be hard to fool. Every reply is checked field by field against a checksum written
from the RFC rather than from `net.c`. Every refusal is followed by a proper ping,
because silence proves nothing about a guest that might have crashed. And every check is shown to be load-bearing: eighteen
mutant kernels, each disabling exactly one test, and `make ping` fails against
every one, each time on the case named for the check it removed. That took a
second attempt. The review found that my first suite built its malformed
headers with checksums over the wrong bytes, so four refusal cases were really
stopped by a checksum failure and deleting the check each was named for — the
IHL check, the frame bound, the 1500-byte cap, the ICMP minimum length — left
it green. One of those, the frame bound, is what keeps bytes from past the end
of a frame from being echoed back onto the wire. `tcpdump`, reading only what
the guest sent, counts 46 echo replies and no checksum complaints.

**25. UDP, and an echo server.** The first transport layer: two port numbers, a
length, and a checksum that covers not only the datagram but a pseudo-header of
fields borrowed from the IP header — both addresses, the protocol, the length —
so that a datagram delivered intact to the wrong machine still fails it. The
pseudo-header is never sent. It is laid in a scratch buffer ahead of the segment
only to be summed, and the question this phase was asked is how that buffer is
sized when a payload's length is only known on arrival. It is not sized on
arrival: the longest possible segment is fixed at compile time by checks that
run before UDP sees a byte — the 1500-byte IPv4 cap, a header of at least 20,
and a segment no longer than its datagram — so a static 1,492-byte buffer holds
every one, and the length that arrives only decides how much is summed. Ring 3
has no heap to allocate from anyway.

Port 7 answers with the same bytes. One's complement has two zeroes, and UDP
spends one of them — a transmitted 0 means "no checksum" — so a checksum that
comes out as 0 goes out as `0xFFFF`, which verifies just the same. And
whenever a request carries a checksum, the echo's is equal to it, since swapping
the addresses and ports only reorders the sum; a guest that copied it instead of
computing it would pass any test that sends a checksum, so the tests also send
requests without one. Echo refuses source ports below 1024 and NFS's 2049,
because echo answering echo or chargen never stops, and because it would
otherwise hand bytes an attacker chose to NFS, which trusts a client by its
port.

Unlike ping, this reaches the host: QEMU forwards `127.0.0.1:7007` to the
guest's port 7, and `nc -u 127.0.0.1 7007` gets back what it sends, through a
UDP/IP stack this project did not write. `make udp-host` does it headless with
the host's own netcat and reads the guest's screen out of VGA memory to see the
datagram arrive. That path cannot prove the checksum is computed: QEMU's stack
always hands the guest a checksummed request, and a correct echo's checksum
equals it, so a guest that copied it would pass — while a missing checksum
would be caught there too, by the same `tcpdump` gate. So `make udp` proves it
over a socket netdev, where a request can carry none. Fifty-five mutant kernels,
each deleting one check or breaking one rule — the pseudo-header's protocol, the
zero rule, the port swap, a missing `ntohs`, a reply sent to the wrong host —
and every one that can be caught is caught by the case named for it; two
guards nothing can reach are shown to be exactly that.

**26. Asking for an address.** Until now the machine's address was a constant:
10.0.2.15, a /24 and a gateway, written into the address checks, the ARP answers
and every reply header. Now it boots with none and asks. DHCP is four messages —
DISCOVER, OFFER, REQUEST, ACK — all broadcast here, because the client asks for
that (the BROADCAST flag) and accepts no unicast until it has an address, and all
carrying the transaction id the client picked.
The machine answers nothing until the ACK: no ARP, no ping, no echo, not even at
0.0.0.0, since an address it has not been given is not one it may answer for.
IPv4 gains exactly one way in for a broadcast — UDP to 255.255.255.255, which
UDP hands only to the DHCP client — so the echo service and ICMP still never
answer one.

The question this phase was asked is how to walk DHCP's options, which are
`[code][length][data]` records ended by an End option, safely enough to pull the
subnet mask and the router out of a stranger's message. One cursor walks from
after the magic cookie to where the UDP payload ends — a bound the IP and UDP
layers have already proved lies inside the frame — and every byte is proved
inside before it is read: a code byte, then, unless it is Pad or End, a length
byte, then the length checked against what is left as `len > end − p` (never
`p + len > end`, which would form a pointer past the object). Unknown options are
stepped over by their length; End is required, and nothing after it is read;
options that repeat are concatenated, as RFC 3396 says, and their lengths
checked on the total. Nothing the options say is used until the whole message
has parsed and the offered address, mask and router have been judged — so a
message malformed at its fortieth option has configured nothing.

Retransmission needs time, and ring 3 had none: the network server woke only
for its card. So the kernel gained an alarm, `sys_alarm`, delivered the way a
forwarded interrupt is — a flag in the control block that `recv` turns into a
`MSG_TIMER` from the kernel — and the client resends on it with RFC 2131's
randomised backoff, gives up on a silent server and starts over, restarts a
second or two after a NAK, and drops its address when the lease runs out —
counted, as RFC 2131 says, from when the REQUEST went out, which needed a clock
ring 3 could read: `sys_ticks`. The clock also closed a hole the review found:
a flood of frames kept the network server draining its receive ring, never
back in `recv` where the alarm is delivered, so a lease outlived its end for as
long as the flood lasted. The receive loop now asks the client, before every
frame, whether its alarm is due. Every restart waits a second or two, so no
server, however it answers, can make the client transmit faster. The
alarm has its own test program, because a DHCP exchange cannot aim at the cases
that matter: an alarm expiring while its owner is busy, then cancelled before
it looks. The xid comes from the timestamp counter, mixed with the MAC; there is
no other entropy in the machine, and the xid needs to be unique, not secret —
every message carrying it is broadcast.

Against QEMU's own DHCP server the whole exchange takes about five
milliseconds. `make dhcp` plays a server that tries to break it instead, and
leases a second boot 10.0.2.42/16 to prove the address is the one leased and not
a constant.

Every phase was reviewed adversarially afterwards, by building variants and
booting them, and the reviews found real defects in most of them: a page
directory that was never zeroed, a buffer overflow in a directory listing, an
unbounded retry that let one client hang a system service, the capability hole
above, and a driver-death path that reported the keyboard controller's stale
acknowledge byte as a dropped keystroke, and an unprivileged flood that took the
keyboard away from every process among them. Where a review refuted a
claim, that is recorded too.

## Design invariants

These are the things that break quietly if you change one half of a pair. Each
one caused, or would have caused, a real bug.

- **Every object depends on the Makefile itself**, so changing a compiler flag
  forces a full rebuild instead of leaving stale objects with mismatched ABI.
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
  fails the build rather than letting that happen quietly. It is also what lets
  `kfree` refuse a pointer that is not `KHEAP_ALIGNMENT`-aligned: every payload
  is, so such a pointer is none, and the header in front of it would be
  misaligned too.
- **A message never moves user-to-user in one motion.** `send` copies the
  sender's buffer into the *receiver's* kernel-owned mailbox; `recv` copies that
  mailbox into the receiver's own buffer when it wakes. Each half validates
  exactly one task's memory, and the kernel overwrites `sender_pid` rather than
  trusting it.
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
  why a ring-3 fault ends the task as a zombie (`TASK_ZOMBIE`, its status the
  trap vector) rather than merely halting it — so a parent waiting on it wakes.
- **Pids are never reused.** Dead tasks stay linked in the ring and `task_find`
  has no state filter, so a recycled pid could resolve to the corpse or the new
  task depending on who is asking; and `sender_pid` is a bare integer, so a
  reply to a pid that died and was reissued would reach the new holder
  undetectably. Reuse needs unlinking and a generation counter first.
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
- **Well-known pids are fixed by creation order, and the core servers come
  first.** A program has to name the VFS server, the keyboard driver or the
  input server or the console before it has spoken to anything, so those four
  constants are compiled in and the kernel creates those four processes first,
  in that order, so they hold 1, 2, 3 and 4. The shell is 5, and nothing
  addresses it. Every well-known pid is checked at boot
  rather than assumed — and a module that fails to load still consumes its
  pid, because otherwise the program after it inherits the address and
  receives everything sent there, which no check can see since the process
  the check is looking for does not exist.
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
- **A process cannot free the address space it is executing on.** So teardown
  is split. `sys_exit` and the fault path free only the *user leaf frames*,
  which is safe to do as the exiting task because it returns them to the
  allocator and then reads no user memory before it yields, with interrupts
  off so nothing else can be handed one meanwhile. The page tables and the
  directory — the structure the CPU is translating through — are freed later,
  by whoever collects the corpse, running on another address space: a parent
  in `sys_waitpid`, or the tick reaper for an orphan. Neither ever collects the
  task it runs on.
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
- **IOPL is raised with one OR, on the saved frame, never on the live
  register.** `eflags |= 0x3000` sets bits 13:12 and touches nothing else, so
  IF at bit 9 — the neighbour that matters — and every arithmetic flag come
  through as the task left them. Assigning `0x3000` instead would clear IF and
  resume the task with interrupts off, never to be preempted again. It has to
  be the frame because `iret` at CPL 0 is the only instruction that loads IOPL;
  a `popf` from ring 3 leaves the field alone without faulting. Once set it
  travels with the task, since every interrupt saves that task's EFLAGS into
  its own frame, and it reaches no other task.
- **A forwarded interrupt is a bit, not a message.** Any process can fill the
  driver's single mailbox slot. If the interrupt had to land there, a hostile
  peer could make the driver miss keystrokes at will; as a pending bit it
  cannot be crowded out, and `recv` turns it into a `MSG_HARDWARE_INTERRUPT`
  ahead of anything waiting in the slot. One bit per line is exactly the right
  size, because the line is masked from the moment it is forwarded until the
  driver reopens it, so at most one event per line can ever be outstanding.
- **A line is masked before its owner is told, and only its owner may reopen
  it.** Letting any process unmask any line would hand it the interrupt
  controller. Ownership is recorded as a pid rather than a task pointer so a
  driver that dies and is reaped leaves nothing dangling — `task_find` simply
  stops finding it, and the kernel's fallback takes over.
- **EOI-before-callback still holds for a forwarded line.** The brief's order
  (mask, send, acknowledge) and the kernel's (acknowledge, mask, send) are
  indistinguishable to the hardware: the gate cleared IF, so nothing is
  delivered until the `iret`, and by then the mask is in place. Moving the EOI
  after the send would reintroduce the exact hazard the central EOI exists to
  prevent, since forwarding switches tasks and does not return.
- **The kernel reads the byte itself when no driver is alive.** The 8042 holds
  IRQ1 asserted while its output buffer is full, the 8259 is edge triggered,
  and a line that never falls never rises again. Dropping a scancode costs one
  keystroke; leaving it in the controller costs the keyboard until reboot.
- **Interrupts are routed to the task the kernel created, not to the
  constant.** `KBD_SERVER_PID` is a checked contract like the other well-known
  pids, but the route and the I/O grant name `kbd->pid`, so an earlier module
  failing to load shifts the pid and produces a warning rather than a dead
  keyboard.
- **The sender that matters gets a slot nobody else can fill.** A single
  mailbox slot is first come, first served, and a process that knows a
  server's well-known pid can keep it full from a tight loop; measured, one
  such process cost every keystroke on the machine. `sys_trust_sender` reserves
  a second slot for one named sender, drained ahead of the first. It changes
  only the caller's own mailbox, so it needs no privilege, and it protects
  exactly one relationship — the driver into the input server, the input
  server into each application — which is the one that has to work.
- **A dead pid is detected by the send, and only by the send.** The kernel
  marks a faulting task dead at once and reaps it later, and `ipc_send`
  refuses both states with `IPC_ERR_NO_TASK`; pids are never reused, so the
  number cannot come to mean a live process. There is no window in which a
  send to a dead process succeeds or is ambiguous, which is what lets the
  input server treat one return code as the whole death notification.
- **The keystroke that exposes a death is dropped, not rerouted.** The user was
  typing into the process that died. Delivering that character to whichever
  application is next is the one recovery worse than losing it. One lost
  keystroke per crash, never a misrouted one, and a printed line saying focus
  moved.
- **A full mailbox is not a dead process.** `IPC_ERR_FULL` means alive and
  slow; `IPC_ERR_NO_TASK` means gone. The input server retries the first a
  bounded number of times and keeps the subscription, and unsubscribes on the
  second. Conflating them would either unsubscribe every application that fell
  one keystroke behind or retry forever into a corpse.
- **Keys are accepted from the driver's pid and from nowhere else**, and
  applications accept keys from the input server's pid and from nowhere else.
  Both checks are on the kernel-stamped sender. Without the first, any process
  could type into any application through the server; without the second, it
  could do so directly.
- **The kernel does not print.** There is no `kprintf`. Diagnostics go to the
  kernel log and are shown by the console server, which owns the screen; the
  kernel writes to the screen only to panic, when there is nothing above it
  left to trust. A ring-3 fault is a diagnostic and goes to the log; a ring-0
  fault is the end and goes to the screen.
- **The screen is a capability.** One page of physical memory, mapped writable
  into exactly one process by a kernel-set bit no system call can set. Every
  other process's output is a message whose sender the kernel stamps, so no
  process can write on another's terminal or pretend to be the input server
  and change what is showing.
- **The hardware cursor is a cell index, not a byte offset.** `row * 80 +
  column`, written as two bytes to CRTC registers `0x0E`/`0x0F` through the
  `0x3D4`/`0x3D5` index/data pair. Writing the byte offset puts the cursor
  twice as far along the screen.
- **A shared terminal is line-buffered per sender.** Output arrives one
  character per message and processes run interleaved, so the system console
  holds each sender's text until its newline and then places the line whole.
  A process's own terminal has one writer and shows each character at once.
- **The log reader's position is an offset into everything ever logged.**
  `sys_klog_read` takes the offset the reader has reached and returns where
  the delivered bytes actually start, which is later if the ring has wrapped
  past the reader; the next offset is start plus length. There is no separate
  "you missed some" flag to get out of step with the data.
- **The reaper never reaps the task it runs on.** It walks the ring from the
  interrupted task and skips it, and runs only from the timer tick with
  interrupts masked. It ran in the idle task until `sys_spawn` let any program
  create a task that never blocks — two bytes, `jmp` to self — which starved
  the idle task forever and with it every reclamation, until the process table
  filled for good; and it must not also run anywhere interrupts are enabled, or
  two reapers, one interruptible by the other, would race inside `kfree`.
- **Copy, then validate, then run.** `sys_spawn` copies the image out of the
  shared segment into kernel memory before parsing it. The frames stay mapped
  and writable in two ring-3 processes, and validating bytes something else
  can still write is a time-of-check/time-of-use hole that a second core would
  make real. On one core the syscall runs with interrupts off and nothing can
  interleave; the copy is what keeps that from being load-bearing.
- **The stated length bounds the parse.** A segment is reused for every
  program the shell runs, so bytes of the last program sit past the end of the
  current one. Without the caller's length, a truncated file's headers could
  point into them and the kernel would assemble a program out of two.
- **Every process comes to exist through one function.** Boot modules and
  spawned images both go through `process_spawn`: `elf_load` then
  `create_user_process`. One set of checks, applied identically, and a fix to
  either path is a fix to both.
- **A process cap is what a spawn loop hits.** The identity window is sized at
  boot for a fixed number of processes; past it, frames stop being reachable
  and the failure mode is obscure. Refusing creation at that number makes the
  failure a line in the log instead.
- **A child prints where its parent prints, and the kernel is the one that
  says who the parent is.** The console asks with `sys_parent_of` rather than
  believing any message: a process claiming another as its child would
  otherwise be able to redirect that process's output onto its own terminal.
- **A terminal with two writers needs atomic messages.** One character per
  message was correct while every terminal had one writer; a shell and its
  child interleaved at character granularity. Up to 31 characters per message,
  placed whole, is the fix; the console's per-sender line buffer still guards
  the shared system console against longer lines.
- **Exit frees the frames; wait frees the shell.** A zombie holds only its
  page tables, directory, kernel stack and control block — kilobytes — not its
  user pages, which went at exit. So a parent that is slow to wait, or the one
  tick before the orphan reaper runs, costs the corpse's bookkeeping and not
  its whole footprint.
- **A fault is an exit.** A ring-3 fault produces a zombie with a status
  carrying the trap vector, identical in every other way to `sys_exit`. One
  collection path, one wake path — and a parent waiting on a child that
  crashes is woken and told, instead of blocking on a corpse that never
  reported. Making a fault merely halt the task would deadlock any waiter.
- **Only a parent reaps its child, and the reaper reaps orphans.** `sys_waitpid`
  refuses a pid that is not a living child of the caller, so a process cannot
  read another's exit status or free its corpse. A zombie whose parent has died
  — or whose parent is the kernel, which never waits — is an orphan, collected
  by the tick reaper; pids are never reused, so a `parent_pid` can never come to
  name a different live task and cause a wrongful collection, and a parent
  blocked in `waitpid` counts as alive so the reaper never races it.
- **Collection is all-or-nothing.** A zombie is torn down completely — page
  tables, directory, kernel stack, control block — by its parent's `waitpid` or,
  if orphaned, by the next tick. Its user frames went at exit and its pid is
  never reused.
- **A physical address is a driver's business, not a process's.** Two calls
  hand one to ring 3 and both are privileged: `sys_grant_info` reports where the
  loader put a module (gated on `may_map_physical`), and `sys_alloc_dma`
  allocates fresh contiguous memory and returns where it landed (gated on
  `may_use_io`), because a device's DMA engine speaks physical and an ordinary
  process has no business knowing where its pages live. The frames are contiguous (a device DMAs across
  one buffer), zeroed (a recycled frame carries another process's bytes), and
  pinned once mapped — a card keeps the pointer after its driver dies, so the
  frames must never return to the pool to be reused under it. The cost is that
  a DMA buffer is never reclaimed; a driver that restarts leaks it.
- **Contiguity is searched, not assumed.** `pmm_alloc_contiguous` is first-fit
  over *runs*: a used frame resets the run, so only genuinely adjacent free
  frames accumulate to the count. Fragmentation is a real, reported failure —
  there is no compaction, since a mapped frame cannot be moved — not something
  papered over.
- **A driver claims its own interrupt line, and the kernel guards which.**
  `sys_claim_irq` routes a discovered line to the caller, but refuses the timer,
  the keyboard, the cascade (IRQ 2, through which every slave line reaches the
  CPU), and any line another driver already owns. A claimed line installs a
  generic forwarder; when its driver dies the line is masked and the handler
  removed rather than reopened, because there is no generic way to drain an
  arbitrary device and a reopened unstaffed line would storm the CPU.
- **A struct cast over wire data is packed, asserted, and byte-swapped.** The
  compiler's layout rules serve the compiler's own structs; fourteen bytes
  written by another machine answer to IEEE 802.3 instead. `ethernet_header_t`
  carries `__attribute__((packed))` so its layout *is* the wire's, and
  `_Static_assert` on its size, its alignment and each member's offset makes a
  future edit that reintroduces padding — or deletes the attribute — a build
  failure rather than a silent misparse. Every
  multi-byte field is read through `ntohs`/`ntohl`, never used raw. Packing also
  drops the struct's alignment to 1, which is what makes casting it over an
  arbitrary ring offset correct rather than lucky.
- **A malformed frame must not be able to kill the driver — and "kill" includes
  going quiet.** The card's length field is 16 bits and the DMA buffer is 12 KiB,
  so an over-long frame is never read; it is stepped over using the length the
  card itself reported, costing one packet. A frame too short to hold a header is
  reported and skipped. A header that cannot be believed at all — no receive bit,
  or too short to hold even the CRC — resynchronises to `CBR`, the card's own
  write head, discarding what was in flight. **Every path out of the drain moves
  `CAPR`.** That is the real invariant, and the first version of this check broke
  it: it stopped the drain without moving the read offset, so one over-long frame
  parked the driver on the same unreadable header forever and every later packet,
  valid or not, was silently lost. A review found it by injecting one 1600-byte
  frame. Trading a memory fault for a permanent silence is not a fix; it is the
  same denial of service, harder to notice.
- **A frame shorter than the Ethernet minimum is padded, with zeroes.** The card
  does not pad for us, and a 42-byte ARP reply sent as 42 bytes is a runt a real
  switch would drop. The padding is explicitly zeroed rather than left as
  whatever the previous frame put in that buffer — padding with stale bytes is
  how a driver leaks the contents of old packets to everyone on the segment.
- **Every multi-byte field crossing the wire is converted, and byte arrays never
  are.** ARP's hardware type, protocol type, opcode and addresses, and IPv4's
  lengths, identification, flags and addresses, go through `htons`/`htonl`
  outbound and `ntohs`/`ntohl` inbound; an address copied from a request into
  its reply stays in network order and is never swapped there and back; MAC addresses are
  `uint8_t[6]` and are copied as bytes, because an array has no endianness. The
  trap this avoids is comparing a raw wire field against a host-order constant,
  which does not fail loudly — it simply never matches, and looks exactly like a
  network that is not asking.
- **A transmit error path must end with the card and the driver agreeing where
  they are.** The RTL8139 transmits its descriptors strictly in order, so a
  descriptor that never comes back cannot be skipped — the card would still be
  waiting on it, and every later frame would be reported sent and never leave.
  A timeout flags the transmitter; the interrupt loop resets the card after the
  receive drain and before unmasking the line, and the driver's index goes back
  to 0 with the card's. It is the transmit-side twin of the receive rule that
  every exit from the drain moves `CAPR`, and the first version broke it the same
  way: by returning without moving anything.
- **A driver answers only for the address it owns.** An ARP request whose target
  is not the leased address gets no reply, and before a lease none does. Answering for addresses you do not hold is
  how a machine hijacks traffic on a segment, and the check that prevents it is
  one comparison, in host order on both sides.
- **A checksum is verified on the way in and computed with its field zeroed on
  the way out.** A received IPv4 header, ICMP message and UDP segment are summed
  as they arrived, checksum included — UDP's with its pseudo-header in front —
  and must come to zero; anything else is dropped without a word. The one
  exception is a UDP checksum of 0, which RFC 768 lets a sender use to mean
  "none" and which is accepted unverified (RFC 1122 4.1.3.4 allows it). Going
  out, UDP is always checksummed, and a checksum that comes out as 0 is sent as
  `0xFFFF` — the other zero of one's complement — so it is never mistaken for
  "none". An outgoing one is summed with its checksum field set to zero
  first, because the checksum is defined over the header as if that field were
  zero — computing it with the old value still there sums garbage in. The outbound side is re-checked by an implementation this project
  did not write: `tcpdump` reads every frame the guest sent during `make ping`,
  `make udp` and `make dhcp` and checks each checksum, and the UDP gate counts a missing
  checksum as a failure. The inbound side — that the guest refuses what it
  should — is tested by requests with deliberately wrong checksums, built with
  the tools' own RFC-derived reference.
- **A reply goes only to a single, ordinary unicast host.** The
  card is promiscuous, so frames for the whole segment arrive; only those sent
  to this card's MAC or to broadcast are handed above layer 2, and IPv4 then
  insists on this machine's own MAC and leased address — the one exception
  being UDP to the limited broadcast, which reaches only the DHCP client. A request from a broadcast, multicast, loopback, reserved source, anything in
  0.0.0.0/8, the subnet's own address, or this machine's own address is dropped:
  RFC 1122 requires it, and the reply would be a datagram no host may send.
  Fan-out on the segment is prevented by a different check — a source MAC that
  is a group address is refused — because the reply's Ethernet destination is
  the request's source MAC, never derived from the IP address. What no check
  here can stop is a request forged from another host's ordinary address, which
  draws a reply to that host, as it would from any machine that answers ping. Only an echo request is
  answered, never an echo reply, so two hosts cannot bounce ICMP replies forever.
  UDP echo has no such marker — a reply looks exactly like a request — so it
  refuses source ports below 1024, where the services that also answer
  everything live, and 2049, NFS, which trusts a client by its port. That stops
  echo-to-echo and echo-to-chargen loops, and stops the echo delivering chosen
  bytes to a service port or to NFS. It governs only where an echo goes: every
  echo still leaves from port 7, a privileged port, so it does not stop chosen
  bytes reaching some other service that trusts a privileged source port, nor a
  loop with a reflector on an ordinary high port (see Known limitations).
- **An alarm is a flag, and it comes out first.** An expired alarm is not a
  message in the task's mailbox, which any process could fill; it is a flag the
  tick sets, which `recv` turns into a `MSG_TIMER` ahead of interrupts and
  messages. Ahead of interrupts because it is one-shot and cannot starve a
  device, while a busy device re-raising its line would postpone the alarm for
  as long as traffic arrived. That order is not enough on its own: a task can
  be kept out of `recv` altogether — the network server, draining a ring a
  flood keeps full — so the DHCP client also keeps its own deadline, read
  against `sys_ticks`, and the receive loop asks before every frame whether it
  is due. Arming or cancelling clears an expiry not yet
  collected, so a cancelled alarm never arrives late. The message is zeroed
  before it is filled: it is built on the kernel stack and copied to ring 3.
- **A stranger's options are walked, never trusted.** Every byte of a DHCP
  message's options is proved to lie before the end of the message — a bound
  the IP and UDP layers established — before it is read; End is required;
  lengths are checked on the concatenated total; and nothing is used until the
  whole message has parsed and made sense. The same rule as every other parser
  here, applied to the first variable-length structure.
- **No address, no answers — before the lease, and after it.** Until a lease is
  bound the machine answers no ARP, no ping and no echo, not even at 0.0.0.0. A
  lease runs from when its REQUEST went out (RFC 2131 4.4.1), not from when the
  ACK arrived, so a late ACK cannot stretch it, and one already over is refused;
  when it runs out the machine stops answering at once (4.4.5), flood or not. A
  broadcast is accepted only as UDP and delivered only to the DHCP client.

## Known limitations

These are deliberate boundaries, not oversights:

- **Spurious IRQ 7/15 are not detected** via the in-service register. Not
  reachable while those lines stay masked.
- **A line is unmasked only when something owns it.** IRQ0 (timer) and IRQ1
  (keyboard) at boot, and any line a driver claims at run time with
  `sys_claim_irq` (the network card takes IRQ11, which also opens the cascade);
  every other line stays masked, and installing a handler is what opens one. A
  forwarded line — IRQ1, or a claimed one — is closed by the kernel each time it
  fires and reopened by the driver, and left masked for good if that driver dies.
- **No virtual memory allocator.** `map_page` maps a frame at an address you
  choose; there is nothing that picks addresses for you, and no unmapping.
- **The identity map starts at `0x0`**, so a null-pointer dereference does not
  fault.
- **New page tables must land in identity-mapped memory.** After `CR0.PG` is set,
  `map_page` can only write a page table it can address. It returns false rather
  than faulting, and the window grows to cover what the allocator has already
  used — but recursive mapping or a higher-half kernel is what removes the
  constraint properly.
- **The heap is a fixed 1 MiB and never grows.** `kmalloc` returns `NULL` once
  it is full; freed pages are not returned to the physical allocator.
- **First fit is O(n) in the number of blocks**, and there is no free list, so a
  heavily fragmented heap makes allocation slow before it makes it fail.
- **The initrd is flat and read-only.** No subdirectories, no path parsing, no
  writing, no creation or deletion. `readdir` returns a pointer to a single
  shared `dirent`, which is safe only while the kernel is single-threaded.
- **Only one filesystem can be mounted**, at `/`. There is no mount table.
- **IPC is single-slot, and waits with no timeout of its own.** A mailbox holds
  one unread message; a second `send` returns `IPC_ERR_FULL` rather than
  queueing, and `recv` blocks until something arrives — a task that wants to
  stop waiting sets an alarm first. Pids are assigned in creation order, never reused, and the
  demo hardcodes the receiver's.
- **The loader is eager and non-relocating.** The whole image is read into the
  heap and copied into frames up front: no demand paging, no `mmap`, and no
  page cache. Only `ET_EXEC` is accepted, so there is no relocation
  processing, no dynamic linking, and no interpreter.
- **A process's frames must be identity-mapped.** The kernel fills them by
  physical address, so an image large enough to push the allocator past the
  identity window fails to load rather than falling back to a temporary
  mapping.
- **No `exec`, no `fork`, and no arguments.** A process is created by the
  kernel at boot or by `sys_spawn` from the shell; it gets no `argv` or
  environment, and cannot replace its own image. `sys_exit` ends it and
  `sys_waitpid` reaps it; its pid is never reused.
- **A page directory created after a process exists would not reach it.**
  Address spaces share the kernel's page tables, so mappings added inside an
  existing table propagate everywhere, but a brand-new kernel directory entry
  would appear only in the kernel's own directory. Nothing creates one after
  boot today.
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
- **`pmm_free_block` cannot tell one holder's reference from another's.** It
  does know a reserved frame from an allocated one — reserved memory is pinned
  and a free of it is ignored — but it takes any caller's word that the
  reference being dropped is theirs, so a double free releases someone else's
  hold on a frame that is still mapped.
- **A shared segment has exactly two possible holders.** The creator names one
  peer at creation, and no third process may attach however it comes by the id —
  which is the fix for a real hole, since ids are sequential and were previously
  checked only for existence. Sharing among three or more would need a grantee
  set rather than a single peer.
- **A shared segment is at most sixteen pages and stays the size it was
  created.** There is no resize and no explicit detach — a mapping lasts until
  the process dies.
- **The shared-memory window is a bump allocator.** Addresses are handed out
  forward and never reclaimed, which is what makes an attach unable to land on
  something already mapped, at the cost of a process that attaches repeatedly
  eventually exhausting its window.
- **The registry is a fixed 16 entries** and any process may fill it. There is
  no quota, so a hostile program can deny shared memory to everyone else.
- **The mutex has no timeout, no owner and no recursion.** Locking twice from
  the same process deadlocks it against itself, a process that dies holding the
  lock leaves it held forever with no way to break it, and there is no priority
  inheritance because there are no priorities.
- **The keyboard driver knows shift and nothing else** — no caps lock, control,
  alt, or extended (`0xE0`) keys. Extended keys are recognised by their prefix
  and dropped whole, because their second byte reuses ordinary values: keypad
  Enter's is Enter's, and a review caught it printing a newline.
- **IOPL is all or nothing.** `sys_grant_io` hands the driver every I/O port
  on the machine and `cli`/`sti` with them, because that is what IOPL=3 means.
  Per-port granularity is what the TSS I/O permission bitmap is for, and it is
  not used here.
- **One driver per interrupt line, and nothing restarts one.** When the driver
  dies the kernel takes its line back at the moment of death — drains the
  controller, reopens the line — so every later keystroke is reported as
  dropped instead of wedging the 8042, but nobody decodes it. A supervisor
  that restarted the driver is what a real microkernel would have here.
- **A forwarded interrupt has no timeout.** The line stays masked until the
  driver calls `sys_unmask_irq`; a driver that never does leaves the device
  silent forever.
- **Focus is one process, and Tab is the only way to move it.** There is no
  focus request, no unsubscribe, and Tab itself can never reach an
  application. Rotation is subscription order.
- **A dead subscriber costs one keystroke.** Death is noticed only when the
  server next sends to it: the key that would have gone to the corpse is
  dropped, or the Tab that would have focused it moves on. There is no
  notification a process could act on sooner.
- **A slow application loses keys rather than queueing them.** Its mailbox
  holds one message; the server retries eight yields and then drops. There is
  no per-application queue.
- **Eight subscribers.** The table is fixed and any process may fill it.
- **Only the keystroke path is protected from flooding.** A subscription is an
  ordinary send, so a process flooding the input server can delay or deny an
  application's subscribe handshake; everything that is not the trusted sender
  still competes for one slot with no fairness.
- **Four terminals, no scrollback, no way to give one back.** A process gets a
  terminal the first time it is focused and keeps it for life; the fourth such
  process finds none free and shares the system console. Each terminal holds one screen.
- **The console holds a partial line until its newline.** A prompt printed
  without one by a process on the shared console does not appear until the
  line completes or reaches 80 characters. A process's own terminal is not
  buffered.
- **The kernel log is shown when the console server wakes**, at startup and
  after every message it handles. A line the kernel logs while nobody is
  printing or typing waits on the console until something is.
- **The console server dying silences the machine.** Every print becomes
  `IPC_ERR_NO_TASK` and is dropped; the kernel keeps logging to a buffer nobody
  reads. A panic still reaches the screen, over whatever was showing.
- **Output is not flood-proof.** Only the input server's switch requests hold
  the console's reserved slot; every process's characters contend for the
  ordinary one, so a process looping a send at the console delays or starves
  everyone else's output — the same single-slot weakness the keystroke path
  had before its reserved slot, and the reason a per-sender queue is the real
  fix.
- **Output is at most 31 characters per message.** A longer line can be split
  by another writer on the same terminal at a chunk boundary; the shared
  console is additionally line-buffered per sender, an owned terminal is not.
- **One load segment, reused.** The shell keeps a sixteen-page segment for the
  life of the machine and the VFS server keeps it attached; neither can give
  it back, because there is no detach. Programs larger than 64 KiB are refused.
- **Sixteen processes, ever at once.** The cap counts the five servers (VFS,
  keyboard, input, console, network) and the shell, so ten more programs can be
  alive at a time. Pids are never reused, so the count of pids ever handed out
  is unbounded; the count alive is not.
- **The retired boot demos are built but not started.** The client, the
  shared-memory pair and the mutex pair address each other by pids that only
  creation order at boot ever made true; `client.elf` is in the image and can
  be run from the shell, the others are not.
- **Reaping runs once per tick.** A dead process's memory is held for at most
  one timer period, ten milliseconds, whatever else is running.
- **A spawned program receives no arguments and no stdin, only an exit
  status.** The shell waits for each command and prints a line if it exited
  non-zero (a fault shows as a status at or above `0x100`). There is no
  backgrounding: a program that never exits blocks the shell until it does, and
  there is no way to interrupt or kill it.
- **Keys typed while a command runs are lost.** The shell blocks inside
  `sys_waitpid` for the whole life of the command, so it is not in `recv` to
  drain its mailbox; its one reserved input slot holds the first such key and
  the input server drops the rest. This is invisible for a program like
  `calc.elf` that exits in microseconds, and inherent to a synchronous wait
  with a single-slot mailbox — a request queue, or a shell that polled
  `waitpid` while still reading keys, is what would keep type-ahead.
- **A long-lived parent that never waits leaks its children's zombies** until
  it dies, at which point they become orphans and the tick reaper takes them.
  The shell waits on every child, so it never accumulates any; a hand-written
  parent that spawns and ignores its children is the case this describes.
- **One card, the first one found.** The PCI scan stops at the first RTL8139
  and a second would be ignored; multi-function and bridged devices past bus 7
  are not walked at all.
- **A literal zero-length frame cannot be tested honestly over a socket netdev.**
  Writing a 4-byte length prefix of zero does not put one empty frame on the
  wire; QEMU's socket backend turns it into a few hundred zero-length receive
  records. The driver rides it out — each is reported as too short, the ring
  advances, and normal frames resume immediately afterwards, with one frame lost
  inside the storm — but the multiplication is the backend's, not the card's, so
  this says nothing about what real hardware would do. Nonzero runts (8 bytes,
  what `tools/inject_frames.py` sends) behave exactly once each.
- **Transmission is four descriptors and no queue.** The card has exactly four
  and works through them strictly in order. When the next one has not come back
  the driver spins briefly and then drops the frame rather than blocking,
  because sending happens on the interrupt path. There is no software queue
  behind them and no retry.
- **A stuck transmitter costs a card reset.** Recovery from a descriptor that
  never completes is a full reset of the card, because only a reset puts the
  card's own transmit pointer back in step with the driver's. The reset also
  clears the receive ring, so frames waiting in it are lost, along with the
  frame whose send timed out. QEMU never gets here on its own; the path was
  exercised by desynchronising the card's pointer from the monitor.
- **ARP answers, but remembers nothing.** There is no ARP cache: a reply is
  built, sent and forgotten. Every reply goes to the MAC the request came from,
  and what the machine originates — DHCP, and one ARP request for its router —
  is broadcast, so nothing looks an address up. It answers for the single
  address it leased, for none before that, and never to a request whose sender
  MAC is a group address, which would make the answer a broadcast.
- **IPv4 routes nothing and reassembles nothing.** No fragment is reassembled — one is dropped,
  which RFC 1122 does not allow a host but which is honest about there being no
  reassembly buffer. Options are accepted and stepped over but not returned:
  RFC 1122 asks for Record Route and Timestamp to be updated in an echo reply and
  a source route to be reversed, and this one sends a plain 20-byte header
  instead. Nothing is routed, and the only datagrams originated rather than
  answered are the DHCP client's broadcasts.
- **ICMP is echo and nothing else.** Only echo requests are answered; every other
  type is logged and ignored, and no ICMP error — destination unreachable,
  parameter problem — is ever generated, so a malformed datagram is dropped
  without telling the sender why.
- **Nothing above IPv4 but ICMP echo, UDP echo and the DHCP client.** No TCP: a datagram for any
  other protocol passes every IPv4 check and is then reported as something
  nothing here speaks. The CRC the card appends is subtracted from the length
  and otherwise ignored. No other process can receive a frame or a datagram
  either — there is no socket interface, so the network server is the only thing
  on this system that knows the card exists, and the echo service lives inside
  it rather than in a program of its own.
- **With the console on screen, a flood slows the network to a crawl.** Every
  received frame is logged to the console, and while terminal 0 is showing,
  each line that scrolls it redraws the whole screen. The network server waits
  for that on every frame, so a host flooding at a few hundred frames a second
  pushes everyone else's ping replies out to about two seconds. With the shell
  in focus, as at boot, the same flood is answered in full. The logging is
  deliberate, for a system whose point is to be watched.
- **The host's own `ping` cannot reach the guest.** QEMU's user-mode network is
  NAT and forwards no ICMP inward, and a TAP device the host could route through
  needs root. `make ping` plays the host over a socket netdev instead; it proves
  the guest's replies, not the host's routing. UDP does reach it, but only
  through the port forward — `nc -u 127.0.0.1 7007`, never `10.0.2.15` — and
  `make qemu` now fails to start if that port is taken.
- **UDP has one service, and no way to say a port is closed.** A datagram to any
  port but 7 (and 68, the DHCP client's) is logged and dropped; RFC 1122 says a host SHOULD answer it with
  an ICMP port-unreachable, and none is generated. Every RFC 1122 MUST about
  the application interface is unmet, for want of an application to deliver
  to: IP options are not passed up with a datagram nor settable on one sent
  (4.1.3.2), ICMP errors are not passed to UDP (4.1.3.3), the destination
  address a datagram arrived on is not passed up and no application chooses a
  source address (4.1.3.5), and nothing can set TTL, TOS or options (4.1.4).
- **A loop through a high-port reflector is not prevented.** The source-port
  rule stops echo answering another well-known service, but a request forged to
  come from a second echo on an ordinary port — another machine's — would be
  answered, and answered again, for as long as both keep going. Under `make
  qemu` the forward itself can be that reflector: QEMU's user-mode stack sets
  `SO_REUSEADDR` on the forward's socket, so any local process can share
  127.0.0.1:7007, and one datagram it sends from there reaches the guest as
  10.0.2.2:7007, whose echo QEMU hands straight back to the guest — a loop of
  a few thousand frames a second that runs until QEMU exits, with the sender
  long gone. The usual remedy is a rate limit; the kernel alarm could now pace
  one, and none is built.
- **Every echo comes from a privileged port.** The echo always leaves from port
  7, carrying bytes the sender chose, to any unicast address a forged request
  names and any port but those below 1024 and 2049. A service on an ordinary
  port that trusts a client for sending from a privileged port can therefore
  still be handed chosen bytes. OpenBSD's inetd, whose rule this is, has the
  same exposure; the remedy is not to run echo where such services listen.
- **A lease is taken, and then taken again.** There is no RENEWING or REBINDING:
  when a lease runs out the address is dropped and, a second or two later, a
  new DISCOVER sent, so under QEMU there is a gap of one to two seconds every
  24 hours. A lease longer than about 248 days is timed in pieces, which no test
  can wait for. The tick is not quite 100 Hz — the PIT's whole-number divisor
  makes it 100.007 — so every alarm ends 69 parts per million early, and a
  day's lease about six seconds early: the safe side.
- **The first OFFER wins, and the address is not checked.** No ARP probe before
  using the address, so no DHCPDECLINE if another host holds it — not even the
  server itself: an offer of the server's own address is believed. No INIT-REBOOT
  to ask for the previous address; no DHCPRELEASE; and option overload (52) is
  refused, not parsed. `secs` is always 0.
- **A server that ignores the BROADCAST flag is not heard.** Before a lease the
  machine accepts no unicast at all, though RFC 2131 (section 2) says a client
  SHOULD accept a datagram sent to its hardware address before its IP address
  is configured; it relies on the flag it sets, which QEMU honours by always
  broadcasting.
- **The transaction id is unique, not secret.** It comes from the timestamp
  counter mixed with the MAC. Anyone on the segment sees it in the broadcast
  DISCOVER and REQUEST, so a host there can forge an OFFER, an ACK or a NAK the
  client will take — as it can for any DHCP client; what is checked is that it
  is at least for this client, this transaction, the state it is in, and
  self-consistent. A forger cannot make the client transmit faster than it
  would anyway: a lease already over is refused, and every restart — after a
  NAK, a give-up or a lease's end — waits a second or two first.

## License

Unlicensed — do as you like with it.
