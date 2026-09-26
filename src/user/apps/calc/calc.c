/* The smallest possible program to be started by the shell: it announces
 * itself, does one sum so there is a result to print, and exits. Its output
 * lands on the shell's terminal, because the console asks the kernel who
 * spawned it. Its exit is what proves a spawned process can end: sys_exit
 * frees its user memory and makes it a zombie, and the shell -- which is
 * waiting on it in sys_waitpid -- reads its status and reaps the corpse. */

#include <stdint.h>

#include "user/ulib.h"

void _start(void)
{
    u_print("Calculator started!\n");
    u_printf("  2 + 2 = %u, and 0x%x is %u. That is all it knows.\n", 2u + 2u, 0x2Au, 0x2Au);
    u_exit(0);
}
