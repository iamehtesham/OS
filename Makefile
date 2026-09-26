# Build for the 32-bit freestanding kernel.
#
# There is no i686-elf cross-compiler here, so we use the host GCC purely as a
# 32-bit code generator and link with ld directly. Going through the gcc driver
# would apply its Linux specs (crt0, libc, PIE) and produce an unbootable image.

CC   := gcc
LD   := ld
QEMU := qemu-system-i386

SRC_DIR   := src
INC_DIR   := include
BUILD_DIR := build

# Snapshotted here, before -include drags the generated .d files into
# MAKEFILE_LIST. Every compile and link depends on this, so editing a flag below
# forces a full rebuild instead of silently doing nothing -- half-applying an
# ABI flag like -mregparm=3 would otherwise link objects with mismatched calling
# conventions into a kernel that still boots and merely misbehaves.
MAKEFILE_DEPS := $(MAKEFILE_LIST)

LINKER := linker.ld
KERNEL := $(BUILD_DIR)/kernel.bin

# The initrd is packed from a directory of ordinary files and handed to the
# kernel as a Multiboot module. Those filenames are arbitrary user data, and
# make cannot represent all of them in a prerequisite list -- a space splits
# into two bogus targets and a colon is a parse error that kills every target,
# clean included. So the image is never made to depend on the filenames. It is
# repacked on every build instead, and the result replaces the old image only
# when the bytes differ, which leaves the mtime alone and keeps downstream
# targets quiet. The packer is deterministic and takes milliseconds.
INITRD_DIR  := initrd
INITRD_TOOL := tools/make_initrd.py
INITRD_IMG  := $(BUILD_DIR)/initrd.img

# Ring-3 programs. These are NOT part of the kernel: each is a directory under
# src/user/ linked into its own ET_EXEC binary and handed to the kernel as a
# Multiboot module, which loads it into an address space of its own. They are
# pruned from the kernel's source discovery below -- linking one in would
# collide with the kernel's own _start and put user code in kernel pages.
#
# Everything in src/user/lib is linked into every program. There is no dynamic
# linker and each process has a private address space, so a shared library
# would have nothing to share; duplicating a few hundred bytes is the whole
# cost of not having one.
USER_DIR      := $(SRC_DIR)/user
USER_LIB_DIR  := $(USER_DIR)/lib
USER_LINKER   := user.ld
# A name may carry a directory (apps/app_a): the ELF lands at build/<name>.elf
# and the sources come from src/user/<name>/. Each program is one directory
# because everything in a directory is linked into one binary, and two _starts
# in one link is an error, so two applications are two directories.
USER_PROGRAMS := vfs_server kbd_server input_server vga_server net_server apps/shell apps/calc \
                 apps/app_a apps/app_b client shm_reader shm_writer mutex_b mutex_a

# Programs packed INTO the filesystem image, for the shell to spawn. The rest
# of USER_PROGRAMS are the boot modules (the servers and the shell) and the
# retired boot-time demos, which still build but are neither loaded at boot
# nor shipped: they address each other by pids that only creation order at
# boot ever made true.
INITRD_PROGRAMS := apps/calc apps/app_a apps/app_b client
INITRD_STAGE    := $(BUILD_DIR)/initrd_root

USER_LIB_SRCS := $(sort $(wildcard $(USER_LIB_DIR)/*.c)) $(sort $(wildcard $(USER_LIB_DIR)/*.S))
USER_LIB_OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,\
                   $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(USER_LIB_SRCS)))
USER_PROGS    := $(patsubst %,$(BUILD_DIR)/%.elf,$(USER_PROGRAMS))
USER_OBJS     := $(USER_LIB_OBJS)

# -fno-pie / -fno-pic: Ubuntu's GCC defaults to PIE, but linker.ld pins us to a
#   fixed load address at 1 MiB.
# -fno-stack-protector: the default -fstack-protector-strong emits calls to
#   __stack_chk_fail, which only libc provides.
# -MMD -MP: emit .d files so edits to a header rebuild every source including it.
CFLAGS := -m32 -std=gnu11 -O2 -Wall -Wextra \
          -ffreestanding \
          -fno-pie -fno-pic \
          -fno-stack-protector \
          -fno-asynchronous-unwind-tables \
          -MMD -MP \
          -I$(INC_DIR)

ASFLAGS := -m32 -ffreestanding -MMD -MP -I$(INC_DIR)

LDFLAGS := -m elf_i386 -T $(LINKER) -nostdlib --build-id=none -z noexecstack

# Ring-3 programs are built with the same freestanding rules -- they have no
# libc either -- but linked at their own base address by user.ld.
# -z max-page-size keeps segments 4 KiB aligned in the file so a page of a
# segment is a page of the file, which is what the loader assumes.
ULDFLAGS := -m elf_i386 -T $(USER_LINKER) -nostdlib --build-id=none \
            -z noexecstack -z max-page-size=0x1000

# Sources are discovered recursively so new subsystems under src/ need no edit
# here; sorted to keep the link order reproducible. The standalone ring-3
# programs are pruned: they are separate binaries, not kernel objects.
# BOTH discoveries prune src/user, not just the C one. Assembly is exactly what
# a libc-less user runtime reaches for -- a syscall trampoline, a hand-written
# _start -- and an unpruned .S would be assembled into the kernel while the
# program that needs it fails to resolve the symbol. A user _start would
# collide with boot.S's outright.
C_SRCS := $(sort $(shell find $(SRC_DIR) -path $(USER_DIR) -prune -o -name '*.c' -print))
S_SRCS := $(sort $(shell find $(SRC_DIR) -path $(USER_DIR) -prune -o -name '*.S' -print))
OBJS   := $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(S_SRCS)) \
          $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS))
DEPS   := $(OBJS:.o=.d)

.PHONY: all build qemu netdemo ping udp udp-host check screenshot clean force-initrd

# Without this make treats the ring-3 objects as intermediate files, deletes
# them after linking, and then rebuilds them on every single invocation because
# the prerequisite it just removed is missing.
.SECONDARY: $(USER_OBJS)

# Stated explicitly rather than relying on `all` being the first target: make
# picks the first non-special target it sees, so adding a helper rule above
# `all` would silently make bare `make` do that instead -- and exit 0 while
# building nothing, which reads as success to anything scripting it.
.DEFAULT_GOAL := all

all: build

build: $(KERNEL) $(INITRD_IMG) $(USER_PROGS)

# Always out of date, so the initrd recipe runs every build and decides for
# itself whether the image actually changed.
force-initrd:

# One rule per program, generated: each links every .c in its own directory
# plus the shared user runtime. A new program is a new directory and a name in
# USER_PROGRAMS, with no other Makefile edit.
define USER_PROGRAM_RULE
$(1)_SRCS := $$(sort $$(wildcard $$(USER_DIR)/$(1)/*.c)) \
             $$(sort $$(wildcard $$(USER_DIR)/$(1)/*.S)) $$(USER_LIB_SRCS)
$(1)_OBJS := $$(patsubst $$(SRC_DIR)/%.c,$$(BUILD_DIR)/%.o,\
               $$(patsubst $$(SRC_DIR)/%.S,$$(BUILD_DIR)/%.o,$$($(1)_SRCS)))
USER_OBJS += $$($(1)_OBJS)

$$(BUILD_DIR)/$(1).elf: $$($(1)_OBJS) $$(USER_LINKER) $$(MAKEFILE_DEPS)
	@mkdir -p $$(@D)
	$$(LD) $$(ULDFLAGS) -o $$@ $$($(1)_OBJS)
endef

$(foreach prog,$(USER_PROGRAMS),$(eval $(call USER_PROGRAM_RULE,$(prog))))

# The image is packed from a staging directory: the files under initrd/ plus
# the spawnable programs, which are real prerequisites so a rebuilt program
# repacks the image.
$(INITRD_IMG): force-initrd $(patsubst %,$(BUILD_DIR)/%.elf,$(INITRD_PROGRAMS))
	@mkdir -p $(@D) $(INITRD_STAGE)
	@rm -f $(INITRD_STAGE)/*
	@cp $(INITRD_DIR)/* $(INITRD_STAGE)/
	@for p in $(INITRD_PROGRAMS); do cp $(BUILD_DIR)/$$p.elf $(INITRD_STAGE)/; done
	@python3 $(INITRD_TOOL) $(INITRD_STAGE) $@.new > $@.log
	@if cmp -s $@.new $@ 2>/dev/null; then \
		rm -f $@.new; \
	else \
		mv -f $@.new $@; cat $@.log; \
	fi
	@rm -f $@.log

# The output directory is created inside each recipe rather than as a
# prerequisite: `build` is also a phony target here, and naming it as a
# dependency makes GNU make drop the edge as circular. $(@D) also picks up the
# per-subsystem subdirectories under build/.
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S $(MAKEFILE_DEPS)
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(MAKEFILE_DEPS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJS) $(LINKER) $(MAKEFILE_DEPS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# The kernel has no filesystem, so everything userland needs arrives as a
# Multiboot module. ORDER IS THE CONTRACT: src/kernel.c indexes this list by
# position (MODULE_VFS_SERVER ... MODULE_SHELL, MODULE_INITRD), so reordering
# it here silently starts the wrong program and grants the wrong memory or the
# wrong privilege. The four core servers come first so they hold pids 1-4, and
# the shell is fifth. Everything else the shell spawns from the image.
COMMA := ,
EMPTY :=
SPACE := $(EMPTY) $(EMPTY)
MODULES     := $(BUILD_DIR)/vfs_server.elf $(BUILD_DIR)/kbd_server.elf \
               $(BUILD_DIR)/input_server.elf $(BUILD_DIR)/vga_server.elf \
               $(BUILD_DIR)/apps/shell.elf $(BUILD_DIR)/net_server.elf $(INITRD_IMG)
MODULE_LIST := $(subst $(SPACE),$(COMMA),$(strip $(MODULES)))

# Boot the ELF image directly: QEMU implements the Multiboot loader itself, so
# no GRUB, ISO or disk image is involved. -initrd takes the whole comma
# separated list and presents it as the module array.
# The rtl8139 with user-mode networking gives the network driver a card to
# find on the PCI bus. User networking needs no host privilege and no tap
# device. Most inbound packets for the RX path -- ARP requests, pings, anything
# malformed -- are injected by the test harnesses, which swap this for a socket
# netdev they can write frames into.
NET_DEVICE := -netdev user,id=net0 -device rtl8139,netdev=net0

# `make qemu` adds one thing: a UDP port forward from the host to the guest's
# echo service, so that `nc -u 127.0.0.1 7007` on the host reaches 10.0.2.15:7
# (press Esc in the QEMU window to watch it arrive). Not `nc -u 10.0.2.15 7`:
# user-mode networking is NAT and the host routes 10.0.2.15 to its LAN router,
# and host port 7 needs privilege. Bound to 127.0.0.1 only, so nothing else on
# the LAN can reach the echo -- but any process on this host can, and QEMU's
# user-mode stack sets SO_REUSEADDR on the forward's socket, so a local process
# can even share its port: one datagram sent from 127.0.0.1:7007 itself makes
# QEMU hand every echo straight back to the guest, which echoes it again, until
# QEMU exits. QEMU will not start if the port is taken:
# make qemu UDP_ECHO_HOST_PORT=<other port>
UDP_ECHO_HOST_PORT ?= 7007
QEMU_NET_DEVICE := -netdev user,id=net0,hostfwd=udp:127.0.0.1:$(UDP_ECHO_HOST_PORT)-10.0.2.15:7 \
                   -device rtl8139,netdev=net0

qemu: build
	$(QEMU) -kernel $(KERNEL) -initrd $(MODULE_LIST) $(QEMU_NET_DEVICE)

# The same machine with a peer that talks back. `make qemu` exercises the
# transmit path on its own -- the driver ARPs the gateway at startup and QEMU's
# user-mode network answers -- and its port forward carries UDP to the echo
# service, but a gateway that only answers never sends an ARP request or a ping,
# so it cannot exercise those reply paths. Here the card is on a socket netdev:
# real ARP, an ICMP echo request, a UDP echo, IPv4, IPv6 and unrecognised frames
# are written into it, and the frames the guest transmits are read back and
# decoded, so the ARP, ping and UDP echo replies are checked as bytes on the
# wire rather than as lines on the guest's console.
# Overridable, because a second copy of this on the same machine would collide:
# make netdemo NET_DEMO_PORT=52140
NET_DEMO_PORT   ?= 52139
NET_DEMO_DEVICE := -netdev socket,id=net0,listen=127.0.0.1:$(NET_DEMO_PORT) \
                   -device rtl8139,netdev=net0
NET_DEMO_TOOL   := tools/inject_frames.py

netdemo: build
	@$(QEMU) -kernel $(KERNEL) -initrd $(MODULE_LIST) $(NET_DEMO_DEVICE) & \
	 qemu_pid=$$!; \
	 trap 'kill $$qemu_pid 2>/dev/null' INT TERM; \
	 sleep 1; \
	 if ! kill -0 $$qemu_pid 2>/dev/null; then \
	   echo "QEMU exited immediately: is port $(NET_DEMO_PORT) in use?"; \
	   echo "Retry with: make netdemo NET_DEMO_PORT=<other port>"; \
	   exit 1; \
	 fi; \
	 python3 $(NET_DEMO_TOOL) --port $(NET_DEMO_PORT) --count 8 --listen 4 || true; \
	 echo "Press Esc in the QEMU window to see the console, or close it to finish."; \
	 wait $$qemu_pid

# Ping the guest, and check every reply. Not with the host's own `ping`: under
# `make qemu` the card is on QEMU's user-mode network, which is NAT -- the host
# cannot reach 10.0.2.15, hostfwd carries TCP and UDP but never ICMP, and a TAP
# device needs root. So tools/ping.py plays the host: it builds real ICMP echo
# requests, delivers them to the card through a socket netdev, and checks each
# reply field by field, then throws malformed and misaddressed requests at it and
# proves after each one that the guest is still answering.
#
# QEMU records every frame the guest transmits into a pcap -- queue=rx on a
# netdev filter is the direction from the card to the backend, checked by
# comparing it with an unfiltered capture -- and afterwards tcpdump, a decoder
# this project did not write, re-checks every checksum in it. Nothing is keyed on
# the guest's MAC: a reply sent from the wrong source MAC is exactly the kind of
# defect that must still be counted, and an earlier version that filtered on the
# guest's MAC could not see one. Reading a capture needs no privilege. Headless,
# and its exit status is the verdict.
PING_PORT   ?= 52141
PING_PCAP   := $(BUILD_DIR)/ping.pcap
PING_DEVICE := -netdev socket,id=net0,listen=127.0.0.1:$(PING_PORT) \
               -device rtl8139,netdev=net0 \
               -object filter-dump,id=dump0,netdev=net0,queue=rx,file=$(PING_PCAP)

ping: build
	@rm -f $(PING_PCAP); \
	 $(QEMU) -kernel $(KERNEL) -initrd $(MODULE_LIST) $(PING_DEVICE) -display none -no-reboot & \
	 qemu_pid=$$!; \
	 trap 'kill $$qemu_pid 2>/dev/null' INT TERM; \
	 sleep 1; \
	 if ! kill -0 $$qemu_pid 2>/dev/null; then \
	   echo "QEMU exited immediately: is port $(PING_PORT) in use?"; \
	   echo "Retry with: make ping PING_PORT=<other port>"; \
	   exit 1; \
	 fi; \
	 python3 tools/ping.py --port $(PING_PORT); status=$$?; \
	 kill $$qemu_pid 2>/dev/null; wait $$qemu_pid 2>/dev/null; \
	 replies=$$(tcpdump -nn -vv -r $(PING_PCAP) icmp 2>/dev/null | grep -c "ICMP echo reply"); \
	 complaints=$$(tcpdump -nn -vv -r $(PING_PCAP) 2>/dev/null | grep -cE "bad cksum|wrong icmp cksum"); \
	 echo; \
	 echo "tcpdump, reading $(PING_PCAP): $$replies echo replies from the guest," \
	      "$$complaints checksum complaints"; \
	 if [ "$$replies" -eq 0 ] || [ "$$complaints" -ne 0 ]; then status=1; fi; \
	 exit $$status

# UDP to the echo service on port 7, and every reply checked. Like `make ping`:
# tools/udp_echo.py writes datagrams into the card over a socket netdev, checks
# each echo field by field, and throws at it what it must refuse -- each case
# built so that only the check it is named for can stop it. It also reads the
# guest's console out of VGA memory through the QEMU monitor, to see the log
# line the brief asks for and a datagram of control characters shown as dots.
#
# The tcpdump gate is stricter than ping's, because a UDP checksum of 0 is
# legal on the wire -- it means "none" -- and tcpdump reports it as "[no cksum]",
# neither ok nor bad. So: every UDP frame the guest sent must read "udp sum ok",
# there must be no complaint of any kind including "no cksum", and there must be
# at least as many as the tool accepted.
UDP_PORT    ?= 52143
UDP_PCAP    := $(BUILD_DIR)/udp.pcap
UDP_COUNT   := $(BUILD_DIR)/udp.count
UDP_MONITOR := $(BUILD_DIR)/udp-monitor.sock
UDP_DEVICE  := -netdev socket,id=net0,listen=127.0.0.1:$(UDP_PORT) \
               -device rtl8139,netdev=net0 \
               -object filter-dump,id=dump0,netdev=net0,queue=rx,file=$(UDP_PCAP)

# The gate, shared by both UDP targets: $(1) is the pcap, $(2) the count file.
define UDP_TCPDUMP_GATE
if ! command -v tcpdump >/dev/null 2>&1; then \
  echo "tcpdump not found: the checksum gate needs it to read $(1)"; status=1; fi; \
frames=$$(tcpdump -nn -r $(1) udp 2>/dev/null | wc -l); \
ok=$$(tcpdump -nn -vv -r $(1) udp 2>/dev/null | grep -c "udp sum ok"); \
complaints=$$(tcpdump -nn -vv -r $(1) 2>/dev/null \
              | grep -cE "bad cksum|bad udp cksum|no cksum|wrong icmp cksum"); \
accepted=$$(cat $(2) 2>/dev/null || echo 0); \
echo; \
echo "tcpdump, reading $(1): $$frames UDP frames from the guest, $$ok with a verified" \
     "checksum, $$complaints checksum complaints ($$accepted replies accepted by the tool)"; \
if [ "$$frames" -eq 0 ] || [ "$$ok" -ne "$$frames" ] || [ "$$complaints" -ne 0 ] \
   || [ "$$frames" -lt "$$accepted" ]; then status=1; fi
endef

udp: build
	@rm -f $(UDP_PCAP) $(UDP_COUNT) $(UDP_MONITOR); \
	 $(QEMU) -kernel $(KERNEL) -initrd $(MODULE_LIST) $(UDP_DEVICE) \
	   -monitor unix:$(UDP_MONITOR),server=on,wait=off -display none -no-reboot & \
	 qemu_pid=$$!; \
	 trap 'kill $$qemu_pid 2>/dev/null' INT TERM; \
	 sleep 1; \
	 if ! kill -0 $$qemu_pid 2>/dev/null; then \
	   echo "QEMU exited immediately: is port $(UDP_PORT) in use?"; \
	   echo "Retry with: make udp UDP_PORT=<other port>"; \
	   exit 1; \
	 fi; \
	 python3 tools/udp_echo.py --port $(UDP_PORT) --monitor $(UDP_MONITOR) \
	   --count-file $(UDP_COUNT); status=$$?; \
	 kill $$qemu_pid 2>/dev/null; wait $$qemu_pid 2>/dev/null; \
	 $(call UDP_TCPDUMP_GATE,$(UDP_PCAP),$(UDP_COUNT)); \
	 exit $$status

# Step 5 of the UDP brief, headless: the host's own netcat through QEMU's
# user-mode network and a port forward -- the same path as `make qemu` plus
# `nc -u 127.0.0.1 7007`, on a port of its own so the two can run side by side.
# tools/udp_host.py pipes a line through `nc -u`, reads the guest's console to
# see it arrive, and sweeps sizes up to 1472 bytes. This proves delivery through
# a UDP/IP stack this project did not write; it cannot prove the guest computes
# its checksum (see the tool's docstring), which is `make udp`'s job. Run on a
# finished tree only: on this netdev a wrong reply can leave the machine.
UDP_HOST_PORT    ?= 7008
UDP_HOST_PCAP    := $(BUILD_DIR)/udp-host.pcap
UDP_HOST_COUNT   := $(BUILD_DIR)/udp-host.count
UDP_HOST_MONITOR := $(BUILD_DIR)/udp-host-monitor.sock
UDP_HOST_DEVICE  := -netdev user,id=net0,hostfwd=udp:127.0.0.1:$(UDP_HOST_PORT)-10.0.2.15:7 \
                    -device rtl8139,netdev=net0 \
                    -object filter-dump,id=dump0,netdev=net0,queue=rx,file=$(UDP_HOST_PCAP)

udp-host: build
	@rm -f $(UDP_HOST_PCAP) $(UDP_HOST_COUNT) $(UDP_HOST_MONITOR); \
	 $(QEMU) -kernel $(KERNEL) -initrd $(MODULE_LIST) $(UDP_HOST_DEVICE) \
	   -monitor unix:$(UDP_HOST_MONITOR),server=on,wait=off -display none -no-reboot & \
	 qemu_pid=$$!; \
	 trap 'kill $$qemu_pid 2>/dev/null' INT TERM; \
	 sleep 1; \
	 if ! kill -0 $$qemu_pid 2>/dev/null; then \
	   echo "QEMU exited immediately: is 127.0.0.1:$(UDP_HOST_PORT)/udp in use?"; \
	   echo "Retry with: make udp-host UDP_HOST_PORT=<other port>"; \
	   exit 1; \
	 fi; \
	 python3 tools/udp_host.py --port $(UDP_HOST_PORT) --monitor $(UDP_HOST_MONITOR) \
	   --count-file $(UDP_HOST_COUNT); status=$$?; \
	 kill $$qemu_pid 2>/dev/null; wait $$qemu_pid 2>/dev/null; \
	 $(call UDP_TCPDUMP_GATE,$(UDP_HOST_PCAP),$(UDP_HOST_COUNT)); \
	 exit $$status

# Confirms the Multiboot 1 header is present, aligned and correctly checksummed.
check: $(KERNEL)
	@grub-file --is-x86-multiboot $(KERNEL) \
		&& echo "Multiboot 1 header: OK" \
		|| { echo "Multiboot 1 header: MISSING"; exit 1; }

# Headless boot that dumps the framebuffer to a PPM, for machines with no display.
screenshot: build
	@{ sleep 1; printf 'screendump $(BUILD_DIR)/screen.ppm\nquit\n'; } \
		| $(QEMU) -kernel $(KERNEL) -initrd $(MODULE_LIST) $(NET_DEVICE) \
			-display none -monitor stdio > /dev/null
	@echo "Wrote $(BUILD_DIR)/screen.ppm"

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS) $(USER_OBJS:.o=.d)
