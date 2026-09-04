#ifndef USER_IPC_DEMO_H
#define USER_IPC_DEMO_H

/* Two ring-3 programs that talk to each other through the kernel. Both live in
 * the .utext section so create_user_task can map them user-accessible.
 *
 * The sender addresses the receiver by pid, and pids are handed out in
 * creation order starting after the kernel task's 0 -- so the receiver has to
 * be created first, and this is the number it will get. */
#define IPC_DEMO_RECEIVER_PID 1u

/* Sends messages carrying an incrementing counter, one per loop. Also tries a
 * pid that does not exist, once, to show the error path. */
void ipc_demo_sender(void);

/* Blocks in sys_recv, then prints each counter it is handed via sys_print. */
void ipc_demo_receiver(void);

#endif /* USER_IPC_DEMO_H */
