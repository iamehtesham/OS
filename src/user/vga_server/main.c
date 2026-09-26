/* The console server: the screen, in ring 3.
 *
 * Nothing else has the VGA text buffer mapped -- the kernel keeps a mapping
 * for panic and uses it for nothing else -- so every character anyone prints
 * arrives here as a message and is placed by this process. It keeps one
 * backing buffer per terminal and copies the active one to the hardware.
 *
 * Terminal 0 is the system console. The kernel's log is shown there (read out
 * with SYS_KLOG_READ, since the kernel no longer prints), and so is the output
 * of every process that has no terminal of its own. A process gets a terminal
 * the first time the input server puts it on screen -- focus and screen move
 * together -- and keeps it. Escape shows the console without moving focus.
 *
 * Two kernel grants make this possible, and the kernel gave them to this
 * process because it started it as the console, not because it asked: the
 * buffer mapping, and I/O so it can move the hardware cursor through the CRT
 * controller's index/data ports. */

#include <stdbool.h>
#include <stdint.h>

#include "arch/io.h"
#include "arch/vga.h"
#include "ipc/input_proto.h"
#include "ipc/ipc.h"
#include "ipc/vga_proto.h"
#include "sys/syscall_abi.h"
#include "user/ulib.h"

#define CELLS (VGA_WIDTH * VGA_HEIGHT)

struct virtual_terminal {
    uint16_t cells[CELLS]; /* the backing buffer: 4000 bytes, one screen */
    uint32_t cursor_x;
    uint32_t cursor_y;
    bool     wrap_pending; /* the last column was just filled; see put_char */
    uint8_t  color;
    uint32_t owner_pid;    /* 0 for the console, which everyone may write on */
};

static struct virtual_terminal terminals[VGA_VT_COUNT];
static uint32_t                active; /* index of the terminal on the hardware */

/* The mapped text buffer. volatile: stores to it are the point. */
static volatile uint16_t *screen;

/* One colour per terminal, so a glance says which one is showing. */
static const uint8_t terminal_colors[VGA_VT_COUNT] = {
    (uint8_t)(VGA_COLOR_LIGHT_GREY | (VGA_COLOR_BLACK << 4)),
    (uint8_t)(VGA_COLOR_LIGHT_GREEN | (VGA_COLOR_BLACK << 4)),
    (uint8_t)(VGA_COLOR_LIGHT_CYAN | (VGA_COLOR_BLACK << 4)),
    (uint8_t)(VGA_COLOR_LIGHT_MAGENTA | (VGA_COLOR_BLACK << 4)),
};

/* Console line buffers, one per sender, pid 0 being the kernel log. Output to
 * the console is held here until a newline, so that two processes printing at
 * once produce two lines rather than one line of alternating characters. A
 * process's own terminal needs no such thing: it has one writer. */
#define LINE_SLOTS 16

struct pending_line {
    bool     used;
    uint32_t pid;
    uint32_t length;
    char     text[VGA_WIDTH];
};

static struct pending_line lines[LINE_SLOTS];

static uint32_t klog_offset; /* how much of the kernel log has been shown */
static uint32_t chars_placed;
static uint32_t switches;

/* ---- the hardware ------------------------------------------------------- */

/* The CRT controller keeps the cursor as one 16-bit cell index -- row * 80 +
 * column, NOT a byte offset -- split across two 8-bit registers reached through
 * the index/data port pair. */
static void hw_cursor(uint32_t x, uint32_t y)
{
    const uint16_t position = (uint16_t)(y * VGA_WIDTH + x);

    outb(VGA_CRTC_INDEX, VGA_CRTC_CURSOR_LOC_LOW);
    outb(VGA_CRTC_DATA, (uint8_t)(position & 0xFFu));
    outb(VGA_CRTC_INDEX, VGA_CRTC_CURSOR_LOC_HIGH);
    outb(VGA_CRTC_DATA, (uint8_t)((position >> 8) & 0xFFu));
}

/* Puts a whole terminal on the hardware: the memcpy, spelled out as 16-bit
 * stores so every one is a real write into video memory. */
static void hw_show(const struct virtual_terminal *terminal)
{
    for (uint32_t i = 0; i < CELLS; i++) {
        screen[i] = terminal->cells[i];
    }

    hw_cursor(terminal->cursor_x, terminal->cursor_y);
}

/* ---- a terminal --------------------------------------------------------- */

static void blank_row(struct virtual_terminal *terminal, uint32_t row)
{
    for (uint32_t col = 0; col < VGA_WIDTH; col++) {
        terminal->cells[row * VGA_WIDTH + col] = vga_entry(' ', terminal->color);
    }
}

static void scroll(struct virtual_terminal *terminal)
{
    for (uint32_t i = VGA_WIDTH; i < CELLS; i++) {
        terminal->cells[i - VGA_WIDTH] = terminal->cells[i];
    }

    blank_row(terminal, VGA_HEIGHT - 1);
}

/* Returns true if the terminal scrolled, which is when the whole thing has to
 * go back to the hardware rather than one cell. */
static bool newline(struct virtual_terminal *terminal)
{
    terminal->cursor_x     = 0;
    terminal->wrap_pending = false;

    if (terminal->cursor_y + 1 == VGA_HEIGHT) {
        scroll(terminal);
        return true;
    }

    terminal->cursor_y++;
    return false;
}

/* Places one character on a terminal and, if that terminal is the one on the
 * hardware, on the hardware too: the one cell that changed, or the whole
 * screen after a scroll, and the cursor either way. The control characters and
 * the deferred wrap are the kernel's old console rules, moved here whole. */
static void put_char(uint32_t index, char c)
{
    struct virtual_terminal *const terminal = &terminals[index];
    const bool                     showing  = (index == active);
    bool                           scrolled = false;
    int32_t                        cell     = -1;

    switch (c) {
    case '\n':
        scrolled = newline(terminal);
        break;

    case '\r':
        terminal->cursor_x     = 0;
        terminal->wrap_pending = false;
        break;

    case '\b':
        /* Step the insertion point back one cell and blank it. A pending wrap
         * means the point is parked past the last column, so clearing the flag
         * is itself the step back. */
        if (terminal->wrap_pending) {
            terminal->wrap_pending = false;
        } else if (terminal->cursor_x > 0) {
            terminal->cursor_x--;
        } else if (terminal->cursor_y > 0) {
            terminal->cursor_y--;
            terminal->cursor_x = VGA_WIDTH - 1;
        } else {
            return; /* top-left corner: nothing to back into */
        }

        cell = (int32_t)(terminal->cursor_y * VGA_WIDTH + terminal->cursor_x);
        terminal->cells[cell] = vga_entry(' ', terminal->color);
        break;

    case '\t':
        /* Spaces to the next 8-column stop, stopping at the right edge so a
         * tab never spills onto the next row. Each space mirrors itself. */
        do {
            put_char(index, ' ');
        } while (!terminal->wrap_pending && terminal->cursor_x % 8 != 0);

        return;

    default:
        if (terminal->wrap_pending) {
            scrolled = newline(terminal);
        }

        cell = (int32_t)(terminal->cursor_y * VGA_WIDTH + terminal->cursor_x);
        terminal->cells[cell] = vga_entry((unsigned char)c, terminal->color);

        /* Park on the last column rather than advancing off the row; the next
         * printable character triggers the deferred wrap above. */
        if (terminal->cursor_x + 1 == VGA_WIDTH) {
            terminal->wrap_pending = true;
        } else {
            terminal->cursor_x++;
        }

        break;
    }

    chars_placed++;

    if (!showing) {
        return;
    }

    if (scrolled) {
        hw_show(terminal);
        return;
    }

    if (cell >= 0) {
        screen[cell] = terminal->cells[cell];
    }

    hw_cursor(terminal->cursor_x, terminal->cursor_y);
}

static void put_string(uint32_t index, const char *text)
{
    for (; *text != '\0'; text++) {
        put_char(index, *text);
    }
}

/* ---- who prints where --------------------------------------------------- */

static uint32_t own_terminal_of(uint32_t pid)
{
    for (uint32_t i = 1; i < VGA_VT_COUNT; i++) {
        if (terminals[i].owner_pid == pid) {
            return i;
        }
    }

    return VGA_VT_CONSOLE;
}

/* Parentage, asked of the kernel once per pid and remembered. Pids are never
 * reused, so an answer never goes stale; a dead parent's pid still names the
 * terminal it was given. */
#define PARENT_SLOTS 32u

static uint32_t parent_pids[PARENT_SLOTS];
static uint32_t parent_of_pid[PARENT_SLOTS];
static uint32_t parent_count;

static uint32_t parent_of(uint32_t pid)
{
    for (uint32_t i = 0; i < parent_count; i++) {
        if (parent_pids[i] == pid) {
            return parent_of_pid[i];
        }
    }

    const uint32_t parent = u_parent_of(pid);

    if (parent_count < PARENT_SLOTS) {
        parent_pids[parent_count]   = pid;
        parent_of_pid[parent_count] = parent;
        parent_count++;
    }

    return parent;
}

/* The terminal a sender's characters land on: its own if it has one; else its
 * nearest ancestor's, so a program started from the shell prints where the
 * command was typed; else the console. The walk is bounded, since a chain of
 * spawns could in principle be long and a cycle cannot exist but should not
 * be trusted not to. */
static uint32_t terminal_of(uint32_t pid)
{
    uint32_t own = own_terminal_of(pid);

    if (own != VGA_VT_CONSOLE) {
        return own;
    }

    uint32_t ancestor = parent_of(pid);

    for (uint32_t depth = 0; depth < 4 && ancestor != 0; depth++) {
        own = own_terminal_of(ancestor);

        if (own != VGA_VT_CONSOLE) {
            return own;
        }

        ancestor = parent_of(ancestor);
    }

    return VGA_VT_CONSOLE;
}

/* The terminal to show for a pid, giving it one if it has none and one is
 * free. The console for pid 0, and for a process when nothing is left. */
static uint32_t terminal_for_switch(uint32_t pid)
{
    if (pid == 0) {
        return VGA_VT_CONSOLE;
    }

    const uint32_t own = own_terminal_of(pid);

    if (own != VGA_VT_CONSOLE) {
        return own;
    }

    for (uint32_t i = 1; i < VGA_VT_COUNT; i++) {
        if (terminals[i].owner_pid == 0) {
            terminals[i].owner_pid = pid;
            return i;
        }
    }

    return VGA_VT_CONSOLE;
}

/* Blanks the terminal the sender writes on and homes its cursor. The sender
 * names nothing, so it can clear only what it could already scribble on. */
static void handle_clear(uint32_t pid)
{
    const uint32_t                 index    = terminal_of(pid);
    struct virtual_terminal *const terminal = &terminals[index];

    for (uint32_t row = 0; row < VGA_HEIGHT; row++) {
        blank_row(terminal, row);
    }

    terminal->cursor_x     = 0;
    terminal->cursor_y     = 0;
    terminal->wrap_pending = false;

    if (index == active) {
        hw_show(terminal);
    }
}

static struct pending_line *line_of(uint32_t pid)
{
    struct pending_line *spare = 0;

    for (uint32_t i = 0; i < LINE_SLOTS; i++) {
        if (lines[i].used && lines[i].pid == pid) {
            return &lines[i];
        }

        if (!lines[i].used && spare == 0) {
            spare = &lines[i];
        }
    }

    if (spare != 0) {
        spare->used   = true;
        spare->pid    = pid;
        spare->length = 0;
    }

    return spare; /* null when sixteen senders already hold a line each */
}

/* A character for the console: buffered per sender, released as a line. */
static void console_char(uint32_t pid, char c)
{
    struct pending_line *const line = line_of(pid);

    if (line == 0) {
        put_char(VGA_VT_CONSOLE, c); /* no slot: unbuffered is better than lost */
        return;
    }

    if (c != '\n') {
        line->text[line->length++] = c;

        if (line->length < VGA_WIDTH) {
            return;
        }
        /* A full row is released as it stands; the row will wrap where it
         * would have anyway. */
    }

    for (uint32_t i = 0; i < line->length; i++) {
        put_char(VGA_VT_CONSOLE, line->text[i]);
    }

    if (c == '\n') {
        put_char(VGA_VT_CONSOLE, '\n');
    }

    /* The slot is only needed while a line is half-written. Giving it back
     * here means the table is bounded by senders with a line IN PROGRESS, not
     * by senders that ever printed -- so it cannot be used up over the life
     * of the machine, however many processes come and go. */
    line->length = 0;
    line->used   = false;
}

static void handle_print(uint32_t pid, char c)
{
    const uint32_t index = terminal_of(pid);

    if (index == VGA_VT_CONSOLE) {
        console_char(pid, c);
    } else {
        put_char(index, c);
    }
}

static void handle_switch(uint32_t pid)
{
    const uint32_t index = terminal_for_switch(pid);

    if (index == active) {
        return;
    }

    active = index;
    switches++;
    hw_show(&terminals[active]);
}

/* Everything the kernel has logged since last time goes on the console, as
 * lines from sender 0. Called at startup and after every message: the kernel
 * cannot tell this server anything has happened, so the server looks. */
static void show_kernel_log(void)
{
    static char chunk[256];

    for (;;) {
        struct sys_klog request;

        request.offset = klog_offset;
        request.buffer = (uint32_t)(uintptr_t)chunk;
        request.length = sizeof(chunk);

        if (!u_klog_read(&request) || request.length == 0) {
            return;
        }

        for (uint32_t i = 0; i < request.length; i++) {
            console_char(0, chunk[i]);
        }

        klog_offset = request.offset + request.length;
    }
}

void _start(void)
{
    screen = (volatile uint16_t *)(uintptr_t)u_map_hw_buffer();

    /* With no buffer there is no way to say so, either: this is the process
     * that would have shown the message. It parks, and the kernel's log line
     * about the missing grant will be there for whoever maps the buffer next. */
    if (screen == 0) {
        for (;;) {
            ipc_message_t never;
            u_recv(&never);
        }
    }

    /* For the cursor ports. Refusal is survivable: the text still shows, the
     * cursor simply stays wherever the kernel left it. */
    (void)u_grant_io();

    for (uint32_t i = 0; i < VGA_VT_COUNT; i++) {
        terminals[i].color = terminal_colors[i];

        for (uint32_t row = 0; row < VGA_HEIGHT; row++) {
            blank_row(&terminals[i], row);
        }
    }

    /* The input server's switch requests get the reserved slot: a screen
     * switch must not queue behind ten processes' worth of characters. */
    (void)u_trust_sender(INPUT_SERVER_PID);

    active = VGA_VT_CONSOLE;
    hw_show(&terminals[active]);

    put_string(VGA_VT_CONSOLE,
               "  [vga] console server up: 4 terminals; Tab shows the focused process's, "
               "Esc this one\n");
    show_kernel_log();

    ipc_message_t msg;

    for (;;) {
        if (u_recv(&msg) != IPC_OK) {
            continue;
        }

        switch (msg.type) {
        case MSG_PRINT_CHAR:
            /* The kernel stamped the sender, which is what picks the
             * terminal: nothing in the payload says where to write. */
            handle_print(msg.sender_pid, (char)msg.data[0]);
            break;

        case MSG_PRINT_STR: {
            /* Placed whole, before the next message is looked at: that is
             * the guarantee that makes a chunk atomic on a shared terminal.
             * The count is bounded by the payload, whatever it claims. */
            uint32_t count = msg.data[0];

            if (count > VGA_PRINT_CHUNK) {
                count = VGA_PRINT_CHUNK;
            }

            for (uint32_t i = 0; i < count; i++) {
                handle_print(msg.sender_pid, (char)msg.data[1 + i]);
            }

            break;
        }

        case MSG_SWITCH_VT:
            /* Only the focus manager decides what is on screen. Anyone else
             * asking is ignored, or an application could hide another. */
            if (msg.sender_pid == INPUT_SERVER_PID) {
                handle_switch(u_load32(msg.data));
            }

            break;

        case MSG_CLEAR_VT:
            handle_clear(msg.sender_pid);
            break;

        default:
            break;
        }

        show_kernel_log();
    }
}
