#ifndef USER_CONTEND_H
#define USER_CONTEND_H

#include <stdbool.h>
#include <stdint.h>

#include "ipc/mutex_proto.h"

/* Writes `mine` into the shared buffer MUTEX_ROUNDS times, one byte at a time,
 * reading it back each round. Returns how many rounds came back holding
 * something other than what was written -- which is how many times another
 * process wrote into the same buffer mid-update.
 *
 * `use_lock` exists so the identical code can be run with and without the
 * mutex: a synchronisation test that only ever runs the safe path proves
 * nothing, because a result of zero is what a broken lock and a working one
 * both look like when nothing is contending. */
uint32_t mutex_contend(struct mutex_shared *shared, const char *mine, bool use_lock);

void mutex_report(const char *who, uint32_t corruptions, bool use_lock);

#endif /* USER_CONTEND_H */
