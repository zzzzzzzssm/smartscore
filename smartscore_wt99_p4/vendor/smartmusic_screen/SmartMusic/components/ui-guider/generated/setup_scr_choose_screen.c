/*
* Copyright 2026 NXP
* NXP Proprietary. This software is owned or controlled by NXP and may only be used strictly in
* accordance with the applicable license terms. By expressly accepting such terms or by downloading, installing,
* activating and/or otherwise using the software, you are agreeing that you have read, and that you agree to
* comply with and are bound by, such license terms.  If you do not agree to be bound by the applicable license
* terms, then you may not retain, install, activate or otherwise use the software.
*/

#include "lvgl.h"
#include <stdio.h>
#include "gui_guider.h"
#include "events_init.h"
#include "widgets_init.h"
#include "custom.h"



void setup_scr_choose_screen(lv_ui *ui)
{
    //Write codes choose_screen
    ui->choose_screen = lv_obj_create(NULL);
    lv_obj_set_size(ui->choose_screen, 1024, 600);
    lv_obj_set_scrollbar_mode(ui->choose_screen, LV_SCROLLBAR_MODE_OFF);

    //Write style for choose_screen, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->choose_screen, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->choose_screen, lv_color_hex(0xF7EDDC), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->choose_screen, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes choose_screen_label_1
    ui->choose_screen_label_1 = lv_label_create(ui->choose_screen);
    lv_obj_set_pos(ui->choose_screen_label_1, 401, 18);
    lv_obj_set_size(ui->choose_screen_label_1, 221, 50);
    lv_label_set_text(ui->choose_screen_label_1, "选择乐谱");
    lv_label_set_long_mode(ui->choose_screen_label_1, LV_LABEL_LONG_WRAP);

    //Write style for choose_screen_label_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->choose_screen_label_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->choose_screen_label_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->choose_screen_label_1, lv_color_hex(0x3A1D14), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->choose_screen_label_1, &lv_font_gudianChinese_42, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->choose_screen_label_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->choose_screen_label_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->choose_screen_label_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->choose_screen_label_1, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->choose_screen_label_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->choose_screen_label_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->choose_screen_label_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->choose_screen_label_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->choose_screen_label_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->choose_screen_label_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes choose_screen_content
    ui->choose_screen_content = lv_obj_create(ui->choose_screen);
    lv_obj_set_pos(ui->choose_screen_content, 26, 77);
    lv_obj_set_size(ui->choose_screen_content, 982, 497);
    lv_obj_set_scrollbar_mode(ui->choose_screen_content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(ui->choose_screen_content, LV_DIR_VER);
    lv_obj_set_flex_flow(ui->choose_screen_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui->choose_screen_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ui->choose_screen_content, 4, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write style for choose_screen_content, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->choose_screen_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->choose_screen_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->choose_screen_content, 128, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->choose_screen_content, lv_color_hex(0xFAF8F5), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->choose_screen_content, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->choose_screen_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->choose_screen_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->choose_screen_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->choose_screen_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->choose_screen_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes choose_screen_back_button
    ui->choose_screen_back_button = lv_button_create(ui->choose_screen);
    lv_obj_set_pos(ui->choose_screen_back_button, 20, 11);
    lv_obj_set_size(ui->choose_screen_back_button, 131, 50);
    ui->choose_screen_back_button_label = lv_label_create(ui->choose_screen_back_button);
    lv_label_set_text(ui->choose_screen_back_button_label, "返回");
    lv_label_set_long_mode(ui->choose_screen_back_button_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->choose_screen_back_button_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->choose_screen_back_button, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->choose_screen_back_button_label, LV_PCT(100));

    //Write style for choose_screen_back_button, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->choose_screen_back_button, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->choose_screen_back_button, 2, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ui->choose_screen_back_button, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui->choose_screen_back_button, lv_color_hex(0xB87D32), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(ui->choose_screen_back_button, LV_BORDER_SIDE_FULL, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->choose_screen_back_button, 8, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->choose_screen_back_button, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->choose_screen_back_button, lv_color_hex(0x3A1D14), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->choose_screen_back_button, &lv_font_gudianChinese_34, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->choose_screen_back_button, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->choose_screen_back_button, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes choose_screen_img_1
    ui->choose_screen_img_1 = lv_image_create(ui->choose_screen);
    lv_obj_set_pos(ui->choose_screen_img_1, 591, 20);
    lv_obj_set_size(ui->choose_screen_img_1, 113, 52);
    lv_obj_add_flag(ui->choose_screen_img_1, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->choose_screen_img_1, &_huawen2_RGB565A8_113x52);
    lv_image_set_pivot(ui->choose_screen_img_1, 50,50);
    lv_image_set_rotation(ui->choose_screen_img_1, 0);

    //Write style for choose_screen_img_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->choose_screen_img_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->choose_screen_img_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes choose_screen_img_2
    ui->choose_screen_img_2 = lv_image_create(ui->choose_screen);
    lv_obj_set_pos(ui->choose_screen_img_2, 304, 19);
    lv_obj_set_size(ui->choose_screen_img_2, 122, 55);
    lv_obj_add_flag(ui->choose_screen_img_2, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->choose_screen_img_2, &_huawen_RGB565A8_122x55);
    lv_image_set_pivot(ui->choose_screen_img_2, 50,50);
    lv_image_set_rotation(ui->choose_screen_img_2, 0);

    //Write style for choose_screen_img_2, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->choose_screen_img_2, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->choose_screen_img_2, 255, LV_PART_MAIN|LV_STATE_DEFAULT);

    //The custom code of choose_screen.


    //Update current screen layout.
    lv_obj_update_layout(ui->choose_screen);

    //Init events for choose_screen.
    events_init_choose_screen(ui);
}
