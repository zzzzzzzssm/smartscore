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



void setup_scr_end_screen(lv_ui *ui)
{
    //Write codes end_screen
    ui->end_screen = lv_obj_create(NULL);
    lv_obj_set_size(ui->end_screen, 1024, 600);
    lv_obj_set_scrollbar_mode(ui->end_screen, LV_SCROLLBAR_MODE_OFF);

    //Write style for end_screen, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->end_screen, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes end_screen_background
    ui->end_screen_background = lv_image_create(ui->end_screen);
    lv_obj_set_pos(ui->end_screen_background, 0, 0);
    lv_obj_set_size(ui->end_screen_background, 1024, 600);
    lv_obj_add_flag(ui->end_screen_background, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->end_screen_background, &_endScreenBackground_resized_resized_RGB565A8_1024x600);
    lv_image_set_pivot(ui->end_screen_background, 50,50);
    lv_image_set_rotation(ui->end_screen_background, 0);

    //Write style for end_screen_background, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->end_screen_background, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->end_screen_background, 255, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes end_screen_completionScore
    ui->end_screen_completionScore = lv_label_create(ui->end_screen);
    lv_obj_set_pos(ui->end_screen_completionScore, 371, 382);
    lv_obj_set_size(ui->end_screen_completionScore, 449, 35);
    lv_label_set_text(ui->end_screen_completionScore, "███████████████████████░░░░░");
    lv_label_set_long_mode(ui->end_screen_completionScore, LV_LABEL_LONG_WRAP);

    //Write style for end_screen_completionScore, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->end_screen_completionScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->end_screen_completionScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->end_screen_completionScore, lv_color_hex(0x000000), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->end_screen_completionScore, &lv_font_unscii_32, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->end_screen_completionScore, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->end_screen_completionScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->end_screen_completionScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->end_screen_completionScore, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->end_screen_completionScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->end_screen_completionScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->end_screen_completionScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->end_screen_completionScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->end_screen_completionScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->end_screen_completionScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes end_screen_wanzhengdu_text
    ui->end_screen_wanzhengdu_text = lv_label_create(ui->end_screen);
    lv_obj_set_pos(ui->end_screen_wanzhengdu_text, 237, 376);
    lv_obj_set_size(ui->end_screen_wanzhengdu_text, 127, 38);
    lv_label_set_text(ui->end_screen_wanzhengdu_text, "完整度:");
    lv_label_set_long_mode(ui->end_screen_wanzhengdu_text, LV_LABEL_LONG_WRAP);

    //Write style for end_screen_wanzhengdu_text, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->end_screen_wanzhengdu_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->end_screen_wanzhengdu_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->end_screen_wanzhengdu_text, lv_color_hex(0x000000), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->end_screen_wanzhengdu_text, &lv_font_gudianChinese_34, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->end_screen_wanzhengdu_text, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->end_screen_wanzhengdu_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->end_screen_wanzhengdu_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->end_screen_wanzhengdu_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->end_screen_wanzhengdu_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->end_screen_wanzhengdu_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->end_screen_wanzhengdu_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->end_screen_wanzhengdu_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->end_screen_wanzhengdu_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->end_screen_wanzhengdu_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes end_screen_xuanlvScore
    ui->end_screen_xuanlvScore = lv_label_create(ui->end_screen);
    lv_obj_set_pos(ui->end_screen_xuanlvScore, 371, 312);
    lv_obj_set_size(ui->end_screen_xuanlvScore, 449, 35);
    lv_label_set_text(ui->end_screen_xuanlvScore, "███████████████████████░░░░░");
    lv_label_set_long_mode(ui->end_screen_xuanlvScore, LV_LABEL_LONG_WRAP);

    //Write style for end_screen_xuanlvScore, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->end_screen_xuanlvScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->end_screen_xuanlvScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->end_screen_xuanlvScore, lv_color_hex(0x000000), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->end_screen_xuanlvScore, &lv_font_unscii_32, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->end_screen_xuanlvScore, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->end_screen_xuanlvScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->end_screen_xuanlvScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->end_screen_xuanlvScore, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->end_screen_xuanlvScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->end_screen_xuanlvScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->end_screen_xuanlvScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->end_screen_xuanlvScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->end_screen_xuanlvScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->end_screen_xuanlvScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes end_screen_yinzhunScore
    ui->end_screen_yinzhunScore = lv_label_create(ui->end_screen);
    lv_obj_set_pos(ui->end_screen_yinzhunScore, 371, 252);
    lv_obj_set_size(ui->end_screen_yinzhunScore, 449, 35);
    lv_label_set_text(ui->end_screen_yinzhunScore, "███████████████████████░░░░░");
    lv_label_set_long_mode(ui->end_screen_yinzhunScore, LV_LABEL_LONG_WRAP);

    //Write style for end_screen_yinzhunScore, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->end_screen_yinzhunScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->end_screen_yinzhunScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->end_screen_yinzhunScore, lv_color_hex(0x000000), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->end_screen_yinzhunScore, &lv_font_unscii_32, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->end_screen_yinzhunScore, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->end_screen_yinzhunScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->end_screen_yinzhunScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->end_screen_yinzhunScore, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->end_screen_yinzhunScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->end_screen_yinzhunScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->end_screen_yinzhunScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->end_screen_yinzhunScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->end_screen_yinzhunScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->end_screen_yinzhunScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes end_screen_jiezou_text
    ui->end_screen_jiezou_text = lv_label_create(ui->end_screen);
    lv_obj_set_pos(ui->end_screen_jiezou_text, 237, 312);
    lv_obj_set_size(ui->end_screen_jiezou_text, 127, 38);
    lv_label_set_text(ui->end_screen_jiezou_text, "节奏:");
    lv_label_set_long_mode(ui->end_screen_jiezou_text, LV_LABEL_LONG_WRAP);

    //Write style for end_screen_jiezou_text, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->end_screen_jiezou_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->end_screen_jiezou_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->end_screen_jiezou_text, lv_color_hex(0x000000), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->end_screen_jiezou_text, &lv_font_gudianChinese_34, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->end_screen_jiezou_text, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->end_screen_jiezou_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->end_screen_jiezou_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->end_screen_jiezou_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->end_screen_jiezou_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->end_screen_jiezou_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->end_screen_jiezou_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->end_screen_jiezou_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->end_screen_jiezou_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->end_screen_jiezou_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes end_screen_yinzhun_text
    ui->end_screen_yinzhun_text = lv_label_create(ui->end_screen);
    lv_obj_set_pos(ui->end_screen_yinzhun_text, 237, 249);
    lv_obj_set_size(ui->end_screen_yinzhun_text, 127, 38);
    lv_label_set_text(ui->end_screen_yinzhun_text, "音准:");
    lv_label_set_long_mode(ui->end_screen_yinzhun_text, LV_LABEL_LONG_WRAP);

    //Write style for end_screen_yinzhun_text, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->end_screen_yinzhun_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->end_screen_yinzhun_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->end_screen_yinzhun_text, lv_color_hex(0x000000), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->end_screen_yinzhun_text, &lv_font_gudianChinese_34, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->end_screen_yinzhun_text, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->end_screen_yinzhun_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->end_screen_yinzhun_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->end_screen_yinzhun_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->end_screen_yinzhun_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->end_screen_yinzhun_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->end_screen_yinzhun_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->end_screen_yinzhun_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->end_screen_yinzhun_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->end_screen_yinzhun_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes end_screen_comprehensiveScore
    ui->end_screen_comprehensiveScore = lv_label_create(ui->end_screen);
    lv_obj_set_pos(ui->end_screen_comprehensiveScore, 361, 165);
    lv_obj_set_size(ui->end_screen_comprehensiveScore, 301, 56);
    lv_label_set_text(ui->end_screen_comprehensiveScore, "综合评分:--");
    lv_label_set_long_mode(ui->end_screen_comprehensiveScore, LV_LABEL_LONG_WRAP);

    //Write style for end_screen_comprehensiveScore, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->end_screen_comprehensiveScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->end_screen_comprehensiveScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->end_screen_comprehensiveScore, lv_color_hex(0x000000), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->end_screen_comprehensiveScore, &lv_font_gudianChinese_48, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->end_screen_comprehensiveScore, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->end_screen_comprehensiveScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->end_screen_comprehensiveScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->end_screen_comprehensiveScore, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->end_screen_comprehensiveScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->end_screen_comprehensiveScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->end_screen_comprehensiveScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->end_screen_comprehensiveScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->end_screen_comprehensiveScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->end_screen_comprehensiveScore, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //The custom code of end_screen.


    //Update current screen layout.
    lv_obj_update_layout(ui->end_screen);

}
