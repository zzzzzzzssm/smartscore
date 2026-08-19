#ifndef MUSIC_LAYOUT_H
#define MUSIC_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "music_model.h"
#include "smufl_glyphs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_SCENE_MAX_ITEMS 4096
#define MUSIC_SCENE_MAX_SYSTEMS MUSIC_MAX_MEASURES
#define MUSIC_SCENE_COLOR_DEFAULT UINT32_C(0xFFFFFFFF)

typedef enum {
    MUSIC_SCENE_GLYPH = 0,
    MUSIC_SCENE_LINE,
    MUSIC_SCENE_BEZIER,
} music_scene_item_kind_t;

typedef struct {
    music_scene_item_kind_t kind;
    /* Index into music_score_t::events. A negative value marks shared
     * engraving geometry (staff lines, clefs, barlines, braces, etc.). */
    int16_t source_event_index;
    /* RGB888 override used by interactive feedback. DEFAULT inherits the
     * view's normal notation color. */
    uint32_t color_rgb;
    union {
        struct {
            smufl_glyph_id_t glyph;
            music_sp_t x;
            music_sp_t y;
            music_sp_t scale;
        } glyph;
        struct {
            music_sp_t x1;
            music_sp_t y1;
            music_sp_t x2;
            music_sp_t y2;
            music_sp_t thickness;
        } line;
        struct {
            music_sp_t x1;
            music_sp_t y1;
            music_sp_t cx1;
            music_sp_t cy1;
            music_sp_t cx2;
            music_sp_t cy2;
            music_sp_t x2;
            music_sp_t y2;
            music_sp_t thickness;
        } bezier;
    } data;
} music_scene_item_t;

typedef struct {
    music_sp_t page_width;
    music_sp_t left_margin;
    music_sp_t right_margin;
    music_sp_t top_margin;
    music_sp_t system_gap;
    music_sp_t staff_gap;
    music_sp_t staff_line_thickness;
    music_sp_t stem_thickness;
    music_sp_t beam_thickness;
    music_sp_t ledger_line_thickness;
    music_sp_t ledger_line_extension;
    music_sp_t minimum_note_spacing;
    uint8_t max_measures_per_system; /* 0 lets content width decide. */
} music_layout_config_t;

typedef struct {
    uint16_t measure_start;
    uint16_t measure_end; /* Exclusive. */
    uint16_t item_start;
    uint16_t item_count;
    music_sp_t top;
    music_sp_t bottom;
} music_scene_system_t;

typedef struct {
    music_scene_item_t items[MUSIC_SCENE_MAX_ITEMS];
    music_scene_system_t systems[MUSIC_SCENE_MAX_SYSTEMS];
    uint16_t item_count;
    uint16_t system_count;
    music_sp_t width;
    music_sp_t height;
} music_scene_t;

void music_layout_default_config(music_layout_config_t *config);
size_t music_scene_set_event_color(music_scene_t *scene,
                                   int source_event_index,
                                   uint32_t color_rgb);
size_t music_scene_clear_event_color(music_scene_t *scene,
                                     int source_event_index);
bool music_layout_build(const music_score_t *score,
                        const music_layout_config_t *config,
                        music_scene_t *scene,
                        char *error,
                        size_t error_size);
/* Preserve systems before the one containing first_dirty_measure and rebuild
 * only the affected tail. The preceding system is included so cross-system
 * connectors and line-breaking remain stable. */
bool music_layout_rebuild_from_measure(const music_score_t *score,
                                       const music_layout_config_t *config,
                                       music_scene_t *scene,
                                       uint16_t first_dirty_measure,
                                       char *error,
                                       size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
