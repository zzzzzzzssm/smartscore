set(SMARTMUSIC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../vendor/smartmusic_screen/SmartMusic")

idf_component_register(
    SRCS
        "${SMARTMUSIC_ROOT}/components/creator_core/creator_quantizer.c"
        "${SMARTMUSIC_ROOT}/components/creator_core/creator_recorder.c"
        "${SMARTMUSIC_ROOT}/components/creator_core/creator_snapshot_mailbox.c"
    INCLUDE_DIRS "${SMARTMUSIC_ROOT}/components/creator_core/include"
    REQUIRES freertos midi_parser
)
