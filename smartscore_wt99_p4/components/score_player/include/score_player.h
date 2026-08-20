#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SCORE_PLAYER_STOPPED = 0,
    SCORE_PLAYER_PREPARING,
    SCORE_PLAYER_PLAYING,
    SCORE_PLAYER_PAUSED,
    SCORE_PLAYER_DRAINING,
    SCORE_PLAYER_ERROR,
} score_player_state_t;

typedef struct {
    score_player_state_t state;
    uint32_t generation;
    uint32_t position_ms;
    int first_note_index;
    int last_note_index;
    esp_err_t last_error;
    char message[64];
} score_player_status_t;

typedef void (*score_player_status_cb_t)(
    const score_player_status_t *status, void *user_data);

esp_err_t score_player_init(score_player_status_cb_t callback,
                            void *user_data);
esp_err_t score_player_start(const char *score_json,
                             uint16_t preview_bpm);
esp_err_t score_player_pause(void);
esp_err_t score_player_resume(void);
esp_err_t score_player_stop(void);
void score_player_get_status(score_player_status_t *status);

#ifdef __cplusplus
}
#endif

