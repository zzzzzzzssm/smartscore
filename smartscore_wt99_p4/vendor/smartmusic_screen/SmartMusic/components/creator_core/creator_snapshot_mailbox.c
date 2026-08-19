#include "creator_snapshot_mailbox.h"

midi_data_t *creator_snapshot_mailbox_replace(
    creator_snapshot_mailbox_t *mailbox, midi_data_t *snapshot)
{
    if (!mailbox) return snapshot;
    midi_data_t *superseded = mailbox->latest;
    mailbox->latest = snapshot;
    if (superseded) mailbox->replacement_count++;
    return superseded;
}

midi_data_t *creator_snapshot_mailbox_take(
    creator_snapshot_mailbox_t *mailbox)
{
    if (!mailbox) return NULL;
    midi_data_t *latest = mailbox->latest;
    mailbox->latest = NULL;
    return latest;
}
