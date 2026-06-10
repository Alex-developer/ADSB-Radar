#include "AircraftPopupController.hpp"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "misc/cache/instance/lv_image_cache.h"

#include "RadarApp.hpp"

/*
 * Popup logic is kept outside RadarApp so hit-testing, detail formatting, and
 * photo lifetime are easier to reason about. It still uses RadarApp's prebuilt
 * labels and aircraft snapshot; no extra aircraft list is allocated here.
 */

/* Find the nearest plotted aircraft to a touch point within the hit radius. */
int AircraftPopupController::findAircraftAtPoint(int x, int y) const
{
    if (!owner) {
        return -1;
    }

    const int hit_radius_px = 24;
    int best_index = -1;
    int best_distance_sq = hit_radius_px * hit_radius_px;

    /*
     * The user taps a small round screen with a finger, so hit-testing is based
     * on a generous radius and chooses the nearest aircraft inside that radius.
     */
    for (size_t i = 0; i < owner->ui_aircraft_count && i < MAX_AIRCRAFT_TARGETS; ++i) {
        int aircraft_x;
        int aircraft_y;
        if (!RadarApp::project_aircraft_to_radar(&owner->ui_aircraft_snapshot[i],
                                                 owner->ui_aircraft_range_mi,
                                                 &aircraft_x, &aircraft_y)) {
            continue;
        }

        int dx = x - aircraft_x;
        int dy = y - aircraft_y;
        int distance_sq = (dx * dx) + (dy * dy);
        if (distance_sq <= best_distance_sq) {
            best_distance_sq = distance_sq;
            best_index = (int)i;
        }
    }

    return best_index;
}

/* Return a non-empty display value, or "-" when the source field is blank. */
const char *AircraftPopupController::valueOrDash(const char *value)
{
    return value && value[0] != '\0' ? value : "-";
}

/* Format altitude for the popup, using flight levels at high altitude. */
void AircraftPopupController::formatAltitude(char *dst, size_t dst_size, int altitude_ft)
{
    if (altitude_ft == INT_MIN) {
        snprintf(dst, dst_size, "ALT ---");
    } else if (altitude_ft == 0) {
        snprintf(dst, dst_size, "ALT GND");
    } else if (altitude_ft >= 18000) {
        snprintf(dst, dst_size, "FL %03d", (altitude_ft + 50) / 100);
    } else {
        snprintf(dst, dst_size, "ALT %d", altitude_ft);
    }
}

/* Show or hide the aircraft photo status label. */
void AircraftPopupController::setPhotoStatusText(const char *text)
{
    if (!owner || !owner->aircraft_photo_status) {
        return;
    }

    lv_label_set_text(owner->aircraft_photo_status, text ? text : "");
    if (text && text[0] != '\0') {
        lv_obj_clear_flag(owner->aircraft_photo_status, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(owner->aircraft_photo_status, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Release any decoded photo currently installed in the popup. */
void AircraftPopupController::releasePhotoImage()
{
    if (!owner) {
        return;
    }

    /*
     * LVGL keeps only a pointer to the image descriptor data. Clear the image
     * source before freeing the backing pixel buffer.
     */
    if (owner->aircraft_photo_image) {
        lv_image_set_src(owner->aircraft_photo_image, NULL);
        lv_obj_add_flag(owner->aircraft_photo_image, LV_OBJ_FLAG_HIDDEN);
    }
    lv_image_cache_drop(&owner->aircraft_photo_dsc);
    if (owner->aircraft_photo_pixels) {
        heap_caps_free(owner->aircraft_photo_pixels);
        owner->aircraft_photo_pixels = NULL;
    }
    owner->aircraft_photo_pixel_size = 0;
    memset(&owner->aircraft_photo_dsc, 0, sizeof(owner->aircraft_photo_dsc));
}

/* Hide the popup and invalidate any outstanding photo request. */
void AircraftPopupController::hide()
{
    if (owner && owner->aircraft_popup) {
        owner->aircraft_photo_request_id++;
        releasePhotoImage();
        lv_obj_add_flag(owner->aircraft_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Populate and position the popup for one aircraft, then start photo loading. */
void AircraftPopupController::show(const aircraft_data_t *aircraft, int screen_x, int screen_y)
{
    if (!owner || !owner->aircraft_popup || !owner->aircraft_popup_title ||
        !owner->aircraft_popup_body || !aircraft) {
        return;
    }

    releasePhotoImage();
    setPhotoStatusText("PHOTO FETCH");

    char title[32];
    snprintf(title, sizeof(title), "%s", aircraft->callsign);
    lv_label_set_text(owner->aircraft_popup_title, title);

    char altitude_text[16];
    char heading_text[16];
    char vertical_text[20];
    formatAltitude(altitude_text, sizeof(altitude_text), aircraft->altitude_ft);
    if (aircraft->heading_deg < 0) {
        snprintf(heading_text, sizeof(heading_text), "HDG ---");
    } else {
        snprintf(heading_text, sizeof(heading_text), "HDG %03d", aircraft->heading_deg);
    }
    if (aircraft->vertical_rate_fpm == INT_MIN) {
        snprintf(vertical_text, sizeof(vertical_text), "VR ---");
    } else {
        snprintf(vertical_text, sizeof(vertical_text), "VR %+d", aircraft->vertical_rate_fpm);
    }

    char body[256];
    snprintf(body, sizeof(body),
             "ICAO %s  REG %s\n"
             "TYPE %s  SQ %s\n"
             "%s  SPD %d\n"
             "%s  BRG %03d\n"
             "RNG %.1f MI  SEEN %.1fs\n"
             "%s",
             valueOrDash(aircraft->icao),
             valueOrDash(aircraft->registration),
             valueOrDash(aircraft->type),
             valueOrDash(aircraft->squawk),
             altitude_text,
             aircraft->speed_kt,
             heading_text,
             aircraft->bearing_deg,
             aircraft->distance_mi,
             aircraft->seen_s,
             vertical_text);
    lv_label_set_text(owner->aircraft_popup_body, body);

    const int popup_w = AIRCRAFT_POPUP_W;
    const int popup_h = AIRCRAFT_POPUP_H;
    int x = screen_x + 14;
    int y = screen_y + 14;
    if (x > SCREEN_W - popup_w - 34) {
        x = screen_x - popup_w - 14;
    }
    if (y > SCREEN_H - popup_h - 88) {
        y = screen_y - popup_h - 14;
    }
    x = RadarApp::clamp_int(x, 34, SCREEN_W - popup_w - 34);
    y = RadarApp::clamp_int(y, 54, SCREEN_H - popup_h - 92);
    lv_obj_set_pos(owner->aircraft_popup, x, y);
    lv_obj_clear_flag(owner->aircraft_popup, LV_OBJ_FLAG_HIDDEN);
    owner->start_aircraft_photo_fetch(aircraft->icao);
}

/* Handle taps on the radar touch layer and show or hide the popup. */
void AircraftPopupController::handleTouchEvent(lv_event_t *event)
{
    if (!owner || lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    lv_obj_t *target = lv_event_get_target_obj(event);
    lv_area_t coords;
    lv_obj_get_coords(target, &coords);
    int local_x = point.x - coords.x1;
    int local_y = point.y - coords.y1;

    int index = findAircraftAtPoint(local_x, local_y);
    if (index < 0) {
        hide();
        return;
    }

    show(&owner->ui_aircraft_snapshot[index], point.x, point.y);
}

/* Create the transparent touch layer above the radar plot. */
void AircraftPopupController::createTouchLayer(lv_obj_t *radar)
{
    lv_obj_t *touch_layer = lv_obj_create(radar);
    lv_obj_set_size(touch_layer, RADAR_SIZE, RADAR_SIZE);
    lv_obj_set_pos(touch_layer, 0, 0);
    lv_obj_set_style_bg_opa(touch_layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(touch_layer, 0, 0);
    lv_obj_set_style_pad_all(touch_layer, 0, 0);
    lv_obj_clear_flag(touch_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(touch_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(touch_layer, RadarApp::radar_touch_event_entry, LV_EVENT_CLICKED, NULL);
}

/* Handle the popup close button event. */
void AircraftPopupController::closeEvent(lv_event_t *event)
{
    (void)event;
    hide();
}

/* Create the popup container, close button, photo widget, and detail labels. */
void AircraftPopupController::create(lv_obj_t *screen)
{
    if (!owner) {
        return;
    }

    owner->aircraft_popup = lv_obj_create(screen);
    lv_obj_set_size(owner->aircraft_popup, AIRCRAFT_POPUP_W, AIRCRAFT_POPUP_H);
    lv_obj_set_pos(owner->aircraft_popup, 200, 206);
    lv_obj_set_style_bg_color(owner->aircraft_popup, lv_color_hex(owner->settings.colors.popup_bg), 0);
    lv_obj_set_style_bg_opa(owner->aircraft_popup, LV_OPA_90, 0);
    lv_obj_set_style_border_color(owner->aircraft_popup, lv_color_hex(owner->settings.colors.popup_border), 0);
    lv_obj_set_style_border_opa(owner->aircraft_popup, LV_OPA_70, 0);
    lv_obj_set_style_border_width(owner->aircraft_popup, 1, 0);
    lv_obj_set_style_radius(owner->aircraft_popup, 8, 0);
    lv_obj_set_style_pad_all(owner->aircraft_popup, 10, 0);
    lv_obj_clear_flag(owner->aircraft_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(owner->aircraft_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(owner->aircraft_popup, LV_OBJ_FLAG_HIDDEN);

    owner->aircraft_popup_title = RadarApp::make_label(owner->aircraft_popup, "", &lv_font_montserrat_18,
                                                       owner->settings.colors.button_text);
    lv_obj_set_width(owner->aircraft_popup_title, 250);
    lv_label_set_long_mode(owner->aircraft_popup_title, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(owner->aircraft_popup_title, 10, 8);

    lv_obj_t *close = lv_obj_create(owner->aircraft_popup);
    lv_obj_set_size(close, 34, 30);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -6, 5);
    lv_obj_set_style_bg_color(close, lv_color_hex(owner->settings.colors.button_pressed), 0);
    lv_obj_set_style_bg_opa(close, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(close, lv_color_hex(owner->settings.colors.popup_border), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(close, LV_OPA_90, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(close, 0, 0);
    lv_obj_set_style_radius(close, 8, 0);
    lv_obj_set_style_pad_all(close, 0, 0);
    lv_obj_clear_flag(close, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(close, RadarApp::aircraft_popup_close_event_entry, LV_EVENT_CLICKED, NULL);

    lv_obj_t *close_label = RadarApp::make_label(close, "X", &lv_font_montserrat_16,
                                                 owner->settings.colors.button_text);
    lv_obj_center(close_label);

    owner->aircraft_photo_image = lv_image_create(owner->aircraft_popup);
    lv_obj_set_pos(owner->aircraft_photo_image,
                   (AIRCRAFT_POPUP_W - AIRCRAFT_PHOTO_W) / 2,
                   40);
    lv_obj_add_flag(owner->aircraft_photo_image, LV_OBJ_FLAG_HIDDEN);

    owner->aircraft_photo_status = RadarApp::make_label(owner->aircraft_popup, "",
                                                        &lv_font_montserrat_12,
                                                        owner->settings.colors.text_secondary);
    lv_obj_set_size(owner->aircraft_photo_status, AIRCRAFT_PHOTO_W, 60);
    lv_label_set_long_mode(owner->aircraft_photo_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(owner->aircraft_photo_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(owner->aircraft_photo_status, 2, 0);
    lv_obj_set_pos(owner->aircraft_photo_status,
                   (AIRCRAFT_POPUP_W - AIRCRAFT_PHOTO_W) / 2,
                   78);
    lv_obj_add_flag(owner->aircraft_photo_status, LV_OBJ_FLAG_HIDDEN);

    owner->aircraft_popup_body = RadarApp::make_label(owner->aircraft_popup, "",
                                                      &lv_font_montserrat_12,
                                                      owner->settings.colors.text_primary);
    lv_obj_set_width(owner->aircraft_popup_body, AIRCRAFT_POPUP_W - 20);
    lv_label_set_long_mode(owner->aircraft_popup_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(owner->aircraft_popup_body, 2, 0);
    lv_obj_set_pos(owner->aircraft_popup_body, 10, 158);
}
