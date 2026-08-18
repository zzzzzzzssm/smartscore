#include "creator_mode.h"
#include "creator_mode_integration.h"

esp_err_t creator_mode_vendor_init(lv_ui *ui);
esp_err_t creator_mode_vendor_init_detached(lv_ui *ui);
esp_err_t creator_mode_vendor_finish(void);
esp_err_t creator_mode_vendor_cancel(void);

/*
 * Compile the immutable SmartMusic implementation into this integration
 * translation unit so the product layer can adapt its private LVGL controls
 * without changing the hash-verified vendor snapshot.
 */
#define creator_mode_init creator_mode_vendor_init
#define creator_mode_init_detached creator_mode_vendor_init_detached
#define creator_mode_finish creator_mode_vendor_finish
#define creator_mode_cancel creator_mode_vendor_cancel
#include "../../../vendor/smartmusic_screen/SmartMusic/main/creator_mode.c"
#undef creator_mode_cancel
#undef creator_mode_finish
#undef creator_mode_init_detached
#undef creator_mode_init

#define TXT_SAVE_SCORE \
    "\xE4\xBF\x9D\xE5\xAD\x98\xE4\xB9\x90\xE8\xB0\xB1"
#define TXT_SELECT_REPLAY_RANGE \
    "\xE7\x82\xB9\xE4\xB8\xA4\xE5\xA4\x84\xE9\x9F\xB3\xE7\xAC\xA6" \
    "\xEF\xBC\x8C\xE9\x80\x89\xE6\x8B\xA9\xE9\x87\x8D\xE5\xBC\xB9" \
    "\xE8\x8C\x83\xE5\x9B\xB4"

static lv_obj_t *s_promoted_save_button;

static void hide_save_button(void)
{
    if (s_promoted_save_button &&
        lv_obj_is_valid(s_promoted_save_button))
        lv_obj_add_flag(s_promoted_save_button, LV_OBJ_FLAG_HIDDEN);
}

static void save_button_cb(lv_event_t *event)
{
    (void)event;
    creator_recorder_state_t state = creator_recorder_state();
    if (state == CREATOR_RECORDER_RECORDING) {
        if (pause_locked() != ESP_OK)
            return;
    } else if (state != CREATOR_RECORDER_PAUSED) {
        return;
    }

    if (finish_locked() == ESP_OK)
        hide_save_button();
}

static void exit_button_cb(lv_event_t *event)
{
    (void)event;
    hide_save_button();
}

static void promote_save_button(void)
{
    lv_obj_t *button = s_creator.finish_button;
    if (!button || !lv_obj_is_valid(button) ||
        !s_creator.ui || !s_creator.ui->music_screen)
        return;

    if (s_promoted_save_button != button ||
        lv_obj_get_parent(button) != s_creator.ui->music_screen) {
        lv_obj_remove_event_cb(button, finish_cb);
        lv_obj_remove_event_cb(button, save_button_cb);
        lv_obj_set_parent(button, s_creator.ui->music_screen);
        lv_obj_set_pos(button, 224, 10);
        lv_obj_set_size(button, 126, 48);
        lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_ACCENT), 0);

        lv_obj_t *label = lv_obj_get_child(button, 0);
        if (label) {
            lv_label_set_text(label, TXT_SAVE_SCORE);
            lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        }
        lv_obj_add_event_cb(button, save_button_cb, LV_EVENT_CLICKED, NULL);
        if (s_creator.exit_button &&
            lv_obj_is_valid(s_creator.exit_button)) {
            lv_obj_remove_event_cb(s_creator.exit_button, exit_button_cb);
            lv_obj_add_event_cb(s_creator.exit_button, exit_button_cb,
                                LV_EVENT_CLICKED, NULL);
        }

        if (s_creator.overwrite_panel &&
            lv_obj_is_valid(s_creator.overwrite_panel)) {
            lv_obj_set_pos(s_creator.overwrite_panel, 360, 7);
            lv_obj_set_size(s_creator.overwrite_panel, 365, 56);
        }
        if (s_creator.overwrite_hint_label &&
            lv_obj_is_valid(s_creator.overwrite_hint_label)) {
            lv_obj_set_pos(s_creator.overwrite_hint_label, 4, 8);
            lv_obj_set_size(s_creator.overwrite_hint_label, 347, 32);
        }

        s_promoted_save_button = button;
        ESP_LOGI(TAG, "Creator save button promoted to persistent control");
    }

    lv_obj_clear_flag(button, LV_OBJ_FLAG_HIDDEN);
}

static void creator_ui_timer_cb(lv_timer_t *timer)
{
    auto_pause_timer_cb(timer);
    if (!s_creator.active) {
        hide_save_button();
        return;
    }

    promote_save_button();
    if (creator_recorder_state() == CREATOR_RECORDER_PAUSED &&
        s_creator.range_start_measure == 0 &&
        s_creator.overwrite_hint_label &&
        lv_obj_is_valid(s_creator.overwrite_hint_label)) {
        lv_label_set_text(s_creator.overwrite_hint_label,
                          TXT_SELECT_REPLAY_RANGE);
    }
}

static esp_err_t install_creator_ui_timer(void)
{
    if (!s_creator.auto_pause_timer) {
        ESP_LOGE(TAG, "Creator UI timer is unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    lv_timer_set_cb(s_creator.auto_pause_timer, creator_ui_timer_cb);
    return ESP_OK;
}

esp_err_t creator_mode_init(lv_ui *ui)
{
    esp_err_t err = creator_mode_vendor_init(ui);
    return err == ESP_OK ? install_creator_ui_timer() : err;
}

esp_err_t creator_mode_init_detached(lv_ui *ui)
{
    esp_err_t err = creator_mode_vendor_init_detached(ui);
    return err == ESP_OK ? install_creator_ui_timer() : err;
}

esp_err_t creator_mode_finish(void)
{
    esp_err_t err = creator_mode_vendor_finish();
    if (err == ESP_OK)
        hide_save_button();
    return err;
}

esp_err_t creator_mode_cancel(void)
{
    esp_err_t err = creator_mode_vendor_cancel();
    if (err == ESP_OK)
        hide_save_button();
    return err;
}

bool creator_mode_voice_go_home(lv_obj_t *home_screen)
{
    if (!home_screen || !lv_obj_is_valid(home_screen) ||
        !s_creator.initialized) {
        return false;
    }

    lv_obj_t *active_screen = lv_screen_active();
    bool on_preparation =
        s_creator.preparation && lv_obj_is_valid(s_creator.preparation) &&
        active_screen == s_creator.preparation;
    bool on_creator_score =
        s_creator.active && s_creator.ui && s_creator.ui->music_screen &&
        active_screen == s_creator.ui->music_screen;
    if (!on_preparation && !on_creator_score)
        return false;

    s_creator.return_screen = home_screen;
    if (s_creator.active) {
        s_creator.last_error = ESP_OK;
        exit_locked();
        hide_save_button();
    } else {
        lv_screen_load_anim(home_screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT,
                            180, 0, false);
    }
    return true;
}
