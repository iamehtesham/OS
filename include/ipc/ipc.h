#ifndef IPC_IPC_H
#define IPC_IPC_H

#include <stdint.h>

#define IPC_PAYLOAD_SIZE 32u

/* A message is a fixed-size value, never a pointer to something else. That is
 * what lets the kernel copy it between tasks without ever taking a length from
 * ring 3, and what lets a mailbox be a single embedded slot in the TCB. */
typedef struct {
    uint32_t sender_pid;   /* written by the kernel, never trusted from a sender */
    uint32_t receiver_pid;
    uint32_t type;
    uint8_t  data[IPC_PAYLOAD_SIZE];
} ipc_message_t;

_Static_assert(sizeof(ipc_message_t) == 44, "ipc_message_t layout changed");

/* Results returned in EAX. Negative is failure. */
#define IPC_OK           0
#define IPC_ERR_NO_TASK (-1) /* target pid missing, dead, or never receives  */
#define IPC_ERR_FULL    (-2) /* receiver's mailbox still holds an unread one */
#define IPC_ERR_FAULT   (-3) /* a user pointer was not accessible            */

/* Kernel-side entry points. Both take the user's buffer as a raw address
 * because it is untrusted: nothing dereferences it until every page it spans
 * has been checked for the access about to be made. */
int32_t ipc_send(uint32_t target_pid, uint32_t user_msg);
int32_t ipc_recv(uint32_t user_msg);

#endif /* IPC_IPC_H */
