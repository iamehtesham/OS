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

# Sources are discovered recursively so new subsystems under src/ need no edit
# here; sorted to keep the link order reproducible.
C_SRCS := $(sort $(shell find $(SRC_DIR) -name '*.c'))
S_SRCS := $(sort $(shell find $(SRC_DIR) -name '*.S'))
OBJS   := $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(S_SRCS)) \
          $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS))
DEPS   := $(OBJS:.o=.d)

.PHONY: all build qemu check screenshot clean

all: build

build: $(KERNEL)

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

# Boot the ELF image directly: QEMU implements the Multiboot loader itself, so
# no GRUB, ISO or disk image is involved.
qemu: $(KERNEL)
	$(QEMU) -kernel $(KERNEL)

# Confirms the Multiboot 1 header is present, aligned and correctly checksummed.
check: $(KERNEL)
	@grub-file --is-x86-multiboot $(KERNEL) \
		&& echo "Multiboot 1 header: OK" \
		|| { echo "Multiboot 1 header: MISSING"; exit 1; }

# Headless boot that dumps the framebuffer to a PPM, for machines with no display.
screenshot: $(KERNEL)
	@{ sleep 1; printf 'screendump $(BUILD_DIR)/screen.ppm\nquit\n'; } \
		| $(QEMU) -kernel $(KERNEL) -display none -monitor stdio > /dev/null
	@echo "Wrote $(BUILD_DIR)/screen.ppm"

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
