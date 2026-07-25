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



void setup_scr_music_preparation(lv_ui *ui)
{
    //Write codes music_preparation
    ui->music_preparation = lv_obj_create(NULL);
    lv_obj_set_size(ui->music_preparation, 1024, 600);
    lv_obj_set_scrollbar_mode(ui->music_preparation, LV_SCROLLBAR_MODE_OFF);

    //Write style for music_preparation, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->music_preparation, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->music_preparation, lv_color_hex(0xF7EDDC), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->music_preparation, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_Title
    ui->music_preparation_Title = lv_label_create(ui->music_preparation);
    lv_obj_set_pos(ui->music_preparation_Title, 351, 19);
    lv_obj_set_size(ui->music_preparation_Title, 320, 57);
    lv_label_set_text(ui->music_preparation_Title, "演奏准备");
    lv_label_set_long_mode(ui->music_preparation_Title, LV_LABEL_LONG_WRAP);

    //Write style for music_preparation_Title, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->music_preparation_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_Title, lv_color_hex(0x2C1810), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_Title, &lv_font_gudianChinese_42, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_Title, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->music_preparation_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->music_preparation_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_Title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->music_preparation_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->music_preparation_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->music_preparation_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->music_preparation_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->music_preparation_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_content
    ui->music_preparation_content = lv_obj_create(ui->music_preparation);
    lv_obj_set_pos(ui->music_preparation_content, 49, 76);
    lv_obj_set_size(ui->music_preparation_content, 937, 516);
    lv_obj_set_scrollbar_mode(ui->music_preparation_content, LV_SCROLLBAR_MODE_OFF);

    //Write style for music_preparation_content, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->music_preparation_content, 2, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ui->music_preparation_content, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui->music_preparation_content, lv_color_hex(0xE5D6C1), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(ui->music_preparation_content, LV_BORDER_SIDE_FULL, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_content, 15, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->music_preparation_content, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->music_preparation_content, lv_color_hex(0xF6EFE4), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->music_preparation_content, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->music_preparation_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->music_preparation_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->music_preparation_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->music_preparation_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_line_1
    ui->music_preparation_line_1 = lv_line_create(ui->music_preparation_content);
    lv_obj_set_pos(ui->music_preparation_line_1, 13, 154);
    lv_obj_set_size(ui->music_preparation_line_1, 906, 5);
    static lv_point_precise_t music_preparation_line_1[] = {{0, 0},{1024, 0}};
    lv_line_set_points(ui->music_preparation_line_1, music_preparation_line_1, 2);

    //Write style for music_preparation_line_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_line_width(ui->music_preparation_line_1, 2, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(ui->music_preparation_line_1, lv_color_hex(0xE5D6C1), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_opa(ui->music_preparation_line_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(ui->music_preparation_line_1, true, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_line_2
    ui->music_preparation_line_2 = lv_line_create(ui->music_preparation_content);
    lv_obj_set_pos(ui->music_preparation_line_2, 224, 34);
    lv_obj_set_size(ui->music_preparation_line_2, 4, 91);
    static lv_point_precise_t music_preparation_line_2[] = {{0, 0},{0, 100}};
    lv_line_set_points(ui->music_preparation_line_2, music_preparation_line_2, 2);

    //Write style for music_preparation_line_2, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_line_width(ui->music_preparation_line_2, 2, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(ui->music_preparation_line_2, lv_color_hex(0xE5D6C1), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_opa(ui->music_preparation_line_2, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(ui->music_preparation_line_2, true, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_line_3
    ui->music_preparation_line_3 = lv_line_create(ui->music_preparation_content);
    lv_obj_set_pos(ui->music_preparation_line_3, 478, 34);
    lv_obj_set_size(ui->music_preparation_line_3, 2, 93);
    static lv_point_precise_t music_preparation_line_3[] = {{0, 0},{0, 100}};
    lv_line_set_points(ui->music_preparation_line_3, music_preparation_line_3, 2);

    //Write style for music_preparation_line_3, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_line_width(ui->music_preparation_line_3, 2, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(ui->music_preparation_line_3, lv_color_hex(0xE5D6C1), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_opa(ui->music_preparation_line_3, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(ui->music_preparation_line_3, true, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_line_4
    ui->music_preparation_line_4 = lv_line_create(ui->music_preparation_content);
    lv_obj_set_pos(ui->music_preparation_line_4, 706, 35);
    lv_obj_set_size(ui->music_preparation_line_4, 4, 92);
    static lv_point_precise_t music_preparation_line_4[] = {{0, 0},{0, 100}};
    lv_line_set_points(ui->music_preparation_line_4, music_preparation_line_4, 2);

    //Write style for music_preparation_line_4, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_line_width(ui->music_preparation_line_4, 2, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(ui->music_preparation_line_4, lv_color_hex(0xE5D6C1), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_opa(ui->music_preparation_line_4, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(ui->music_preparation_line_4, true, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_speed_icon
    ui->music_preparation_speed_icon = lv_label_create(ui->music_preparation_content);
    lv_obj_set_pos(ui->music_preparation_speed_icon, 790, 22);
    lv_obj_set_size(ui->music_preparation_speed_icon, 53, 53);
    lv_label_set_text(ui->music_preparation_speed_icon, "");
    lv_label_set_long_mode(ui->music_preparation_speed_icon, LV_LABEL_LONG_WRAP);

    //Write style for music_preparation_speed_icon, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->music_preparation_speed_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_speed_icon, 49, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_speed_icon, lv_color_hex(0x000000), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_speed_icon, &lv_font_montserratMedium_16, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_speed_icon, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->music_preparation_speed_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->music_preparation_speed_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_speed_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->music_preparation_speed_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->music_preparation_speed_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->music_preparation_speed_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->music_preparation_speed_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->music_preparation_speed_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_src(ui->music_preparation_speed_icon, &_1music_RGB565A8_53x53, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_opa(ui->music_preparation_speed_icon, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_recolor_opa(ui->music_preparation_speed_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_speed_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_diaoxing_icon
    ui->music_preparation_diaoxing_icon = lv_label_create(ui->music_preparation_content);
    lv_obj_set_pos(ui->music_preparation_diaoxing_icon, 322, 21);
    lv_obj_set_size(ui->music_preparation_diaoxing_icon, 53, 53);
    lv_label_set_text(ui->music_preparation_diaoxing_icon, "");
    lv_label_set_long_mode(ui->music_preparation_diaoxing_icon, LV_LABEL_LONG_WRAP);

    //Write style for music_preparation_diaoxing_icon, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->music_preparation_diaoxing_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_diaoxing_icon, 49, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_diaoxing_icon, lv_color_hex(0x000000), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_diaoxing_icon, &lv_font_montserratMedium_16, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_diaoxing_icon, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->music_preparation_diaoxing_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->music_preparation_diaoxing_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_diaoxing_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->music_preparation_diaoxing_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->music_preparation_diaoxing_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->music_preparation_diaoxing_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->music_preparation_diaoxing_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->music_preparation_diaoxing_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_src(ui->music_preparation_diaoxing_icon, &_music_RGB565A8_53x53, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_opa(ui->music_preparation_diaoxing_icon, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_recolor_opa(ui->music_preparation_diaoxing_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_diaoxing_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_measurement_icon
    ui->music_preparation_measurement_icon = lv_label_create(ui->music_preparation_content);
    lv_obj_set_pos(ui->music_preparation_measurement_icon, 79, 21);
    lv_obj_set_size(ui->music_preparation_measurement_icon, 53, 53);
    lv_label_set_text(ui->music_preparation_measurement_icon, "");
    lv_label_set_long_mode(ui->music_preparation_measurement_icon, LV_LABEL_LONG_WRAP);

    //Write style for music_preparation_measurement_icon, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->music_preparation_measurement_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_measurement_icon, 49, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_measurement_icon, lv_color_hex(0x000000), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_measurement_icon, &lv_font_montserratMedium_16, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_measurement_icon, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->music_preparation_measurement_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->music_preparation_measurement_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_measurement_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->music_preparation_measurement_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->music_preparation_measurement_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->music_preparation_measurement_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->music_preparation_measurement_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->music_preparation_measurement_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_src(ui->music_preparation_measurement_icon, &_piano_RGB565A8_53x53, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_opa(ui->music_preparation_measurement_icon, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_recolor_opa(ui->music_preparation_measurement_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_measurement_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_paishu_icon
    ui->music_preparation_paishu_icon = lv_label_create(ui->music_preparation_content);
    lv_obj_set_pos(ui->music_preparation_paishu_icon, 568, 22);
    lv_obj_set_size(ui->music_preparation_paishu_icon, 53, 53);
    lv_label_set_text(ui->music_preparation_paishu_icon, "");
    lv_label_set_long_mode(ui->music_preparation_paishu_icon, LV_LABEL_LONG_WRAP);

    //Write style for music_preparation_paishu_icon, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->music_preparation_paishu_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_paishu_icon, 49, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_paishu_icon, lv_color_hex(0x000000), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_paishu_icon, &lv_font_montserratMedium_16, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_paishu_icon, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->music_preparation_paishu_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->music_preparation_paishu_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_paishu_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->music_preparation_paishu_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->music_preparation_paishu_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->music_preparation_paishu_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->music_preparation_paishu_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->music_preparation_paishu_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_src(ui->music_preparation_paishu_icon, &_1music_RGB565A8_53x53, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_opa(ui->music_preparation_paishu_icon, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_recolor_opa(ui->music_preparation_paishu_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_paishu_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_measurement_title
    ui->music_preparation_measurement_title = lv_label_create(ui->music_preparation_content);
    lv_obj_set_pos(ui->music_preparation_measurement_title, 83, 87);
    lv_obj_set_size(ui->music_preparation_measurement_title, 44, 21);
    lv_label_set_text(ui->music_preparation_measurement_title, "乐器");
    lv_label_set_long_mode(ui->music_preparation_measurement_title, LV_LABEL_LONG_WRAP);

    //Write style for music_preparation_measurement_title, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->music_preparation_measurement_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_measurement_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_measurement_title, lv_color_hex(0x2C1810), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_measurement_title, &lv_font_SourceHanSansSCBold_18, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_measurement_title, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->music_preparation_measurement_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->music_preparation_measurement_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_measurement_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->music_preparation_measurement_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->music_preparation_measurement_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->music_preparation_measurement_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->music_preparation_measurement_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->music_preparation_measurement_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_measurement_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_measure_text
    ui->music_preparation_measure_text = lv_label_create(ui->music_preparation_content);
    lv_obj_set_pos(ui->music_preparation_measure_text, 45, 115);
    lv_obj_set_size(ui->music_preparation_measure_text, 120, 32);
    lv_label_set_text(ui->music_preparation_measure_text, "钢琴");
    lv_label_set_long_mode(ui->music_preparation_measure_text, LV_LABEL_LONG_WRAP);

    //Write style for music_preparation_measure_text, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->music_preparation_measure_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_measure_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_measure_text, lv_color_hex(0x2C1810), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_measure_text, &lv_font_SourceHanSansSCBold_26, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_measure_text, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->music_preparation_measure_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->music_preparation_measure_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_measure_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->music_preparation_measure_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->music_preparation_measure_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->music_preparation_measure_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->music_preparation_measure_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->music_preparation_measure_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_measure_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_diaoxing_title
    ui->music_preparation_diaoxing_title = lv_label_create(ui->music_preparation_content);
    lv_obj_set_pos(ui->music_preparation_diaoxing_title, 328, 86);
    lv_obj_set_size(ui->music_preparation_diaoxing_title, 44, 21);
    lv_label_set_text(ui->music_preparation_diaoxing_title, "调性");
    lv_label_set_long_mode(ui->music_preparation_diaoxing_title, LV_LABEL_LONG_WRAP);

    //Write style for music_preparation_diaoxing_title, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->music_preparation_diaoxing_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_diaoxing_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_diaoxing_title, lv_color_hex(0x2C1810), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_diaoxing_title, &lv_font_SourceHanSansSCBold_18, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_diaoxing_title, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->music_preparation_diaoxing_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->music_preparation_diaoxing_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_diaoxing_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->music_preparation_diaoxing_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->music_preparation_diaoxing_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->music_preparation_diaoxing_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->music_preparation_diaoxing_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->music_preparation_diaoxing_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_diaoxing_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_paishu_title
    ui->music_preparation_paishu_title = lv_label_create(ui->music_preparation_content);
    lv_obj_set_pos(ui->music_preparation_paishu_title, 573, 81);
    lv_obj_set_size(ui->music_preparation_paishu_title, 44, 21);
    lv_label_set_text(ui->music_preparation_paishu_title, "拍数");
    lv_label_set_long_mode(ui->music_preparation_paishu_title, LV_LABEL_LONG_WRAP);

    //Write style for music_preparation_paishu_title, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->music_preparation_paishu_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_paishu_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_paishu_title, lv_color_hex(0x2C1810), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_paishu_title, &lv_font_SourceHanSansSCBold_18, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_paishu_title, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->music_preparation_paishu_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->music_preparation_paishu_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_paishu_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->music_preparation_paishu_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->music_preparation_paishu_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->music_preparation_paishu_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->music_preparation_paishu_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->music_preparation_paishu_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_paishu_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_speed_title
    ui->music_preparation_speed_title = lv_label_create(ui->music_preparation_content);
    lv_obj_set_pos(ui->music_preparation_speed_title, 793, 83);
    lv_obj_set_size(ui->music_preparation_speed_title, 44, 21);
    lv_label_set_text(ui->music_preparation_speed_title, "速度");
    lv_label_set_long_mode(ui->music_preparation_speed_title, LV_LABEL_LONG_WRAP);

    //Write style for music_preparation_speed_title, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->music_preparation_speed_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_speed_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_speed_title, lv_color_hex(0x2C1810), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_speed_title, &lv_font_SourceHanSansSCBold_18, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_speed_title, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->music_preparation_speed_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->music_preparation_speed_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_speed_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->music_preparation_speed_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->music_preparation_speed_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->music_preparation_speed_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->music_preparation_speed_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->music_preparation_speed_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_speed_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_diaoxing_text
    ui->music_preparation_diaoxing_text = lv_label_create(ui->music_preparation_content);
    lv_obj_set_pos(ui->music_preparation_diaoxing_text, 288, 113);
    lv_obj_set_size(ui->music_preparation_diaoxing_text, 120, 32);
    lv_label_set_text(ui->music_preparation_diaoxing_text, "C大调");
    lv_label_set_long_mode(ui->music_preparation_diaoxing_text, LV_LABEL_LONG_WRAP);

    //Write style for music_preparation_diaoxing_text, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->music_preparation_diaoxing_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_diaoxing_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_diaoxing_text, lv_color_hex(0x2C1810), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_diaoxing_text, &lv_font_SourceHanSansSCBold_26, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_diaoxing_text, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->music_preparation_diaoxing_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->music_preparation_diaoxing_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_diaoxing_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->music_preparation_diaoxing_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->music_preparation_diaoxing_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->music_preparation_diaoxing_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->music_preparation_diaoxing_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->music_preparation_diaoxing_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_diaoxing_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_paishu_text
    ui->music_preparation_paishu_text = lv_label_create(ui->music_preparation_content);
    lv_obj_set_pos(ui->music_preparation_paishu_text, 535, 114);
    lv_obj_set_size(ui->music_preparation_paishu_text, 120, 32);
    lv_label_set_text(ui->music_preparation_paishu_text, "4/4");
    lv_label_set_long_mode(ui->music_preparation_paishu_text, LV_LABEL_LONG_WRAP);

    //Write style for music_preparation_paishu_text, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->music_preparation_paishu_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_paishu_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_paishu_text, lv_color_hex(0x2C1810), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_paishu_text, &lv_font_SourceHanSansSCBold_26, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_paishu_text, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->music_preparation_paishu_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->music_preparation_paishu_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_paishu_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->music_preparation_paishu_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->music_preparation_paishu_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->music_preparation_paishu_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->music_preparation_paishu_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->music_preparation_paishu_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_paishu_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_speed_text
    ui->music_preparation_speed_text = lv_label_create(ui->music_preparation_content);
    lv_obj_set_pos(ui->music_preparation_speed_text, 755, 114);
    lv_obj_set_size(ui->music_preparation_speed_text, 120, 32);
    lv_label_set_text(ui->music_preparation_speed_text, "76");
    lv_label_set_long_mode(ui->music_preparation_speed_text, LV_LABEL_LONG_WRAP);

    //Write style for music_preparation_speed_text, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->music_preparation_speed_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_speed_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_speed_text, lv_color_hex(0x2C1810), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_speed_text, &lv_font_SourceHanSansSCBold_26, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_speed_text, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->music_preparation_speed_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->music_preparation_speed_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_speed_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->music_preparation_speed_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->music_preparation_speed_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->music_preparation_speed_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->music_preparation_speed_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->music_preparation_speed_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_speed_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_setting_content
    ui->music_preparation_setting_content = lv_obj_create(ui->music_preparation_content);
    lv_obj_set_pos(ui->music_preparation_setting_content, 13, 167);
    lv_obj_set_size(ui->music_preparation_setting_content, 914, 254);
    lv_obj_set_scrollbar_mode(ui->music_preparation_setting_content, LV_SCROLLBAR_MODE_ON);

    //Write style for music_preparation_setting_content, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->music_preparation_setting_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_setting_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->music_preparation_setting_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->music_preparation_setting_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->music_preparation_setting_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->music_preparation_setting_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->music_preparation_setting_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_setting_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_metronme_title
    ui->music_preparation_metronme_title = lv_label_create(ui->music_preparation_setting_content);
    lv_obj_set_pos(ui->music_preparation_metronme_title, 8, 10);
    lv_obj_set_size(ui->music_preparation_metronme_title, 149, 32);
    lv_label_set_text(ui->music_preparation_metronme_title, "节拍器设置");
    lv_label_set_long_mode(ui->music_preparation_metronme_title, LV_LABEL_LONG_WRAP);

    //Write style for music_preparation_metronme_title, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->music_preparation_metronme_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_metronme_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_metronme_title, lv_color_hex(0x2C1810), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_metronme_title, &lv_font_SourceHanSansSCBold_28, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_metronme_title, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->music_preparation_metronme_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->music_preparation_metronme_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_metronme_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->music_preparation_metronme_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->music_preparation_metronme_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->music_preparation_metronme_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->music_preparation_metronme_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->music_preparation_metronme_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_metronme_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_metronome_switch
    ui->music_preparation_metronome_switch = lv_switch_create(ui->music_preparation_setting_content);
    lv_obj_set_pos(ui->music_preparation_metronome_switch, 177, 13);
    lv_obj_set_size(ui->music_preparation_metronome_switch, 63, 24);

    //Write style for music_preparation_metronome_switch, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->music_preparation_metronome_switch, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->music_preparation_metronome_switch, lv_color_hex(0xF7EDDC), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->music_preparation_metronome_switch, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->music_preparation_metronome_switch, 2, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ui->music_preparation_metronome_switch, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui->music_preparation_metronome_switch, lv_color_hex(0xE5D6C1), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(ui->music_preparation_metronome_switch, LV_BORDER_SIDE_FULL, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_metronome_switch, 20, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_metronome_switch, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write style for music_preparation_metronome_switch, Part: LV_PART_INDICATOR, State: LV_STATE_CHECKED.
    lv_obj_set_style_bg_opa(ui->music_preparation_metronome_switch, 255, LV_PART_INDICATOR|LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(ui->music_preparation_metronome_switch, lv_color_hex(0x2C1810), LV_PART_INDICATOR|LV_STATE_CHECKED);
    lv_obj_set_style_bg_grad_dir(ui->music_preparation_metronome_switch, LV_GRAD_DIR_NONE, LV_PART_INDICATOR|LV_STATE_CHECKED);
    lv_obj_set_style_border_width(ui->music_preparation_metronome_switch, 0, LV_PART_INDICATOR|LV_STATE_CHECKED);

    //Write style for music_preparation_metronome_switch, Part: LV_PART_KNOB, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->music_preparation_metronome_switch, 255, LV_PART_KNOB|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->music_preparation_metronome_switch, lv_color_hex(0xffffff), LV_PART_KNOB|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->music_preparation_metronome_switch, LV_GRAD_DIR_NONE, LV_PART_KNOB|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->music_preparation_metronome_switch, 0, LV_PART_KNOB|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_metronome_switch, 10, LV_PART_KNOB|LV_STATE_DEFAULT);

    //Write codes music_preparation_metronme_speed_title
    ui->music_preparation_metronme_speed_title = lv_label_create(ui->music_preparation_setting_content);
    lv_obj_set_pos(ui->music_preparation_metronme_speed_title, 55, 55);
    lv_obj_set_size(ui->music_preparation_metronme_speed_title, 100, 32);
    lv_label_set_text(ui->music_preparation_metronme_speed_title, "速度设置:");
    lv_label_set_long_mode(ui->music_preparation_metronme_speed_title, LV_LABEL_LONG_WRAP);

    //Write style for music_preparation_metronme_speed_title, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->music_preparation_metronme_speed_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_metronme_speed_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_metronme_speed_title, lv_color_hex(0x2C1810), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_metronme_speed_title, &lv_font_SourceHanSansSCBold_22, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_metronme_speed_title, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->music_preparation_metronme_speed_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->music_preparation_metronme_speed_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_metronme_speed_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->music_preparation_metronme_speed_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->music_preparation_metronme_speed_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->music_preparation_metronme_speed_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->music_preparation_metronme_speed_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->music_preparation_metronme_speed_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_metronme_speed_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_metroneme_speed_button_1
    ui->music_preparation_metroneme_speed_button_1 = lv_button_create(ui->music_preparation_setting_content);
    lv_obj_set_pos(ui->music_preparation_metroneme_speed_button_1, 177, 44);
    lv_obj_set_size(ui->music_preparation_metroneme_speed_button_1, 111, 55);
    ui->music_preparation_metroneme_speed_button_1_label = lv_label_create(ui->music_preparation_metroneme_speed_button_1);
    lv_label_set_text(ui->music_preparation_metroneme_speed_button_1_label, "60");
    lv_label_set_long_mode(ui->music_preparation_metroneme_speed_button_1_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->music_preparation_metroneme_speed_button_1_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->music_preparation_metroneme_speed_button_1, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->music_preparation_metroneme_speed_button_1_label, LV_PCT(100));

    //Write style for music_preparation_metroneme_speed_button_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->music_preparation_metroneme_speed_button_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->music_preparation_metroneme_speed_button_1, lv_color_hex(0xEEDEC6), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->music_preparation_metroneme_speed_button_1, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->music_preparation_metroneme_speed_button_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_metroneme_speed_button_1, 5, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_metroneme_speed_button_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_metroneme_speed_button_1, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_metroneme_speed_button_1, &lv_font_SourceHanSansSCBold_26, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_metroneme_speed_button_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_metroneme_speed_button_1, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_metroneme_speed_button_3
    ui->music_preparation_metroneme_speed_button_3 = lv_button_create(ui->music_preparation_setting_content);
    lv_obj_set_pos(ui->music_preparation_metroneme_speed_button_3, 459, 44);
    lv_obj_set_size(ui->music_preparation_metroneme_speed_button_3, 111, 55);
    ui->music_preparation_metroneme_speed_button_3_label = lv_label_create(ui->music_preparation_metroneme_speed_button_3);
    lv_label_set_text(ui->music_preparation_metroneme_speed_button_3_label, "100");
    lv_label_set_long_mode(ui->music_preparation_metroneme_speed_button_3_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->music_preparation_metroneme_speed_button_3_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->music_preparation_metroneme_speed_button_3, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->music_preparation_metroneme_speed_button_3_label, LV_PCT(100));

    //Write style for music_preparation_metroneme_speed_button_3, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->music_preparation_metroneme_speed_button_3, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->music_preparation_metroneme_speed_button_3, lv_color_hex(0xEEDEC6), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->music_preparation_metroneme_speed_button_3, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->music_preparation_metroneme_speed_button_3, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_metroneme_speed_button_3, 5, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_metroneme_speed_button_3, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_metroneme_speed_button_3, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_metroneme_speed_button_3, &lv_font_SourceHanSansSCBold_26, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_metroneme_speed_button_3, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_metroneme_speed_button_3, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_metroneme_speed_button_2
    ui->music_preparation_metroneme_speed_button_2 = lv_button_create(ui->music_preparation_setting_content);
    lv_obj_set_pos(ui->music_preparation_metroneme_speed_button_2, 313, 44);
    lv_obj_set_size(ui->music_preparation_metroneme_speed_button_2, 111, 55);
    ui->music_preparation_metroneme_speed_button_2_label = lv_label_create(ui->music_preparation_metroneme_speed_button_2);
    lv_label_set_text(ui->music_preparation_metroneme_speed_button_2_label, "80");
    lv_label_set_long_mode(ui->music_preparation_metroneme_speed_button_2_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->music_preparation_metroneme_speed_button_2_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->music_preparation_metroneme_speed_button_2, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->music_preparation_metroneme_speed_button_2_label, LV_PCT(100));

    //Write style for music_preparation_metroneme_speed_button_2, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->music_preparation_metroneme_speed_button_2, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->music_preparation_metroneme_speed_button_2, lv_color_hex(0x3f2204), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->music_preparation_metroneme_speed_button_2, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->music_preparation_metroneme_speed_button_2, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_metroneme_speed_button_2, 5, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_metroneme_speed_button_2, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_metroneme_speed_button_2, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_metroneme_speed_button_2, &lv_font_SourceHanSansSCBold_26, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_metroneme_speed_button_2, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_metroneme_speed_button_2, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_metroneme_speed_button_4
    ui->music_preparation_metroneme_speed_button_4 = lv_button_create(ui->music_preparation_setting_content);
    lv_obj_set_pos(ui->music_preparation_metroneme_speed_button_4, 600, 44);
    lv_obj_set_size(ui->music_preparation_metroneme_speed_button_4, 111, 55);
    ui->music_preparation_metroneme_speed_button_4_label = lv_label_create(ui->music_preparation_metroneme_speed_button_4);
    lv_label_set_text(ui->music_preparation_metroneme_speed_button_4_label, "120");
    lv_label_set_long_mode(ui->music_preparation_metroneme_speed_button_4_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->music_preparation_metroneme_speed_button_4_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->music_preparation_metroneme_speed_button_4, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->music_preparation_metroneme_speed_button_4_label, LV_PCT(100));

    //Write style for music_preparation_metroneme_speed_button_4, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->music_preparation_metroneme_speed_button_4, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->music_preparation_metroneme_speed_button_4, lv_color_hex(0xEEDEC6), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->music_preparation_metroneme_speed_button_4, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->music_preparation_metroneme_speed_button_4, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_metroneme_speed_button_4, 5, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_metroneme_speed_button_4, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_metroneme_speed_button_4, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_metroneme_speed_button_4, &lv_font_SourceHanSansSCBold_26, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_metroneme_speed_button_4, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_metroneme_speed_button_4, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_metronment_paihao_title
    ui->music_preparation_metronment_paihao_title = lv_label_create(ui->music_preparation_setting_content);
    lv_obj_set_pos(ui->music_preparation_metronment_paihao_title, 56, 122);
    lv_obj_set_size(ui->music_preparation_metronment_paihao_title, 100, 32);
    lv_label_set_text(ui->music_preparation_metronment_paihao_title, "拍号设置:");
    lv_label_set_long_mode(ui->music_preparation_metronment_paihao_title, LV_LABEL_LONG_WRAP);

    //Write style for music_preparation_metronment_paihao_title, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->music_preparation_metronment_paihao_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_metronment_paihao_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_metronment_paihao_title, lv_color_hex(0x2C1810), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_metronment_paihao_title, &lv_font_SourceHanSansSCBold_22, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_metronment_paihao_title, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->music_preparation_metronment_paihao_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->music_preparation_metronment_paihao_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_metronment_paihao_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->music_preparation_metronment_paihao_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->music_preparation_metronment_paihao_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->music_preparation_metronment_paihao_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->music_preparation_metronment_paihao_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->music_preparation_metronment_paihao_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_metronment_paihao_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_metronment_paihao_button_1
    ui->music_preparation_metronment_paihao_button_1 = lv_button_create(ui->music_preparation_setting_content);
    lv_obj_set_pos(ui->music_preparation_metronment_paihao_button_1, 176, 112);
    lv_obj_set_size(ui->music_preparation_metronment_paihao_button_1, 111, 55);
    ui->music_preparation_metronment_paihao_button_1_label = lv_label_create(ui->music_preparation_metronment_paihao_button_1);
    lv_label_set_text(ui->music_preparation_metronment_paihao_button_1_label, "2/4");
    lv_label_set_long_mode(ui->music_preparation_metronment_paihao_button_1_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->music_preparation_metronment_paihao_button_1_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->music_preparation_metronment_paihao_button_1, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->music_preparation_metronment_paihao_button_1_label, LV_PCT(100));

    //Write style for music_preparation_metronment_paihao_button_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->music_preparation_metronment_paihao_button_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->music_preparation_metronment_paihao_button_1, lv_color_hex(0xEEDEC6), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->music_preparation_metronment_paihao_button_1, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->music_preparation_metronment_paihao_button_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_metronment_paihao_button_1, 5, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_metronment_paihao_button_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_metronment_paihao_button_1, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_metronment_paihao_button_1, &lv_font_SourceHanSansSCBold_26, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_metronment_paihao_button_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_metronment_paihao_button_1, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_metronment_paihao_button_2
    ui->music_preparation_metronment_paihao_button_2 = lv_button_create(ui->music_preparation_setting_content);
    lv_obj_set_pos(ui->music_preparation_metronment_paihao_button_2, 312, 112);
    lv_obj_set_size(ui->music_preparation_metronment_paihao_button_2, 111, 55);
    ui->music_preparation_metronment_paihao_button_2_label = lv_label_create(ui->music_preparation_metronment_paihao_button_2);
    lv_label_set_text(ui->music_preparation_metronment_paihao_button_2_label, "3/4");
    lv_label_set_long_mode(ui->music_preparation_metronment_paihao_button_2_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->music_preparation_metronment_paihao_button_2_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->music_preparation_metronment_paihao_button_2, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->music_preparation_metronment_paihao_button_2_label, LV_PCT(100));

    //Write style for music_preparation_metronment_paihao_button_2, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->music_preparation_metronment_paihao_button_2, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->music_preparation_metronment_paihao_button_2, lv_color_hex(0x3f2204), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->music_preparation_metronment_paihao_button_2, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->music_preparation_metronment_paihao_button_2, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_metronment_paihao_button_2, 5, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_metronment_paihao_button_2, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_metronment_paihao_button_2, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_metronment_paihao_button_2, &lv_font_SourceHanSansSCBold_26, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_metronment_paihao_button_2, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_metronment_paihao_button_2, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_metronment_paihao_button_3
    ui->music_preparation_metronment_paihao_button_3 = lv_button_create(ui->music_preparation_setting_content);
    lv_obj_set_pos(ui->music_preparation_metronment_paihao_button_3, 460, 112);
    lv_obj_set_size(ui->music_preparation_metronment_paihao_button_3, 111, 55);
    ui->music_preparation_metronment_paihao_button_3_label = lv_label_create(ui->music_preparation_metronment_paihao_button_3);
    lv_label_set_text(ui->music_preparation_metronment_paihao_button_3_label, "4/4");
    lv_label_set_long_mode(ui->music_preparation_metronment_paihao_button_3_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->music_preparation_metronment_paihao_button_3_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->music_preparation_metronment_paihao_button_3, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->music_preparation_metronment_paihao_button_3_label, LV_PCT(100));

    //Write style for music_preparation_metronment_paihao_button_3, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->music_preparation_metronment_paihao_button_3, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->music_preparation_metronment_paihao_button_3, lv_color_hex(0xEEDEC6), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->music_preparation_metronment_paihao_button_3, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->music_preparation_metronment_paihao_button_3, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_metronment_paihao_button_3, 5, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_metronment_paihao_button_3, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_metronment_paihao_button_3, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_metronment_paihao_button_3, &lv_font_SourceHanSansSCBold_26, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_metronment_paihao_button_3, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_metronment_paihao_button_3, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_metronment_paihao_button_4
    ui->music_preparation_metronment_paihao_button_4 = lv_button_create(ui->music_preparation_setting_content);
    lv_obj_set_pos(ui->music_preparation_metronment_paihao_button_4, 601, 112);
    lv_obj_set_size(ui->music_preparation_metronment_paihao_button_4, 111, 55);
    ui->music_preparation_metronment_paihao_button_4_label = lv_label_create(ui->music_preparation_metronment_paihao_button_4);
    lv_label_set_text(ui->music_preparation_metronment_paihao_button_4_label, "6/8");
    lv_label_set_long_mode(ui->music_preparation_metronment_paihao_button_4_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->music_preparation_metronment_paihao_button_4_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->music_preparation_metronment_paihao_button_4, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->music_preparation_metronment_paihao_button_4_label, LV_PCT(100));

    //Write style for music_preparation_metronment_paihao_button_4, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->music_preparation_metronment_paihao_button_4, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->music_preparation_metronment_paihao_button_4, lv_color_hex(0xEEDEC6), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->music_preparation_metronment_paihao_button_4, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->music_preparation_metronment_paihao_button_4, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_metronment_paihao_button_4, 5, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_metronment_paihao_button_4, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_metronment_paihao_button_4, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_metronment_paihao_button_4, &lv_font_SourceHanSansSCBold_26, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_metronment_paihao_button_4, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_metronment_paihao_button_4, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_yinpin_title
    ui->music_preparation_yinpin_title = lv_label_create(ui->music_preparation_setting_content);
    lv_obj_set_pos(ui->music_preparation_yinpin_title, 7, 195);
    lv_obj_set_size(ui->music_preparation_yinpin_title, 134, 32);
    lv_label_set_text(ui->music_preparation_yinpin_title, "音频设置");
    lv_label_set_long_mode(ui->music_preparation_yinpin_title, LV_LABEL_LONG_WRAP);

    //Write style for music_preparation_yinpin_title, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->music_preparation_yinpin_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_yinpin_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_yinpin_title, lv_color_hex(0x2C1810), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_yinpin_title, &lv_font_SourceHanSansSCBold_28, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_yinpin_title, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->music_preparation_yinpin_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->music_preparation_yinpin_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_yinpin_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->music_preparation_yinpin_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->music_preparation_yinpin_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->music_preparation_yinpin_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->music_preparation_yinpin_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->music_preparation_yinpin_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_yinpin_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_metronment_paihao_input_button_2
    ui->music_preparation_metronment_paihao_input_button_2 = lv_button_create(ui->music_preparation_setting_content);
    lv_obj_set_pos(ui->music_preparation_metronment_paihao_input_button_2, 401, 182);
    lv_obj_set_size(ui->music_preparation_metronment_paihao_input_button_2, 192, 55);
    ui->music_preparation_metronment_paihao_input_button_2_label = lv_label_create(ui->music_preparation_metronment_paihao_input_button_2);
    lv_label_set_text(ui->music_preparation_metronment_paihao_input_button_2_label, "MIDI直插输入");
    lv_label_set_long_mode(ui->music_preparation_metronment_paihao_input_button_2_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->music_preparation_metronment_paihao_input_button_2_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->music_preparation_metronment_paihao_input_button_2, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->music_preparation_metronment_paihao_input_button_2_label, LV_PCT(100));

    //Write style for music_preparation_metronment_paihao_input_button_2, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->music_preparation_metronment_paihao_input_button_2, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->music_preparation_metronment_paihao_input_button_2, lv_color_hex(0x3f2204), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->music_preparation_metronment_paihao_input_button_2, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->music_preparation_metronment_paihao_input_button_2, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_metronment_paihao_input_button_2, 5, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_metronment_paihao_input_button_2, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_metronment_paihao_input_button_2, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_metronment_paihao_input_button_2, &lv_font_SourceHanSansSCBold_26, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_metronment_paihao_input_button_2, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_metronment_paihao_input_button_2, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_metronment_paihao_input_button_1
    ui->music_preparation_metronment_paihao_input_button_1 = lv_button_create(ui->music_preparation_setting_content);
    lv_obj_set_pos(ui->music_preparation_metronment_paihao_input_button_1, 175, 185);
    lv_obj_set_size(ui->music_preparation_metronment_paihao_input_button_1, 192, 55);
    ui->music_preparation_metronment_paihao_input_button_1_label = lv_label_create(ui->music_preparation_metronment_paihao_input_button_1);
    lv_label_set_text(ui->music_preparation_metronment_paihao_input_button_1_label, "麦克风输入");
    lv_label_set_long_mode(ui->music_preparation_metronment_paihao_input_button_1_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->music_preparation_metronment_paihao_input_button_1_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->music_preparation_metronment_paihao_input_button_1, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->music_preparation_metronment_paihao_input_button_1_label, LV_PCT(100));

    //Write style for music_preparation_metronment_paihao_input_button_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->music_preparation_metronment_paihao_input_button_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->music_preparation_metronment_paihao_input_button_1, lv_color_hex(0xEEDEC6), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->music_preparation_metronment_paihao_input_button_1, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->music_preparation_metronment_paihao_input_button_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_metronment_paihao_input_button_1, 5, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_metronment_paihao_input_button_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_metronment_paihao_input_button_1, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_metronment_paihao_input_button_1, &lv_font_SourceHanSansSCBold_26, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_metronment_paihao_input_button_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_metronment_paihao_input_button_1, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_metronment_paihao_input_title
    ui->music_preparation_metronment_paihao_input_title = lv_label_create(ui->music_preparation_setting_content);
    lv_obj_add_flag(ui->music_preparation_metronment_paihao_input_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(ui->music_preparation_metronment_paihao_input_title, 62, 389);
    lv_obj_set_size(ui->music_preparation_metronment_paihao_input_title, 102, 32);
    lv_label_set_text(ui->music_preparation_metronment_paihao_input_title, "输入来源:");
    lv_label_set_long_mode(ui->music_preparation_metronment_paihao_input_title, LV_LABEL_LONG_WRAP);

    //Write style for music_preparation_metronment_paihao_input_title, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->music_preparation_metronment_paihao_input_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_metronment_paihao_input_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_metronment_paihao_input_title, lv_color_hex(0x2C1810), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_metronment_paihao_input_title, &lv_font_SourceHanSansSCBold_22, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_metronment_paihao_input_title, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->music_preparation_metronment_paihao_input_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->music_preparation_metronment_paihao_input_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_metronment_paihao_input_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->music_preparation_metronment_paihao_input_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->music_preparation_metronment_paihao_input_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->music_preparation_metronment_paihao_input_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->music_preparation_metronment_paihao_input_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->music_preparation_metronment_paihao_input_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_metronment_paihao_input_title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_music_type
    ui->music_preparation_music_type = lv_label_create(ui->music_preparation_setting_content);
    lv_obj_set_pos(ui->music_preparation_music_type, 6, 256);
    lv_obj_set_size(ui->music_preparation_music_type, 134, 32);
    lv_label_set_text(ui->music_preparation_music_type, "乐谱类型");
    lv_label_set_long_mode(ui->music_preparation_music_type, LV_LABEL_LONG_WRAP);

    //Write style for music_preparation_music_type, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->music_preparation_music_type, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_music_type, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_music_type, lv_color_hex(0x2C1810), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_music_type, &lv_font_SourceHanSansSCBold_28, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_music_type, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->music_preparation_music_type, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->music_preparation_music_type, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_music_type, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->music_preparation_music_type, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->music_preparation_music_type, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->music_preparation_music_type, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->music_preparation_music_type, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->music_preparation_music_type, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_music_type, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_music_type_button_1
    ui->music_preparation_music_type_button_1 = lv_button_create(ui->music_preparation_setting_content);
    lv_obj_set_pos(ui->music_preparation_music_type_button_1, 175, 250);
    lv_obj_set_size(ui->music_preparation_music_type_button_1, 192, 55);
    ui->music_preparation_music_type_button_1_label = lv_label_create(ui->music_preparation_music_type_button_1);
    lv_label_set_text(ui->music_preparation_music_type_button_1_label, "简谱");
    lv_label_set_long_mode(ui->music_preparation_music_type_button_1_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->music_preparation_music_type_button_1_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->music_preparation_music_type_button_1, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->music_preparation_music_type_button_1_label, LV_PCT(100));

    //Write style for music_preparation_music_type_button_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->music_preparation_music_type_button_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->music_preparation_music_type_button_1, lv_color_hex(0xEEDEC6), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->music_preparation_music_type_button_1, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->music_preparation_music_type_button_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_music_type_button_1, 5, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_music_type_button_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_music_type_button_1, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_music_type_button_1, &lv_font_SourceHanSansSCBold_26, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_music_type_button_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_music_type_button_1, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_music_type_button_2
    ui->music_preparation_music_type_button_2 = lv_button_create(ui->music_preparation_setting_content);
    lv_obj_set_pos(ui->music_preparation_music_type_button_2, 401, 252);
    lv_obj_set_size(ui->music_preparation_music_type_button_2, 192, 55);
    ui->music_preparation_music_type_button_2_label = lv_label_create(ui->music_preparation_music_type_button_2);
    lv_label_set_text(ui->music_preparation_music_type_button_2_label, "五线谱");
    lv_label_set_long_mode(ui->music_preparation_music_type_button_2_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->music_preparation_music_type_button_2_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->music_preparation_music_type_button_2, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->music_preparation_music_type_button_2_label, LV_PCT(100));

    //Write style for music_preparation_music_type_button_2, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->music_preparation_music_type_button_2, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->music_preparation_music_type_button_2, lv_color_hex(0x3f2204), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->music_preparation_music_type_button_2, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->music_preparation_music_type_button_2, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_music_type_button_2, 5, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_music_type_button_2, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_music_type_button_2, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_music_type_button_2, &lv_font_SourceHanSansSCBold_26, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_music_type_button_2, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_music_type_button_2, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_start_button
    ui->music_preparation_start_button = lv_button_create(ui->music_preparation);
    lv_obj_set_pos(ui->music_preparation_start_button, 357, 530);
    lv_obj_set_size(ui->music_preparation_start_button, 287, 54);
    ui->music_preparation_start_button_label = lv_label_create(ui->music_preparation_start_button);
    lv_label_set_text(ui->music_preparation_start_button_label, "开始演奏");
    lv_label_set_long_mode(ui->music_preparation_start_button_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->music_preparation_start_button_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->music_preparation_start_button, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->music_preparation_start_button_label, LV_PCT(100));

    //Write style for music_preparation_start_button, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->music_preparation_start_button, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->music_preparation_start_button, lv_color_hex(0x3f2204), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->music_preparation_start_button, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->music_preparation_start_button, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->music_preparation_start_button, 5, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->music_preparation_start_button, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->music_preparation_start_button, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->music_preparation_start_button, &lv_font_gudianChinese_34, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->music_preparation_start_button, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->music_preparation_start_button, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_img_1
    ui->music_preparation_img_1 = lv_image_create(ui->music_preparation);
    lv_obj_set_pos(ui->music_preparation_img_1, 304, 19);
    lv_obj_set_size(ui->music_preparation_img_1, 122, 55);
    lv_obj_add_flag(ui->music_preparation_img_1, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->music_preparation_img_1, &_huawen_RGB565A8_122x55);
    lv_image_set_pivot(ui->music_preparation_img_1, 50,50);
    lv_image_set_rotation(ui->music_preparation_img_1, 0);

    //Write style for music_preparation_img_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->music_preparation_img_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->music_preparation_img_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes music_preparation_img_2
    ui->music_preparation_img_2 = lv_image_create(ui->music_preparation);
    lv_obj_set_pos(ui->music_preparation_img_2, 591, 20);
    lv_obj_set_size(ui->music_preparation_img_2, 113, 52);
    lv_obj_add_flag(ui->music_preparation_img_2, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->music_preparation_img_2, &_huawen2_RGB565A8_113x52);
    lv_image_set_pivot(ui->music_preparation_img_2, 50,50);
    lv_image_set_rotation(ui->music_preparation_img_2, 0);

    //Write style for music_preparation_img_2, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->music_preparation_img_2, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->music_preparation_img_2, 255, LV_PART_MAIN|LV_STATE_DEFAULT);

    //The custom code of music_preparation.


    //Update current screen layout.
    lv_obj_update_layout(ui->music_preparation);

}
