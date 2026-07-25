#include "app_font.h"

#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_font";

/* Source Han Sans SC Bold 24 px is split into seven LVGL binary fonts.
 * Keeping each allocation small is considerably safer than parsing and
 * retaining the complete 10.6 MB TTF on the ESP32-P4. */
static lv_font_t *s_chunks[APP_FONT_CHUNK_COUNT];
static lv_font_t *s_chinese_24;

static uint32_t utf8_next(const char *text, uint32_t *offset)
{
    const uint8_t *s = (const uint8_t *)text;
    uint32_t i = *offset;
    uint32_t cp;
    if (s[i] < 0x80) {
        cp = s[i++];
    } else if ((s[i] & 0xe0) == 0xc0 && s[i + 1]) {
        cp = ((uint32_t)(s[i] & 0x1f) << 6) | (s[i + 1] & 0x3f);
        i += 2;
    } else if ((s[i] & 0xf0) == 0xe0 && s[i + 1] && s[i + 2]) {
        cp = ((uint32_t)(s[i] & 0x0f) << 12) |
             ((uint32_t)(s[i + 1] & 0x3f) << 6) | (s[i + 2] & 0x3f);
        i += 3;
    } else if ((s[i] & 0xf8) == 0xf0 && s[i + 1] && s[i + 2] && s[i + 3]) {
        cp = ((uint32_t)(s[i] & 0x07) << 18) |
             ((uint32_t)(s[i + 1] & 0x3f) << 12) |
             ((uint32_t)(s[i + 2] & 0x3f) << 6) | (s[i + 3] & 0x3f);
        i += 4;
    } else {
        cp = s[i++];
    }
    *offset = i;
    return cp;
}

static bool is_cjk(uint32_t cp)
{
    return (cp >= 0x3400 && cp <= 0x9fff) ||
           (cp >= 0xf900 && cp <= 0xfaff) ||
           (cp >= 0x20000 && cp <= 0x3134f);
}

static bool label_has_missing_cjk(lv_obj_t *label)
{
    const char *text = lv_label_get_text(label);
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    if (!text || !font) return false;
    uint32_t offset = 0;
    while (text[offset]) {
        uint32_t cp = utf8_next(text, &offset);
        if (!is_cjk(cp)) continue;
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(font, &glyph, cp, 0)) return true;
    }
    return false;
}

static void apply_missing_cjk_recursive(lv_obj_t *obj)
{
    if (!obj) return;
    if (lv_obj_check_type(obj, &lv_label_class) && label_has_missing_cjk(obj))
        lv_obj_set_style_text_font(obj, s_chinese_24, LV_PART_MAIN);
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i)
        apply_missing_cjk_recursive(lv_obj_get_child(obj, (int32_t)i));
}

static void destroy_chunks(void)
{
    for (int i = 0; i < APP_FONT_CHUNK_COUNT; ++i) {
        if (s_chunks[i]) {
            lv_binfont_destroy(s_chunks[i]);
            s_chunks[i] = NULL;
        }
    }
    s_chinese_24 = NULL;
}

static lv_font_t *load_chunk_from_psram(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long file_size = ftell(file);
    if (file_size <= 0 || file_size > 1024 * 1024L ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    uint8_t *data = heap_caps_malloc((size_t)file_size,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) data = malloc((size_t)file_size);
    if (!data) {
        fclose(file);
        return NULL;
    }
    size_t read_size = fread(data, 1, (size_t)file_size, file);
    fclose(file);
    if (read_size != (size_t)file_size) {
        free(data);
        return NULL;
    }

    /* The binary loader performs many small seeks per glyph. Feeding it from
     * MEMFS turns those operations into RAM access instead of thousands of
     * synchronous SD-card reads. It copies the final font tables/bitmaps, so
     * this temporary source buffer can be released after the call. */
    lv_font_t *font = lv_binfont_create_from_buffer(data, (uint32_t)read_size);
    free(data);
    return font;
}

esp_err_t app_font_init(void)
{
    if (s_chinese_24) return ESP_OK;

    /* Check all files through ESP VFS first, so missing SD assets produce a
     * precise path in the log instead of a generic LVGL loader failure. */
    for (int i = 0; i < APP_FONT_CHUNK_COUNT; ++i) {
        char vfs_path[96];
        snprintf(vfs_path, sizeof(vfs_path), APP_FONT_VFS_PATTERN, i);
        FILE *file = fopen(vfs_path, "rb");
        if (!file) {
            ESP_LOGE(TAG, "SD font chunk missing: %s", vfs_path);
            destroy_chunks();
            return ESP_ERR_NOT_FOUND;
        }
        fclose(file);
    }

    /* Load backwards, then link c0 -> c1 -> ... -> c6. */
    lv_font_t *fallback = (lv_font_t *)LV_FONT_DEFAULT;
    for (int i = APP_FONT_CHUNK_COUNT - 1; i >= 0; --i) {
        char vfs_path[96];
        snprintf(vfs_path, sizeof(vfs_path), APP_FONT_VFS_PATTERN, i);
        s_chunks[i] = load_chunk_from_psram(vfs_path);
        if (!s_chunks[i]) {
            ESP_LOGE(TAG, "LVGL rejected memory-loaded font chunk: %s",
                     vfs_path);
            destroy_chunks();
            return ESP_FAIL;
        }
        s_chunks[i]->fallback = fallback;
        fallback = s_chunks[i];
        ESP_LOGI(TAG, "loaded font chunk c%d from PSRAM buffer", i);
        /* Let IDLE0 run between chunks even on a particularly slow SD card. */
        vTaskDelay(1);
    }
    s_chinese_24 = s_chunks[0];

    lv_font_glyph_dsc_t test_glyph;
    if (!lv_font_get_glyph_dsc(s_chinese_24, &test_glyph, 0x4e2d, 0)) {
        ESP_LOGE(TAG, "chunked font loaded but U+4E2D is unavailable");
        destroy_chunks();
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "Source Han Sans SC Bold ready: %d chunks, %d px",
             APP_FONT_CHUNK_COUNT, APP_FONT_CHINESE_SIZE);
    return ESP_OK;
}

bool app_font_is_ready(void)
{
    return s_chinese_24 != NULL;
}

const lv_font_t *app_font_chinese_22(void)
{
    /* Compatibility name retained for existing UI code. */
    return s_chinese_24;
}

void app_font_apply_missing_cjk(lv_obj_t *root)
{
    if (!s_chinese_24 || !root) return;
    apply_missing_cjk_recursive(root);
}
