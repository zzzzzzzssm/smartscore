#include "practice_advice_view.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_font.h"
#include "cJSON.h"
#include "practice_advice_service.h"

#define ADVICE_TEXT_INITIAL_CAPACITY 2048U
#define ADVICE_TEXT_MAX_CAPACITY (48U * 1024U)

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} text_builder_t;

static lv_obj_t *s_advice_screen;
static lv_obj_t *s_return_screen;

static const char *json_text(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? item->valuestring : "";
}

static bool builder_reserve(text_builder_t *builder, size_t extra)
{
    if (builder->length + extra + 1U <= builder->capacity) return true;
    size_t required = builder->length + extra + 1U;
    size_t next = builder->capacity > 0
                      ? builder->capacity
                      : ADVICE_TEXT_INITIAL_CAPACITY;
    while (next < required && next < ADVICE_TEXT_MAX_CAPACITY) next *= 2U;
    if (next > ADVICE_TEXT_MAX_CAPACITY) next = ADVICE_TEXT_MAX_CAPACITY;
    if (next < required) return false;
    char *data = realloc(builder->data, next);
    if (data == NULL) return false;
    builder->data = data;
    builder->capacity = next;
    return true;
}

static bool builder_appendf(text_builder_t *builder, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0 || !builder_reserve(builder, (size_t)needed)) {
        va_end(args);
        return false;
    }
    vsnprintf(builder->data + builder->length,
              builder->capacity - builder->length, format, args);
    va_end(args);
    builder->length += (size_t)needed;
    return true;
}

static char *format_advice(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }

    text_builder_t builder = {0};
    const char *summary = json_text(root, "summary");
    bool ok = summary[0] != '\0' &&
              builder_appendf(&builder, "总结\n%s\n\n优先练习\n", summary);

    const cJSON *focus = cJSON_GetObjectItemCaseSensitive(root, "focus");
    int focus_index = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, focus) {
        ++focus_index;
        const cJSON *practice =
            cJSON_GetObjectItemCaseSensitive(item, "practice");
        const cJSON *bpm = cJSON_GetObjectItemCaseSensitive(practice, "bpm");
        const cJSON *minutes =
            cJSON_GetObjectItemCaseSensitive(practice, "minutes");
        const cJSON *repetitions =
            cJSON_GetObjectItemCaseSensitive(practice, "repetitions");
        ok = ok && builder_appendf(
            &builder,
            "%d. %s\n练习：%s\n参数：%d BPM，%d 分钟，重复 %d 次\n达标：%s\n",
            focus_index, json_text(item, "problem"),
            json_text(practice, "action"),
            cJSON_IsNumber(bpm) ? bpm->valueint : 0,
            cJSON_IsNumber(minutes) ? minutes->valueint : 0,
            cJSON_IsNumber(repetitions) ? repetitions->valueint : 0,
            json_text(practice, "target"));

        const cJSON *evidence =
            cJSON_GetObjectItemCaseSensitive(item, "evidence");
        if (cJSON_IsArray(evidence) && cJSON_GetArraySize(evidence) > 0) {
            ok = ok && builder_appendf(&builder, "依据：");
            int evidence_index = 0;
            const cJSON *entry = NULL;
            cJSON_ArrayForEach(entry, evidence) {
                if (!cJSON_IsString(entry)) continue;
                ok = ok && builder_appendf(&builder, "%s%s",
                                            evidence_index++ > 0 ? "；" : "",
                                            entry->valuestring);
            }
            ok = ok && builder_appendf(&builder, "\n");
        }
        ok = ok && builder_appendf(&builder, "\n");
    }

    const cJSON *next = cJSON_GetObjectItemCaseSensitive(root, "next_session");
    const cJSON *total = cJSON_GetObjectItemCaseSensitive(next,
                                                          "total_minutes");
    ok = ok && builder_appendf(&builder, "下次练习（%d 分钟）\n",
                                cJSON_IsNumber(total) ? total->valueint : 0);
    const cJSON *steps = cJSON_GetObjectItemCaseSensitive(next, "steps");
    int step_index = 0;
    const cJSON *step = NULL;
    cJSON_ArrayForEach(step, steps) {
        const cJSON *minutes =
            cJSON_GetObjectItemCaseSensitive(step, "minutes");
        ok = ok && builder_appendf(
            &builder, "%d. %s（%d 分钟）\n", ++step_index,
            json_text(step, "action"),
            cJSON_IsNumber(minutes) ? minutes->valueint : 0);
    }

    const char *encouragement = json_text(root, "encouragement");
    if (encouragement[0] != '\0') {
        ok = ok && builder_appendf(&builder, "\n%s\n", encouragement);
    }
    const cJSON *insufficient =
        cJSON_GetObjectItemCaseSensitive(root, "insufficient_data");
    if (cJSON_IsArray(insufficient) && cJSON_GetArraySize(insufficient) > 0) {
        ok = ok && builder_appendf(&builder, "\n数据不足：");
        int notice_index = 0;
        const cJSON *notice = NULL;
        cJSON_ArrayForEach(notice, insufficient) {
            if (!cJSON_IsString(notice)) continue;
            ok = ok && builder_appendf(&builder, "%s%s",
                                        notice_index++ > 0 ? "；" : "",
                                        notice->valuestring);
        }
    }
    cJSON_Delete(root);
    if (!ok || builder.data == NULL) {
        free(builder.data);
        return NULL;
    }
    return builder.data;
}

static void modal_close_cb(lv_event_t *event)
{
    lv_obj_t *message_box = lv_event_get_user_data(event);
    if (message_box && lv_obj_is_valid(message_box)) {
        lv_msgbox_close_async(message_box);
    }
}

static void show_message(const char *title, const char *message)
{
    lv_obj_t *box = lv_msgbox_create(NULL);
    if (box == NULL) return;
    lv_obj_set_size(box, 620, 270);
    lv_msgbox_add_title(box, title != NULL ? title : "练习建议");
    lv_msgbox_add_text(box, message != NULL ? message : "建议暂不可用");
    lv_obj_t *button = lv_msgbox_add_footer_button(box, "知道了");
    lv_obj_add_event_cb(button, modal_close_cb, LV_EVENT_CLICKED, box);
    app_font_apply_missing_cjk(box);
}

static void back_event_cb(lv_event_t *event)
{
    (void)event;
    if (s_return_screen && lv_obj_is_valid(s_return_screen)) {
        lv_screen_load(s_return_screen);
    }
    if (s_advice_screen && lv_obj_is_valid(s_advice_screen)) {
        lv_obj_t *old = s_advice_screen;
        s_advice_screen = NULL;
        lv_obj_delete_async(old);
    }
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             int x, int y, int width, int height)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x6B3515), 0);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label);
    return button;
}

static void open_advice_screen(lv_obj_t *return_screen, const char *text)
{
    if (s_advice_screen && lv_obj_is_valid(s_advice_screen)) {
        lv_obj_delete(s_advice_screen);
    }
    s_return_screen = return_screen;
    s_advice_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_advice_screen, 1024, 600);
    lv_obj_set_style_bg_color(s_advice_screen, lv_color_hex(0xF7EDDC), 0);
    lv_obj_set_style_bg_opa(s_advice_screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_advice_screen, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *back = make_button(s_advice_screen, "返回评分",
                                 24, 20, 138, 48);
    lv_obj_add_event_cb(back, back_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(s_advice_screen);
    lv_obj_set_pos(title, 332, 24);
    lv_obj_set_size(title, 360, 46);
    lv_label_set_text(title, "本次练习建议");
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x3B2415), 0);

    lv_obj_t *content = lv_obj_create(s_advice_screen);
    lv_obj_set_pos(content, 30, 88);
    lv_obj_set_size(content, 964, 488);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_radius(content, 18, 0);
    lv_obj_set_style_bg_color(content, lv_color_hex(0xFFFDF8), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(content, 1, 0);
    lv_obj_set_style_border_color(content, lv_color_hex(0xDCC5A6), 0);
    lv_obj_set_style_pad_all(content, 24, 0);

    lv_obj_t *label = lv_label_create(content);
    lv_obj_set_width(label, 884);
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0x3B2415), 0);
    lv_obj_set_style_text_line_space(label, 8, 0);

    app_font_apply_missing_cjk(s_advice_screen);
    lv_screen_load(s_advice_screen);
}

void practice_advice_view_open(lv_obj_t *return_screen)
{
    practice_advice_status_t status;
    practice_advice_service_get_status(&status);
    if (status.state == PRACTICE_ADVICE_WAITING_SCORE ||
        status.state == PRACTICE_ADVICE_RUNNING) {
        show_message("练习建议", "建议正在后台生成，请稍等");
        return;
    }
    if (status.state == PRACTICE_ADVICE_SKIPPED_OFFLINE ||
        status.state == PRACTICE_ADVICE_FAILED) {
        show_message("建议未生成", status.message);
        return;
    }
    if (status.state != PRACTICE_ADVICE_READY) {
        show_message("练习建议", "请先完成一次练习");
        return;
    }

    char *json = NULL;
    if (practice_advice_service_copy_advice(&json, NULL) != ESP_OK) {
        show_message("建议暂不可用", "设备内存不足，无法打开练习建议");
        return;
    }
    char *text = format_advice(json);
    free(json);
    if (text == NULL) {
        show_message("建议暂不可用", "练习建议内容解析失败");
        return;
    }
    open_advice_screen(return_screen, text);
    free(text);
}
