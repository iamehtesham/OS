#include <stddef.h>
#include <stdint.h>

#include "cpu/isr.h"
#include "ipc/ipc.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/shm.h"
#include "sys/syscall.h"
#include "sys/uaccess.h"
#include "task/scheduler.h"
#include "task/task.h"
#include "utils/stdio.h"

static uint32_t call_count;

/* Prints a string whose pointer came from ring 3, and therefore cannot be
 * trusted to be terminated, mapped, or even to belong to the caller.
 *
 * Two things make it safe. The length is bounded, so an unterminated string
 * cannot walk the kernel off the end of memory. And every page is checked for
 * user accessibility BEFORE it is read, so a pointer aimed at kernel memory is
 * rejected rather than dereferenced -- without that check this call would be a
 * read primitive for arbitrary kernel data. The page is re-checked whenever the
 * string crosses a boundary, since the next page may not be mapped at all.
 *
 * Returns the number of characters printed, or 0 if the pointer was rejected. */
static uint32_t sys_print(uint32_t user_string)
{
    if (user_string == 0) {
        return 0;
    }

    const char *const text = (const char *)(uintptr_t)user_string;
    uint32_t          checked_page = ~0u;
    uint32_t          written      = 0;

    for (uint32_t i = 0; i < SYSCALL_MAX_STRING; i++) {
        const uint32_t address = user_string + i;
        const uint32_t page    = address & PAGE_FRAME_MASK;

        if (page != checked_page) {
            if (!paging_user_can_read(address)) {
                kprintf("\n[sys_print: rejected pointer %p]\n", (void *)(uintptr_t)address);
                return written;
            }

            checked_page = page;
        }

        if (text[i] == '\0') {
            break;
        }

        kprintf("%c", text[i]);
        written++;
    }

    return written;
}

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

/* Creates a shared page and maps it into the caller.
 *
 * The caller is handed an id and an address, and names the one other process
 * that may attach. Only the id is meaningful elsewhere: an address is a fact
 * about one address space, and a physical address would be one the receiver
 * could invent. The id alone is not a permission, though -- ids are dense and
 * guessable -- so the peer is recorded here and enforced at attach. */
static int32_t sys_shm_map(uint32_t user_out, uint32_t peer_pid)
{
    task_t *const self = task_current();

    if (self == NULL) {
        return -1;
    }

    /* Vet the destination before committing a registry slot and a frame to it.
     * Not what makes the registry hard to exhaust -- only a quota would, and
     * there is none -- but a call that is going to fail should not take
     * resources on its way out. */
    if (!user_range_writable(user_out, (uint32_t)sizeof(struct sys_shm))) {
        return -1;
    }

    uint32_t id;
    uint32_t frame;

    /* The registry's own reference is the one this creates. Everything below
     * that fails simply leaves the segment idle, and the next collection pass
     * retires it -- no unwind path of its own to get wrong. */
    if (!shm_create(self->pid, peer_pid, &id, &frame)) {
        return -1;
    }

    const uint32_t vaddr = task_reserve_shm_vaddr(self);

    if (vaddr == 0) {
        return -1;
    }

    /* One reference per mapping, taken before the mapping exists so a failure
     * to map is a failure to have referenced. */
    if (!pmm_ref_block((void *)(uintptr_t)frame)) {
        return -1;
    }

    /* USER and WRITABLE both, or the process page-faults on its own shared
     * page the moment it touches it. */
    if (!paging_map_in(self->cr3, frame, vaddr, PAGE_USER | PAGE_WRITABLE)) {
        pmm_free_block((void *)(uintptr_t)frame);
        return -1;
    }

    struct sys_shm out;

    out.id    = id;
    out.vaddr = vaddr;

    if (!copy_to_user(user_out, &out, (uint32_t)sizeof(out))) {
        /* The mapping stands, but the caller never learned where. It costs one
         * page until the process dies, which is when its address space is torn
         * down and the reference goes with it. */
        return -1;
    }

    return 0;
}

/* Maps an existing shared page into the caller at an address of the kernel's
 * choosing, and returns it. Zero means the id named nothing, the frame was at
 * its reference ceiling, or this process has exhausted its window. */
static uint32_t sys_shm_attach(uint32_t id)
{
    task_t *const self = task_current();

    if (self == NULL) {
        return 0;
    }

    uint32_t frame;

    /* Takes the reference for the mapping about to be made. */
    if (!shm_attach(id, self->pid, &frame)) {
        return 0;
    }

    const uint32_t vaddr = task_reserve_shm_vaddr(self);

    if (vaddr == 0) {
        pmm_free_block((void *)(uintptr_t)frame);
        return 0;
    }

    if (!paging_map_in(self->cr3, frame, vaddr, PAGE_USER | PAGE_WRITABLE)) {
        pmm_free_block((void *)(uintptr_t)frame);
        return 0;
    }

    return vaddr;
}

void syscall_handler(struct registers *regs)
{
    call_count++;

    switch (regs->eax) {
    case SYS_PRINT:
        /* Result goes back in eax: popa restores the frame on the way out, so
         * writing the field here is what the caller sees in its own EAX. */
        regs->eax = sys_print(regs->ebx);
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
        regs->eax = (uint32_t)sys_shm_map(regs->ebx, regs->ecx);
        break;

    case SYS_SHM_ATTACH:
        regs->eax = sys_shm_attach(regs->ebx);
        break;

    default:
        kprintf("\n[syscall: unknown call %u from cs=0x%x]\n", regs->eax, regs->cs);
        regs->eax = (uint32_t)-1;
        break;
    }
}

uint32_t syscall_count(void)
{
    return call_count;
}
