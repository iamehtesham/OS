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
USER_PROGRAMS := vfs_server client shm_reader shm_writer mutex_b mutex_a

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

.PHONY: all build qemu check screenshot clean force-initrd

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

$(INITRD_IMG): force-initrd
	@mkdir -p $(@D)
	@python3 $(INITRD_TOOL) $(INITRD_DIR) $@.new > $@.log
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
# position (MODULE_VFS_SERVER, MODULE_CLIENT, MODULE_INITRD), so reordering it
# here silently starts the wrong program and grants the wrong memory.
COMMA := ,
EMPTY :=
SPACE := $(EMPTY) $(EMPTY)
MODULES     := $(BUILD_DIR)/vfs_server.elf $(BUILD_DIR)/client.elf \
               $(BUILD_DIR)/shm_reader.elf $(BUILD_DIR)/shm_writer.elf \
               $(BUILD_DIR)/mutex_b.elf $(BUILD_DIR)/mutex_a.elf $(INITRD_IMG)
MODULE_LIST := $(subst $(SPACE),$(COMMA),$(strip $(MODULES)))

# Boot the ELF image directly: QEMU implements the Multiboot loader itself, so
# no GRUB, ISO or disk image is involved. -initrd takes the whole comma
# separated list and presents it as the module array.
qemu: build
	$(QEMU) -kernel $(KERNEL) -initrd $(MODULE_LIST)

# Confirms the Multiboot 1 header is present, aligned and correctly checksummed.
check: $(KERNEL)
	@grub-file --is-x86-multiboot $(KERNEL) \
		&& echo "Multiboot 1 header: OK" \
		|| { echo "Multiboot 1 header: MISSING"; exit 1; }

# Headless boot that dumps the framebuffer to a PPM, for machines with no display.
screenshot: build
	@{ sleep 1; printf 'screendump $(BUILD_DIR)/screen.ppm\nquit\n'; } \
		| $(QEMU) -kernel $(KERNEL) -initrd $(MODULE_LIST) -display none -monitor stdio > /dev/null
	@echo "Wrote $(BUILD_DIR)/screen.ppm"

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS) $(USER_OBJS:.o=.d)
