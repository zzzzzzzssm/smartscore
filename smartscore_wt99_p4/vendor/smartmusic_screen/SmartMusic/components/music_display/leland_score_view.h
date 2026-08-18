#ifndef LELAND_SCORE_VIEW_H
#define LELAND_SCORE_VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "midi_parser.h"

typedef struct leland_score_view leland_score_view_t;

/* Screen-coordinate hit test used by touch interaction on a scrolled score. */
bool leland_score_view_hit_test_note(const leland_score_view_t *view,
                                     int screen_x, int screen_y,
                                     int *note_index, int *measure_number);
leland_score_view_t *leland_score_view_create(lv_obj_t *parent);
void leland_score_view_destroy(leland_score_view_t *view);
bool leland_score_view_set_midi(leland_score_view_t *view,
                                const midi_data_t *midi,
                                char *error, size_t error_size);
bool leland_score_view_set_note_color(leland_score_view_t *view,
                                      int note_index, uint32_t color_rgb);
/* Highlight the next score-following group without changing result colors. */
void leland_score_view_set_note_guide(leland_score_view_t *view,
                                      int first_note_index,
                                      int last_note_index);
int leland_score_view_page_count(const leland_score_view_t *view);
bool leland_score_view_show_page(leland_score_view_t *view, int page,
                                 bool animated);
void leland_score_view_set_hidden(leland_score_view_t *view, bool hidden);
int leland_score_view_get_last_note_on_page(const leland_score_view_t *view,
                                            int page);

#endif
