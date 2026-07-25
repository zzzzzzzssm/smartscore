/* Event glue kept intentionally small. Hand-written score UI lives in
 * custom/score_ui_flow.c so GUI Guider regeneration does not own the logic. */
#include "events_init.h"

#include "score_ui_flow.h"
#include "app_font.h"
#include "bsp/esp-bsp.h"

static lv_obj_t *s_setting_events_screen;

static void setting_back_event_handler(lv_event_t *event)
{
    (void)event;
    lv_screen_load_anim(guider_ui.screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT,
                        180, 0, false);
}

static void setting_brightness_event_handler(lv_event_t *event)
{
    bsp_display_brightness_set(
        lv_slider_get_value(lv_event_get_target(event)));
}

static void setting_reset_event_handler(lv_event_t *event)
{
    (void)event;
    lv_slider_set_value(guider_ui.setting_screen_voice_slide, 70,
                        LV_ANIM_ON);
    lv_slider_set_value(guider_ui.setting_screen_brightness_slide, 100,
                        LV_ANIM_ON);
    bsp_display_brightness_set(100);
}

void events_init_setting_screen(lv_ui *ui)
{
    if (!ui || !ui->setting_screen ||
        s_setting_events_screen == ui->setting_screen) return;
    s_setting_events_screen = ui->setting_screen;
    lv_slider_set_value(ui->setting_screen_brightness_slide, 100, LV_ANIM_OFF);
    lv_obj_add_event_cb(ui->setting_screen_back_button,
                        setting_back_event_handler, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->setting_screen_brightness_slide,
                        setting_brightness_event_handler,
                        LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(ui->setting_screen_reset_button,
                        setting_reset_event_handler, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->setting_screen_save_button,
                        setting_back_event_handler, LV_EVENT_CLICKED, ui);
    app_font_apply_missing_cjk(ui->setting_screen);
}

static void screen_setting_button_event_handler(lv_event_t *event)
{
    lv_ui *ui = lv_event_get_user_data(event);
    if (!ui) return;
    if (!ui->setting_screen || ui->setting_screen_del) {
        setup_scr_setting_screen(ui);
        ui->setting_screen_del = false;
    }
    events_init_setting_screen(ui);
    lv_screen_load_anim(ui->setting_screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT,
                        180, 0, false);
}

static void screen_start_button_event_handler(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED)
        score_ui_flow_open_choose(&guider_ui);
}

void events_init_screen(lv_ui *ui)
{
    app_font_apply_missing_cjk(ui->screen);
    if (ui->screen_start_button)
        lv_obj_add_event_cb(ui->screen_start_button,
                            screen_start_button_event_handler,
                            LV_EVENT_CLICKED, ui);
    if (ui->screen_setting_button)
        lv_obj_add_event_cb(ui->screen_setting_button,
                            screen_setting_button_event_handler,
                            LV_EVENT_CLICKED, ui);
}

void events_init_choose_screen(lv_ui *ui)
{
    score_ui_flow_init_choose(ui);
}

void events_init(lv_ui *ui)
{
    (void)ui;
}
