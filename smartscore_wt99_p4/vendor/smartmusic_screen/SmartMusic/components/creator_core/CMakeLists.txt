idf_component_register(
    SRCS "creator_quantizer.c"
         "creator_recorder.c"
         "creator_snapshot_mailbox.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos midi_parser
)
