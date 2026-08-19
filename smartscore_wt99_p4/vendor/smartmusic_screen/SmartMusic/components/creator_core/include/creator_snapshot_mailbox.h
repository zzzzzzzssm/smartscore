#ifndef CREATOR_SNAPSHOT_MAILBOX_H
#define CREATOR_SNAPSHOT_MAILBOX_H

#include <stdint.h>

#include "midi_parser.h"

typedef struct {
    midi_data_t *latest;
    uint32_t replacement_count;
} creator_snapshot_mailbox_t;

/* Caller supplies synchronization. The returned pointer was superseded and
 * remains owned by the caller. */
midi_data_t *creator_snapshot_mailbox_replace(
    creator_snapshot_mailbox_t *mailbox, midi_data_t *snapshot);
midi_data_t *creator_snapshot_mailbox_take(
    creator_snapshot_mailbox_t *mailbox);

#endif
