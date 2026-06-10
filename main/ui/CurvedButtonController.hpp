#pragma once

#include <stdint.h>

#include "lvgl.h"
#include "RadarTypes.hpp"

class RadarApp;

/*
 * Creates and maintains the curved bezel buttons.
 *
 * The round display leaves very little straight-edged space. This controller
 * splits button text into individual labels and positions them along an arc,
 * while a normal rectangular hitbox catches touch events.
 */
class CurvedButtonController {
public:
    /* Attach the owning application used for fonts, colours, and settings. */
    void bind(RadarApp *app) { owner = app; }

    /* Recalculate character positions after text or geometry changes. */
    static void positionText(curved_button_t *button);

    /* Update the stored label and refresh curved characters. */
    static void setText(curved_button_t *button, const char *text);

    /* Apply a new text colour to all labels owned by the button. */
    static void setColor(curved_button_t *button, uint32_t color);

    /*
     * Create a button.
     *
     * start_deg and end_deg describe the visual arc. x/y/w/h describe the
     * practical LVGL hit target, which intentionally does not have to be curved.
     */
    void create(lv_obj_t *parent, curved_button_t *button, int start_deg, int end_deg,
                int x, int y, int w, int h, const char *text, lv_event_cb_t cb,
                uint32_t text_color, bool top_text = false);

private:
    RadarApp *owner = nullptr;
};
