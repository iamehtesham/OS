#include <stddef.h>
#include <stdint.h>

#include "arch/io.h"
#include "arch/ps2.h"
#include "cpu/irq.h"
#include "cpu/isr.h"
#include "drivers/keyboard.h"
#include "task/scheduler.h"
#include "task/task.h"
#include "utils/klog.h"

static uint32_t dropped;

/* IRQ1. The kernel does not read the scancode here; it does not know what one
 * means any more. It hands the event to the driver that owns the line and gets
 * out of the way.
 *
 * Also invoked with regs == NULL by irq_release_owner when the driver has just
 * died, to drain whatever it left in the controller. */
static void keyboard_callback(struct registers *regs)
{
    (void)regs;

    task_t *const driver = irq_forward_to_owner(IRQ_KEYBOARD);

    if (driver != NULL) {
        /* The line is now masked and the scancode is still sitting in the
         * controller, waiting for the driver's inb. Run the driver now rather
         * than at its next turn: it is a keystroke, and a key that echoes a
         * tick late is a key that feels broken. The interrupted task keeps its
         * place in the ring. */
        schedule_to(driver);
        return;
    }

    /* No driver, or a dead one. Whatever is in the controller MUST still be
     * read: the 8042 holds the line asserted while its output buffer is full,
     * the 8259 is edge triggered, and a line that never falls never rises
     * again. Skipping this read would silence the keyboard until reboot.
     *
     * Gated on the status bit and bounded, for two reasons that are both about
     * this not being a plain interrupt handler any more. It also runs out of
     * band from irq_release_owner, where nothing guarantees a byte is waiting
     * -- a review caught it reporting the controller's stale 0xFA acknowledge
     * as a dropped keystroke when the driver died before touching the port.
     * And a driver that dies mid-service can leave more than one byte queued
     * behind the one it was reading. */
    for (int i = 0; i < 16 && (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL); i++) {
        const uint8_t scancode = inb(PS2_DATA_PORT);

        dropped++;
        klog("[kbd: no ring-3 driver owns IRQ1; scancode 0x%x dropped]\n", scancode);
    }
}

void keyboard_init(void)
{
    /* Drain anything the firmware left latched before the line is unmasked.
     * The 8042 holds IRQ1 asserted for as long as a byte sits in its output
     * buffer, and pic_remap's ICW1 has just reset the 8259's edge-sense
     * circuit -- a line that is already high never produces the fresh
     * low-to-high transition an interrupt needs. Nothing reads the data port
     * again until the ring-3 driver is told to, and it is only told on an
     * interrupt, so an undrained byte would deadlock the keyboard permanently:
     * no interrupt, so no read, so no edge, forever.
     *
     * The count is bounded so a wedged controller cannot hang the boot. */
    for (int i = 0; i < 16 && (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL); i++) {
        (void)inb(PS2_DATA_PORT);
    }

    irq_install_handler(IRQ_KEYBOARD, keyboard_callback);
}

uint32_t keyboard_dropped(void)
{
    return dropped;
}
