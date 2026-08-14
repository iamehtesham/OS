Files in this directory are packed into build/initrd.img by
tools/make_initrd.py and handed to the kernel as a Multiboot module.

The kernel parses the image with src/fs/initrd.c and exposes it through
the VFS in src/fs/vfs.c, so every file here appears under /.
