#include <stddef.h>
#include <stdint.h>

#include "arch/vga.h"
#include "cpu/irq.h"
#include "cpu/isr.h"
#include "cpu/pic.h"
#include "ipc/ipc.h"
#include "mm/kheap.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/shm.h"
#include "sys/syscall.h"
#include "sys/uaccess.h"
#include "task/scheduler.h"
#include "task/task.h"
#include "utils/klog.h"
#include "utils/string.h"

static uint32_t call_count;

/* Maps a physical range the caller has been granted, and returns the virtual
 * address it landed at, or 0.
 *
 * This is the one call that can hand ring 3 a view of memory it did not
 * allocate, so it is gated three separate ways and each gate is sufficient on
 * its own.
 *
 * WHO. A bit in the task control block, which lives on a supervisor page. Ring
 * 3 can neither read nor write it, and no system call sets it -- only the
 * kernel does, when it creates a task it has decided to privilege.
 *
 * WHAT. The grant. A flag alone would make a server exactly as dangerous as
 * the kernel: one parser bug in a ring-3 program and it maps kernel text. So
 * the capability names a specific range, recorded by the kernel from the boot
 * loader's module list, and the request must lie wholly inside it. Nothing the
 * caller says contributes to the grant.
 *
 * WHERE. The kernel picks the virtual address. A caller that chose its own
 * could map over its own stack or its own code, which turns this into a way to
 * corrupt itself; and the destination is confined to a window inside the
 * caller's own address space.
 *
 * The mapping is read-only and user-accessible, into the caller's own page
 * directory alone. A module is read-only data and there is no reason to hand
 * out write access to it. */
static uint32_t sys_map_physical(uint32_t physical_addr, uint32_t length)
{
    task_t *const self = task_current();

    if (self == NULL || !self->may_map_physical || self->grant_length == 0) {
        return 0;
    }

    if (length == 0) {
        return 0;
    }

    /* Overflow before range, because the sum below is meaningless if it wraps. */
    if (physical_addr > UINT32_MAX - length) {
        return 0;
    }

    if (physical_addr < self->grant_base ||
        physical_addr + length > self->grant_base + self->grant_length) {
        return 0;
    }

    /* Mapping is page-granular, so the grant is honoured to the pages that
     * contain it. The slack either side belongs to the boot loader's own data,
     * never to the kernel. */
    const uint32_t grant_page = self->grant_base & PAGE_FRAME_MASK;
    const uint32_t first      = physical_addr & PAGE_FRAME_MASK;
    const uint32_t last       = (physical_addr + length - 1u) & PAGE_FRAME_MASK;

    if (last - grant_page > USER_MAP_LIMIT - USER_MAP_BASE - PAGE_SIZE) {
        return 0; /* would run past the window the kernel reserved for this */
    }

    uint32_t virt = USER_MAP_BASE + (first - grant_page);

    for (uint32_t page = first;; page += PAGE_SIZE, virt += PAGE_SIZE) {
        if (!paging_map_in(self->cr3, page, virt, PAGE_USER)) {
            return 0;
        }

        if (page >= last) {
            break;
        }
    }

    return USER_MAP_BASE + (physical_addr - grant_page);
}

/* Tells a task which physical range it may map. A server cannot guess where
 * the boot loader put its data, and must not learn it from another user
 * program -- so the kernel is the only thing that can say. A task with no
 * grant gets a zeroed struct and a failure, which is what every process except
 * a deliberately privileged server sees. */
static int32_t sys_grant_info(uint32_t user_out)
{
    task_t *const self = task_current();

    if (self == NULL) {
        return -1;
    }

    struct sys_grant grant;

    grant.base   = self->may_map_physical ? self->grant_base : 0u;
    grant.length = self->may_map_physical ? self->grant_length : 0u;

    if (!copy_to_user(user_out, &grant, (uint32_t)sizeof(grant))) {
        return -1;
    }

    return grant.length != 0u ? 0 : -1;
}

/* Maps `pages` frames of a segment contiguously at the next free address in
 * the caller's shared-memory window, taking one reference per frame. On
 * failure every reference taken here is given back and nothing is mapped:
 * the registry keeps its own references, so a segment left idle is retired by
 * the next collection pass rather than leaked. Returns the address or 0. */
static uint32_t map_segment(task_t *self, const uint32_t *frames, uint32_t pages)
{
    const uint32_t vaddr = task_reserve_shm_vaddr(self, pages);

    if (vaddr == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < pages; i++) {
        /* One reference per mapping, taken before the mapping exists so a
         * failure to map is a failure to have referenced. The rollback gives
         * back exactly the references THIS loop took: pages before i, plus i
         * itself only once its reference exists. USER and WRITABLE both, or
         * the process page-faults on its own shared page. */
        if (!pmm_ref_block((void *)(uintptr_t)frames[i])) {
            for (uint32_t j = 0; j < i; j++) {
                pmm_free_block((void *)(uintptr_t)frames[j]);
            }

            return 0;
        }

        if (!paging_map_in(self->cr3, frames[i], vaddr + i * PAGE_SIZE,
                           PAGE_USER | PAGE_WRITABLE)) {
            for (uint32_t j = 0; j <= i; j++) {
                pmm_free_block((void *)(uintptr_t)frames[j]);
            }

            return 0;
        }
    }

    return vaddr;
}

/* Creates a shared segment and maps it into the caller.
 *
 * The caller is handed an id, an address and a page count, and names the one
 * other process that may attach. Only the id is meaningful elsewhere: an
 * address is a fact about one address space, and a physical address would be
 * one the receiver could invent. The id alone is not a permission, though --
 * ids are dense and guessable -- so the peer is recorded here and enforced at
 * attach. */
static int32_t sys_shm_map(uint32_t user_out, uint32_t peer_pid, uint32_t pages)
{
    task_t *const self = task_current();

    if (self == NULL) {
        return -1;
    }

    if (pages == 0) {
        pages = 1; /* the original single-page call, from a caller that set no count */
    }

    if (pages > SHM_MAX_PAGES) {
        return -1;
    }

    /* Vet the destination before committing a registry slot and frames to it.
     * Not what makes the registry hard to exhaust -- only a quota would, and
     * there is none -- but a call that is going to fail should not take
     * resources on its way out. */
    if (!user_range_writable(user_out, (uint32_t)sizeof(struct sys_shm))) {
        return -1;
    }

    uint32_t id;
    uint32_t frames[SHM_MAX_PAGES];

    /* The registry's own references are the ones this creates. Everything
     * below that fails simply leaves the segment idle, and the next collection
     * pass retires it -- no unwind path of its own to get wrong. */
    if (!shm_create(self->pid, peer_pid, pages, &id, frames)) {
        return -1;
    }

    const uint32_t vaddr = map_segment(self, frames, pages);

    if (vaddr == 0) {
        return -1;
    }

    struct sys_shm out;

    out.id    = id;
    out.vaddr = vaddr;
    out.pages = pages;

    if (!copy_to_user(user_out, &out, (uint32_t)sizeof(out))) {
        /* The mapping stands, but the caller never learned where. It costs the
         * pages until the process dies, which is when its address space is
         * torn down and the references go with it. */
        return -1;
    }

    return 0;
}

/* Maps an existing shared segment into the caller at an address of the
 * kernel's choosing, and reports the address and the size. -1 means the id
 * named nothing the caller may have, a frame was at its reference ceiling, or
 * this process has exhausted its window. */
static int32_t sys_shm_attach(uint32_t user_req)
{
    task_t *const self = task_current();

    if (self == NULL) {
        return -1;
    }

    struct sys_shm req;

    if (!copy_from_user(&req, user_req, (uint32_t)sizeof(req))) {
        return -1;
    }

    uint32_t frames[SHM_MAX_PAGES];
    uint32_t pages;

    /* Takes the references for the mapping about to be made. */
    if (!shm_attach(req.id, self->pid, frames, &pages)) {
        return -1;
    }

    const uint32_t vaddr = task_reserve_shm_vaddr(self, pages);

    if (vaddr == 0) {
        for (uint32_t i = 0; i < pages; i++) {
            pmm_free_block((void *)(uintptr_t)frames[i]);
        }

        return -1;
    }

    for (uint32_t i = 0; i < pages; i++) {
        if (!paging_map_in(self->cr3, frames[i], vaddr + i * PAGE_SIZE,
                           PAGE_USER | PAGE_WRITABLE)) {
            /* Pages already mapped keep their references and go with the
             * address space; the rest are given back now. */
            for (uint32_t j = i; j < pages; j++) {
                pmm_free_block((void *)(uintptr_t)frames[j]);
            }

            return -1;
        }
    }

    req.vaddr = vaddr;
    req.pages = pages;

    return copy_to_user(user_req, &req, (uint32_t)sizeof(req)) ? 0 : -1;
}

/* Starts a new process from an ELF image a ring-3 process has placed in a
 * shared segment. The caller says which segment and how long the image is.
 *
 * The kernel trusts neither. It must be the segment's owner or peer -- an id
 * is a name, not a capability, and a process must not be able to have the
 * kernel read memory it could not map itself. The length must fit the
 * segment, and bounds the parse, so a short file's headers cannot reach stale
 * bytes left by the last program loaded there. And the image is COPIED into
 * kernel memory before a byte of it is parsed: the frames stay mapped and
 * writable in two other processes, and validating what somebody else can
 * still write is a time-of-check/time-of-use bug that a second core would
 * turn from theoretical into real. What is parsed is then process_spawn's
 * business, which is elf_load's: bad magic, wrong class or machine, a header
 * or segment past the end, an entry outside the image, each ends in -1 with
 * no task created. Returns the child's pid. */
static int32_t sys_spawn(uint32_t shm_id, uint32_t length)
{
    task_t *const self = task_current();

    if (self == NULL) {
        return -1;
    }

    uint32_t frames[SHM_MAX_PAGES];
    uint32_t pages;

    if (!shm_frames(shm_id, self->pid, frames, &pages)) {
        return -1;
    }

    if (length == 0 || length > pages * PAGE_SIZE) {
        return -1;
    }

    /* The kernel reads the frames by physical address. They were checked
     * reachable when the segment was created, but the check is cheap and the
     * consequence of being wrong is a kernel page fault. */
    for (uint32_t i = 0; i < pages; i++) {
        if (!paging_frame_is_reachable(frames[i])) {
            return -1;
        }
    }

    uint8_t *const image = kmalloc(length);

    if (image == NULL) {
        return -1;
    }

    for (uint32_t copied = 0; copied < length;) {
        const uint32_t page  = copied / PAGE_SIZE;
        const uint32_t chunk = length - copied < PAGE_SIZE ? length - copied : PAGE_SIZE;

        kmemcpy(image + copied, (const void *)(uintptr_t)frames[page], chunk);
        copied += chunk;
    }

    task_t *const child = process_spawn(image, length, "spawned image");

    kfree(image);

    if (child == NULL) {
        return -1;
    }

    child->parent_pid = self->pid;

    klog("Spawn: pid %u started pid %u\n", self->pid, child->pid);

    return (int32_t)child->pid;
}

static int32_t sys_parent_of(uint32_t pid)
{
    const task_t *const task = task_find(pid);

    return task != NULL ? (int32_t)task->parent_pid : 0;
}

/* The one way a process ends on purpose, and identical in effect to a fault:
 * task_zombify frees its user memory now and makes it a TASK_ZOMBIE, so the
 * scheduler stops giving it slices and a sender learns IPC_ERR_NO_TASK; a
 * driver's interrupt lines go back to the kernel; and its parent is woken if
 * it was waiting. The corpse -- page tables, directory, kernel stack, control
 * block -- is freed later by the parent's sys_waitpid, or by the tick reaper
 * if this task is an orphan. schedule() never returns to a zombie, so the loop
 * below is only a safety net. */
static void sys_exit(int32_t status)
{
    task_t *const self = task_current();

    if (self != NULL) {
        klog("Exit : pid %u exited with status %d\n", self->pid, status);
        /* Frees this task's user memory and wakes a waiting parent; the corpse
         * (tables, directory, kernel stack, control block) survives for the
         * parent's waitpid, or the reaper if this task is an orphan. */
        task_zombify(self, status);
        schedule();
    }

    /* schedule() never returns to a zombie; this is only a safety net. */
    for (;;) {
        __asm__ volatile ("sti; hlt");
    }
}

/* Waits for a child to exit and reaps it. The rendezvous that lets the shell
 * pause until a command finishes, and the only thing that frees a
 * non-orphaned process's tables, directory, stack and control block. */
static int32_t sys_waitpid(uint32_t target_pid, uint32_t user_status)
{
    task_t *const self = task_current();

    if (self == NULL) {
        return -1;
    }

    if (user_status != 0 &&
        !user_range_writable(user_status, (uint32_t)sizeof(int32_t))) {
        return -1;
    }

    for (;;) {
        task_t *const target = task_find(target_pid);

        /* Only a parent may reap its own child, and only one that still exists.
         * Pids are never reused, so a missing target is unambiguous: already
         * reaped, or never real. Both are the caller's error, not a wait. */
        if (target == NULL || target->parent_pid != self->pid) {
            return -1;
        }

        if (target->state == TASK_ZOMBIE) {
            /* Read the status before collecting, since collection frees the
             * control block it lives in. The pointer was checked writable
             * above, so the copy-out cannot fail here. */
            const int32_t status = target->exit_status;

            task_collect_zombie(target);

            if (user_status != 0) {
                (void)copy_to_user(user_status, &status, (uint32_t)sizeof(status));
            }

            return (int32_t)target_pid;
        }

        /* Alive: park until it exits. task_zombify wakes a parent whose
         * wait_target names the exiting child, so this returns exactly when
         * there is a corpse to collect -- including when the child dies of a
         * fault, which zombifies it the same way a clean exit does, so a shell
         * waiting on a crashing program is woken rather than left hanging. */
        self->wait_target = target_pid;
        self->state       = TASK_WAITING_CHILD;
        schedule();
    }
}

/* Raises the caller's IOPL to 3, by editing the EFLAGS image the CPU pushed on
 * entry to this system call. iret is the only instruction that can load IOPL,
 * and only at CPL 0, which is why this cannot be done from ring 3 with popf --
 * the CPU silently leaves the field alone -- and why it has to be done here, in
 * the frame the iret at the end of this call will pop.
 *
 * The edit is one OR. Bits 13:12 go to 1 and no other bit of the mask is set,
 * so IF at bit 9 -- the neighbour that matters -- and every arithmetic flag
 * come through untouched. Once set it travels with the task: every later
 * interrupt saves this task's EFLAGS to its own frame and every return restores
 * them, so the privilege persists for this process and reaches no other.
 *
 * It is a wide privilege. IOPL gates all 65536 ports and cli/sti, not one
 * device, so the gate is the same kind as sys_map_physical's WHO gate: a bit
 * the kernel set on a supervisor page, for the one task it started to be a
 * driver. A task without it gets -1 here and a #GP if it tries in/out anyway. */
static int32_t sys_grant_io(struct registers *regs)
{
    task_t *const self = task_current();

    if (self == NULL || !self->may_use_io) {
        return -1;
    }

    const uint32_t before = regs->eflags;

    regs->eflags |= EFLAGS_IOPL_MASK;

    klog("IOPL : pid %u eflags 0x%x -> 0x%x, IF %s\n", self->pid, before, regs->eflags,
            (regs->eflags & EFLAGS_IF) ? "kept" : "LOST");

    return 0;
}

/* Re-opens a line the kernel masked when it forwarded an interrupt. The driver
 * calls this when it has serviced the device, and not before: the line stays
 * closed for exactly as long as the device is unattended. Only the task the
 * kernel routes the line to may open it -- letting any process unmask any line
 * would hand the interrupt controller to whoever asked. Pids are never 0 in
 * ring 3, so an unowned line (owner 0) is refused by the same comparison. */
static int32_t sys_unmask_irq(uint32_t irq)
{
    task_t *const self = task_current();

    if (self == NULL || irq >= IRQ_COUNT || irq_owner((uint8_t)irq) != self->pid) {
        return -1;
    }

    pic_clear_mask((uint8_t)irq);

    return 0;
}

/* Names the sender whose messages to this task get the reserved slot. Nothing
 * to gate: a task deciding whom IT trusts changes only its own mailbox. A
 * message already waiting in the reserved slot from a previous trustee is
 * still delivered; only where the NEXT message lands changes. */
static int32_t sys_trust_sender(uint32_t pid)
{
    task_t *const self = task_current();

    if (self == NULL) {
        return -1;
    }

    self->trusted_sender_pid = pid;

    return 0;
}

/* Maps the VGA text buffer into the caller, writable, and returns where.
 *
 * The one physical page the console server needs and nothing else may have:
 * gated on a bit the kernel set when it started that server, the same shape as
 * may_map_physical and may_use_io. Unlike a module grant the mapping is
 * writable, because a screen is for writing to; and unlike a module it is
 * pinned memory below 1 MiB, so no reference count moves and the server dying
 * releases nothing that was ever allocated. The kernel keeps its own
 * supervisor mapping of the same frame for panic. */
static uint32_t sys_map_hw_buffer(void)
{
    task_t *const self = task_current();

    if (self == NULL || !self->may_map_vga) {
        return 0;
    }

    if (!paging_map_in(self->cr3, VGA_TEXT_BUFFER_PHYS, USER_HW_BASE,
                       PAGE_USER | PAGE_WRITABLE)) {
        return 0;
    }

    return USER_HW_BASE;
}

/* Copies a chunk of the kernel log out to ring 3. The request struct is
 * in/out: the caller says where it has read up to and how much room it has,
 * and learns where the bytes it got actually start -- which is later than it
 * asked if the log has wrapped past it -- and how many there are. Anyone may
 * read it; a log of pids and fault addresses is not a secret, and the console
 * server is not the only program that could usefully show it. */
static int32_t sys_klog_read(uint32_t user_req)
{
    struct sys_klog req;

    if (!copy_from_user(&req, user_req, (uint32_t)sizeof(req))) {
        return -1;
    }

    char chunk[256];

    if (req.length > sizeof(chunk)) {
        req.length = sizeof(chunk);
    }

    if (req.length != 0 && !user_range_writable(req.buffer, req.length)) {
        return -1;
    }

    uint32_t       offset = req.offset;
    const uint32_t count  = klog_read(&offset, chunk, req.length);

    if (count != 0 && !copy_to_user(req.buffer, chunk, count)) {
        return -1;
    }

    req.offset = offset;
    req.length = count;

    return copy_to_user(user_req, &req, (uint32_t)sizeof(req)) ? 0 : -1;
}

/* Allocates a run of contiguous physical frames, maps it into the caller, and
 * hands back both addresses. A driver needs the physical one, because the
 * NIC's DMA engine speaks physical, not virtual; sys_grant_info also reports a
 * physical address, but of memory the boot loader already placed, whereas this
 * allocates fresh contiguous memory for the purpose. Driver-only either way,
 * gated on the same bit as I/O.
 *
 * The frames are contiguous (a device DMAs across one buffer), zeroed (a
 * recycled frame carries another process's bytes, and this one is about to be
 * readable), and pinned once mapped: a device keeps the physical pointer we
 * hand it, and if these frames were reclaimed when the driver died the device
 * would DMA into whatever process got them next. Pinning costs the buffer for
 * good, which for a long-lived driver is the right trade. */
static uint32_t sys_alloc_dma(uint32_t pages, uint32_t user_phys_out)
{
    task_t *const self = task_current();

    if (self == NULL || !self->may_use_io) {
        return 0;
    }

    if (pages == 0 || pages > DMA_MAX_PAGES) {
        return 0;
    }

    if (!user_range_writable(user_phys_out, (uint32_t)sizeof(uint32_t))) {
        return 0;
    }

    void *const base = pmm_alloc_contiguous(pages);

    if (base == NULL) {
        return 0; /* no contiguous run that long is free */
    }

    const uint32_t phys = (uint32_t)(uintptr_t)base;

    /* The kernel zeroes the frames through the identity map, so they must be
     * reachable there -- as any frame the allocator hands out below the window
     * is. Free the run and fail otherwise, before anything is pinned. */
    for (uint32_t i = 0; i < pages; i++) {
        if (!paging_frame_is_reachable(phys + i * PAGE_SIZE)) {
            for (uint32_t j = 0; j < pages; j++) {
                pmm_free_block((void *)(uintptr_t)(phys + j * PAGE_SIZE));
            }

            return 0;
        }
    }

    kmemset(base, 0, pages * PAGE_SIZE);

    const uint32_t vaddr = task_reserve_dma_vaddr(self, pages);

    if (vaddr == 0) {
        for (uint32_t i = 0; i < pages; i++) {
            pmm_free_block((void *)(uintptr_t)(phys + i * PAGE_SIZE));
        }

        return 0;
    }

    for (uint32_t i = 0; i < pages; i++) {
        if (!paging_map_in(self->cr3, phys + i * PAGE_SIZE, vaddr + i * PAGE_SIZE,
                           PAGE_USER | PAGE_WRITABLE)) {
            /* Nothing is pinned yet, so the whole run is still freeable. The
             * caller gets 0 and never touches the window, so the partial
             * mappings left behind name frames that are about to be free --
             * unreachable in practice (the DMA window's tables are fresh), and
             * a buggy driver that ignored the 0 would fault on them, not the
             * kernel. */
            for (uint32_t j = 0; j < pages; j++) {
                pmm_free_block((void *)(uintptr_t)(phys + j * PAGE_SIZE));
            }

            return 0;
        }
    }

    /* Mapped and about to be handed out: pin so no teardown reclaims a frame
     * the device still points at. Last, and un-failable, so no cleanup path
     * has to undo it. */
    for (uint32_t i = 0; i < pages; i++) {
        pmm_pin((void *)(uintptr_t)(phys + i * PAGE_SIZE));
    }

    if (!copy_to_user(user_phys_out, &phys, (uint32_t)sizeof(phys))) {
        return 0; /* the mapping stands, pinned; the caller just never learned the address */
    }

    return vaddr;
}

/* Routes a line a driver discovered on the PCI bus to itself. The run-time
 * counterpart of the boot-time irq_route_to_task the keyboard uses: the kernel
 * cannot wire a PCI device's IRQ at boot because it does not know it until the
 * bus is scanned, so the driver reports it. Driver-only, and irq_claim refuses
 * the timer, the keyboard, and any line already owned. */
static int32_t sys_claim_irq(uint32_t irq)
{
    task_t *const self = task_current();

    if (self == NULL || !self->may_use_io) {
        return -1;
    }

    return irq_claim((uint8_t)irq, self->pid) ? 0 : -1;
}

void syscall_handler(struct registers *regs)
{
    call_count++;

    switch (regs->eax) {
    case SYS_ALLOC_DMA:
        regs->eax = sys_alloc_dma(regs->ebx, regs->ecx);
        break;

    case SYS_CLAIM_IRQ:
        regs->eax = (uint32_t)sys_claim_irq(regs->ebx);
        break;

    case SYS_MAP_HW_BUFFER:
        regs->eax = sys_map_hw_buffer();
        break;

    case SYS_KLOG_READ:
        regs->eax = (uint32_t)sys_klog_read(regs->ebx);
        break;

    case SYS_TRUST_SENDER:
        regs->eax = (uint32_t)sys_trust_sender(regs->ebx);
        break;

    case SYS_SEND:
        regs->eax = (uint32_t)ipc_send(regs->ebx, regs->ecx);
        break;

    case SYS_RECV:
        /* May not return for a long time: if nothing is waiting, the task
         * blocks and the scheduler switches away from inside this call. This
         * frame stays parked on the task's kernel stack until a sender wakes
         * it, at which point execution resumes here and unwinds to iret. */
        regs->eax = (uint32_t)ipc_recv(regs->ebx);
        break;

    case SYS_YIELD:
        schedule();
        regs->eax = 0;
        break;

    case SYS_MAP_PHYSICAL:
        regs->eax = sys_map_physical(regs->ebx, regs->ecx);
        break;

    case SYS_GRANT_INFO:
        regs->eax = (uint32_t)sys_grant_info(regs->ebx);
        break;

    case SYS_SHM_MAP:
        regs->eax = (uint32_t)sys_shm_map(regs->ebx, regs->ecx, regs->edx);
        break;

    case SYS_SHM_ATTACH:
        regs->eax = (uint32_t)sys_shm_attach(regs->ebx);
        break;

    case SYS_SPAWN:
        regs->eax = (uint32_t)sys_spawn(regs->ebx, regs->ecx);
        break;

    case SYS_PARENT_OF:
        regs->eax = (uint32_t)sys_parent_of(regs->ebx);
        break;

    case SYS_EXIT:
        sys_exit((int32_t)regs->ebx); /* does not return */
        break;

    case SYS_WAITPID:
        regs->eax = (uint32_t)sys_waitpid(regs->ebx, regs->ecx);
        break;

    case SYS_GRANT_IO:
        regs->eax = (uint32_t)sys_grant_io(regs);
        break;

    case SYS_UNMASK_IRQ:
        regs->eax = (uint32_t)sys_unmask_irq(regs->ebx);
        break;

    default:
        klog("\n[syscall: unknown call %u from cs=0x%x]\n", regs->eax, regs->cs);
        regs->eax = (uint32_t)-1;
        break;
    }
}

uint32_t syscall_count(void)
{
    return call_count;
}
