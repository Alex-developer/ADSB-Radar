#include "CurvedButtonController.hpp"

#include <math.h>
#include <string.h>
#include <stdio.h>

#include "RadarApp.hpp"

/*
 * Curved buttons are visually drawn as text along the radar bezel, but the
 * actual touch target is a normal LVGL object. This keeps touch handling simple
 * while letting the labels follow the round display.
 */

/* Position each character label along the button's configured arc. */
void CurvedButtonController::positionText(curved_button_t *button)
{
    size_t len = strlen(button->text);
    if (len > CONTROL_TEXT_MAX) {
        len = CONTROL_TEXT_MAX;
    }

    for (size_t i = 0; i < CONTROL_TEXT_MAX; ++i) {
        if (!button->chars[i]) {
            continue;
        }

        if (i >= len) {
            lv_obj_add_flag(button->chars[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        float span = (float)(button->end_deg - button->start_deg);
        if (span < 0.0f) {
            span += 360.0f;
        }
        float center_deg = (float)button->start_deg + (span * 0.5f);
        float usable_span = span > 12.0f ? span - 12.0f : span;
        /*
         * Use a fixed preferred spacing, then compress if the text would spill
         * beyond the button's arc. This avoids the wide, dotted look caused by
         * spreading short labels across the full arc.
         */
        float step_deg = len > 1 ? 2.25f : 0.0f;
        float text_span = step_deg * (float)(len - 1);
        if (len > 1 && text_span > usable_span) {
            step_deg = usable_span / (float)(len - 1);
        }

        float offset = step_deg * (float)(len - 1) * 0.5f;
        float degrees = button->top_text ?
                        (center_deg - offset + (step_deg * (float)i)) :
                        (center_deg + offset - (step_deg * (float)i));
        float radians = (degrees - 90.0f) * PI_F / 180.0f;
        int x = (SCREEN_W / 2) + (int)lroundf(cosf(radians) * (float)button->text_radius);
        int y = (SCREEN_H / 2) + (int)lroundf(sinf(radians) * (float)button->text_radius);

        char glyph[2] = {button->text[i], '\0'};
        lv_label_set_text(button->chars[i], glyph);
        lv_obj_set_pos(button->chars[i], x - 8, y - 10);
        float rotation = button->top_text ? degrees : (degrees - 180.0f);
        while (rotation > 180.0f) {
            rotation -= 360.0f;
        }
        while (rotation < -180.0f) {
            rotation += 360.0f;
        }
        lv_obj_set_style_transform_rotation(button->chars[i], (int32_t)lroundf(rotation * 10.0f), 0);
        lv_obj_clear_flag(button->chars[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* Update the button caption and refresh either flat or curved text. */
void CurvedButtonController::setText(curved_button_t *button, const char *text)
{
    if (!button) {
        return;
    }

    if (strncmp(button->text, text, sizeof(button->text)) == 0) {
        return;
    }
    snprintf(button->text, sizeof(button->text), "%s", text);
    if (button->label) {
        lv_label_set_text(button->label, button->text);
        return;
    }
    if (button->chars[0]) {
        positionText(button);
    }
}

/* Apply a text colour to every label owned by a curved button. */
void CurvedButtonController::setColor(curved_button_t *button, uint32_t color)
{
    if (!button) {
        return;
    }

    button->text_color = color;
    if (button->label) {
        lv_obj_set_style_text_color(button->label, lv_color_hex(color), 0);
    }
    for (size_t i = 0; i < CONTROL_TEXT_MAX; ++i) {
        if (button->chars[i]) {
            lv_obj_set_style_text_color(button->chars[i], lv_color_hex(color), 0);
        }
    }
}

/* Create a curved visual button with a simple LVGL touch hitbox. */
void CurvedButtonController::create(lv_obj_t *parent, curved_button_t *button,
                                    int start_deg, int end_deg,
                                    int x, int y, int w, int h,
                                    const char *text, lv_event_cb_t cb,
                                    uint32_t text_color, bool top_text)
{
    if (!owner || !button) {
        return;
    }

    memset(button, 0, sizeof(*button));
    button->start_deg = start_deg;
    button->end_deg = end_deg;
    button->text_radius = CONTROL_ARC_RADIUS + 1;
    button->text_color = text_color;
    button->top_text = top_text;

    if (!top_text) {
        for (size_t i = 0; i < CONTROL_TEXT_MAX; ++i) {
            button->chars[i] = RadarApp::make_label(parent, "",
                                                    RadarApp::configured_label_font(owner->settings.label_styles.button),
                                                    text_color);
            lv_obj_set_size(button->chars[i], 16, 20);
            lv_label_set_long_mode(button->chars[i], LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(button->chars[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_transform_pivot_x(button->chars[i], 8, 0);
            lv_obj_set_style_transform_pivot_y(button->chars[i], 10, 0);
            lv_obj_clear_flag(button->chars[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(button->chars[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    button->hitbox = lv_obj_create(parent);
    lv_obj_set_size(button->hitbox, w, h);
    lv_obj_set_pos(button->hitbox, x, y);
    lv_obj_set_style_bg_opa(button->hitbox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(button->hitbox, lv_color_hex(owner->settings.colors.button_pressed),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button->hitbox, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_radius(button->hitbox, h / 2, 0);
    lv_obj_set_style_border_width(button->hitbox, 0, 0);
    lv_obj_set_style_pad_all(button->hitbox, 0, 0);
    lv_obj_clear_flag(button->hitbox, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) {
        lv_obj_add_flag(button->hitbox, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(button->hitbox, cb, LV_EVENT_CLICKED, NULL);
    }

    if (top_text) {
        button->label = RadarApp::make_label(button->hitbox, "",
                                             RadarApp::configured_label_font(owner->settings.label_styles.button, -2),
                                             text_color);
        lv_obj_set_width(button->label, w - 20);
        lv_label_set_long_mode(button->label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(button->label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_opa(button->label, LV_OPA_70, 0);
        lv_obj_center(button->label);
        lv_obj_clear_flag(button->label, LV_OBJ_FLAG_CLICKABLE);
    }

    setText(button, text);
}
