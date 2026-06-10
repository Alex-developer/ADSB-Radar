#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "RadarTypes.hpp"

class RadarApp;

/*
 * Touch hit-testing and popup UI for aircraft details.
 *
 * The radar canvas is not itself a rich widget, so a transparent LVGL hit layer
 * routes touches here. The controller finds the nearest plotted aircraft,
 * positions the popup, and coordinates photo image lifetime.
 */
class AircraftPopupController {
public:
    /* Attach the owning application that provides aircraft snapshots and widgets. */
    void bind(RadarApp *app) { owner = app; }

    /* Return the index of the aircraft nearest to a screen point, or -1. */
    int findAircraftAtPoint(int x, int y) const;

    /* Return a displayable value, replacing empty strings with "-". */
    static const char *valueOrDash(const char *value);

    /* Format altitude for the popup body. */
    static void formatAltitude(char *dst, size_t dst_size, int altitude_ft);

    /* Show or hide the photo status label. */
    void setPhotoStatusText(const char *text);

    /* Release any installed photo pixels owned by the popup. */
    void releasePhotoImage();

    /* Hide the popup and invalidate any outstanding photo request. */
    void hide();

    /* Show details for one aircraft at the requested screen point. */
    void show(const aircraft_data_t *aircraft, int screen_x, int screen_y);

    /* LVGL touch callback body for the radar hit layer. */
    void handleTouchEvent(lv_event_t *event);

    /* Create the transparent hit layer above the radar. */
    void createTouchLayer(lv_obj_t *radar);

    /* LVGL close-button callback body. */
    void closeEvent(lv_event_t *event);

    /* Create the popup container and static child widgets. */
    void create(lv_obj_t *screen);

private:
    RadarApp *owner = nullptr;
};
