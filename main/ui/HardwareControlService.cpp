#include "HardwareControlService.hpp"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "iot_knob.h"

#include "RadarApp.hpp"
#include "RadarSettings.hpp"

static const char *TAG = "hardware_controls";

static constexpr int64_t BUTTON_DEBOUNCE_US = 35000;

enum {
    HW_MENU_RANGE = 0,
    HW_MENU_FILTER,
    HW_MENU_HEADING,
    HW_MENU_AIRPORTS,
    HW_MENU_COUNTRIES,
    HW_MENU_RUNWAYS,
    HW_MENU_GROUND,
    HW_MENU_SWEEP,
    HW_MENU_WIFI_SETUP,
    HW_MENU_REBOOT,
    HW_MENU_ITEM_COUNT,
};

void HardwareControlService::bind(RadarApp *app)
{
    owner = app;
}

void HardwareControlService::knobLeftEntry(void *knob, void *user_data)
{
    (void)knob;
    HardwareControlService *self = static_cast<HardwareControlService *>(user_data);
    if (self) {
        self->encoder_pending_delta = self->encoder_pending_delta - 1;
    }
}

void HardwareControlService::knobRightEntry(void *knob, void *user_data)
{
    (void)knob;
    HardwareControlService *self = static_cast<HardwareControlService *>(user_data);
    if (self) {
        self->encoder_pending_delta = self->encoder_pending_delta + 1;
    }
}

void HardwareControlService::timerEntry(lv_timer_t *timer)
{
    HardwareControlService *self = timer ? static_cast<HardwareControlService *>(lv_timer_get_user_data(timer)) : nullptr;
    if (self) {
        self->timerTick();
    }
}

gpio_num_t HardwareControlService::gpioFromSetting(int value) const
{
    if (value < 0 || value > 63) {
        return GPIO_NUM_NC;
    }
    return (gpio_num_t)value;
}

void HardwareControlService::initInputs()
{
    if (!owner) {
        return;
    }

    if (rotary_knob_handle) {
        iot_knob_delete((knob_handle_t)rotary_knob_handle);
        rotary_knob_handle = nullptr;
    }

    confirm_gpio = gpioFromSetting(owner->settings.hardware_confirm_gpio);
    back_gpio = gpioFromSetting(owner->settings.hardware_back_gpio);
    rotary_a_gpio = gpioFromSetting(owner->settings.hardware_rotary_a_gpio);
    rotary_b_gpio = gpioFromSetting(owner->settings.hardware_rotary_b_gpio);
    rotary_push_gpio = gpioFromSetting(owner->settings.hardware_rotary_push_gpio);

    const uint64_t pin_mask = (1ULL << confirm_gpio) |
                              (1ULL << back_gpio) |
                              (1ULL << rotary_push_gpio);
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = pin_mask;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Button GPIO setup failed: %s", esp_err_to_name(err));
    }

    knob_config_t knob_cfg = {};
    knob_cfg.default_direction = 0;
    knob_cfg.gpio_encoder_a = rotary_a_gpio;
    knob_cfg.gpio_encoder_b = rotary_b_gpio;
    knob_cfg.enable_power_save = false;
    rotary_knob_handle = iot_knob_create(&knob_cfg);
    if (rotary_knob_handle) {
        iot_knob_register_cb((knob_handle_t)rotary_knob_handle, KNOB_LEFT,
                             HardwareControlService::knobLeftEntry, this);
        iot_knob_register_cb((knob_handle_t)rotary_knob_handle, KNOB_RIGHT,
                             HardwareControlService::knobRightEntry, this);
    } else {
        ESP_LOGW(TAG, "Rotary knob driver setup failed");
    }

    encoder_pending_delta = 0;
    encoder_event_accum = 0;
    confirm_last_level = gpio_get_level(confirm_gpio);
    back_last_level = gpio_get_level(back_gpio);
    push_last_level = gpio_get_level(rotary_push_gpio);
    confirm_last_change_us = back_last_change_us = push_last_change_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Rotary encoder configured: CLK=%d DATA=%d PUSH=%d initial=%d/%d/%d",
             rotary_a_gpio, rotary_b_gpio, rotary_push_gpio,
             gpio_get_level(rotary_a_gpio), gpio_get_level(rotary_b_gpio), gpio_get_level(rotary_push_gpio));
    ESP_LOGI(TAG, "Hardware buttons configured: confirm=%d back=%d initial=%d/%d",
             confirm_gpio, back_gpio, confirm_last_level, back_last_level);
}

void HardwareControlService::createMenu(lv_obj_t *screen)
{
    if (!owner || !screen) {
        return;
    }

    menu = lv_obj_create(screen);
    lv_obj_set_size(menu, 440, 560);
    lv_obj_set_pos(menu, (SCREEN_W - 440) / 2, 72);
    lv_obj_set_style_bg_color(menu, lv_color_hex(owner->settings.colors.popup_bg), 0);
    lv_obj_set_style_bg_opa(menu, LV_OPA_90, 0);
    lv_obj_set_style_border_color(menu, lv_color_hex(owner->settings.colors.popup_border), 0);
    lv_obj_set_style_border_width(menu, 2, 0);
    lv_obj_set_style_radius(menu, 10, 0);
    lv_obj_set_style_pad_all(menu, 16, 0);
    lv_obj_clear_flag(menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(menu, LV_OBJ_FLAG_HIDDEN);

    title = owner->make_label(menu, "DISPLAY MENU",
                              owner->configured_label_font(owner->settings.label_styles.button),
                              owner->settings.colors.button_text);
    lv_obj_set_pos(title, 0, 8);
    lv_obj_set_width(title, 408);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    for (int i = 0; i < HW_MENU_ITEM_COUNT; ++i) {
        lv_obj_t *row = lv_obj_create(menu);
        lv_obj_set_size(row, 392, 38);
        lv_obj_set_pos(row, 8, 50 + (i * 42));
        lv_obj_set_style_bg_color(row, lv_color_hex(owner->settings.colors.button_pressed), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_20, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(owner->settings.colors.popup_border), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        labels[i] = owner->make_label(row, "",
                                      owner->configured_label_font(owner->settings.label_styles.button, -2),
                                      owner->settings.colors.text_primary);
        lv_obj_set_pos(labels[i], 12, 8);
        lv_obj_set_width(labels[i], 210);

        values[i] = owner->make_label(row, "",
                                      owner->configured_label_font(owner->settings.label_styles.button, -2),
                                      owner->settings.colors.button_status);
        lv_obj_set_pos(values[i], 226, 8);
        lv_obj_set_width(values[i], 150);
        lv_obj_set_style_text_align(values[i], LV_TEXT_ALIGN_RIGHT, 0);

        rows[i] = row;
    }

    help = owner->make_label(menu, "",
                             owner->configured_label_font(owner->settings.label_styles.gps, -2),
                             owner->settings.colors.text_secondary);
    lv_obj_set_pos(help, 8, 502);
    lv_obj_set_width(help, 392);
    lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_CENTER, 0);
    refreshMenu();
}

void HardwareControlService::refreshFonts()
{
    if (!owner) {
        return;
    }
    owner->apply_label_font(title, owner->settings.label_styles.button);
    for (int i = 0; i < HW_MENU_ITEM_COUNT; ++i) {
        owner->apply_label_font(labels[i], owner->settings.label_styles.button, -2);
        owner->apply_label_font(values[i], owner->settings.label_styles.button, -2);
    }
    owner->apply_label_font(help, owner->settings.label_styles.gps, -2);
}

void HardwareControlService::refreshColors()
{
    if (!owner) {
        return;
    }
    if (menu) {
        lv_obj_set_style_bg_color(menu, lv_color_hex(owner->settings.colors.popup_bg), 0);
        lv_obj_set_style_border_color(menu, lv_color_hex(owner->settings.colors.popup_border), 0);
    }
    if (title) {
        lv_obj_set_style_text_color(title, lv_color_hex(owner->settings.colors.button_text), 0);
    }
    for (int i = 0; i < HW_MENU_ITEM_COUNT; ++i) {
        if (rows[i]) {
            lv_obj_set_style_bg_color(rows[i], lv_color_hex(owner->settings.colors.button_pressed), 0);
            lv_obj_set_style_border_color(rows[i], lv_color_hex(owner->settings.colors.popup_border), 0);
        }
        if (labels[i]) {
            lv_obj_set_style_text_color(labels[i], lv_color_hex(owner->settings.colors.text_primary), 0);
        }
        if (values[i]) {
            lv_obj_set_style_text_color(values[i], lv_color_hex(owner->settings.colors.button_status), 0);
        }
    }
    if (help) {
        lv_obj_set_style_text_color(help, lv_color_hex(owner->settings.colors.text_secondary), 0);
    }
    refreshMenu();
}

void HardwareControlService::refreshMenu()
{
    if (!owner || !menu) {
        return;
    }

    static const char *row_labels[HW_MENU_ITEM_COUNT] = {
        "Range", "Aircraft", "Heading", "Airports", "Countries",
        "Runways", "Ground A/C", "Sweep", "WiFi Setup", "Reboot"
    };
    char row_values[HW_MENU_ITEM_COUNT][32] = {};
    snprintf(row_values[HW_MENU_RANGE], sizeof(row_values[HW_MENU_RANGE]), "%d MI", owner->get_current_range_mi());
    snprintf(row_values[HW_MENU_FILTER], sizeof(row_values[HW_MENU_FILTER]), "%s",
             owner->aircraft_filter == AIRCRAFT_FILTER_MILITARY ? "Military" :
             (owner->aircraft_filter == AIRCRAFT_FILTER_INTERESTING ? "Interesting" : "All"));
    snprintf(row_values[HW_MENU_HEADING], sizeof(row_values[HW_MENU_HEADING]), "%s",
             !owner->settings.show_aircraft_heading ||
             owner->settings.aircraft_heading_style == RADAR_HEADING_STYLE_NONE ? "Off" :
             (owner->settings.aircraft_heading_style == RADAR_HEADING_STYLE_LINE ? "Line" : "Arrow"));
    snprintf(row_values[HW_MENU_AIRPORTS], sizeof(row_values[HW_MENU_AIRPORTS]), "%s",
             owner->settings.show_airports ? "On" : "Off");
    snprintf(row_values[HW_MENU_COUNTRIES], sizeof(row_values[HW_MENU_COUNTRIES]), "%s",
             owner->settings.show_countries ? "On" : "Off");
    snprintf(row_values[HW_MENU_RUNWAYS], sizeof(row_values[HW_MENU_RUNWAYS]), "%s",
             owner->settings.center_source == RADAR_CENTER_SOURCE_AIRPORT ?
             (owner->settings.show_airport_runways ? "On" : "Off") : "N/A");
    snprintf(row_values[HW_MENU_GROUND], sizeof(row_values[HW_MENU_GROUND]), "%s",
             owner->settings.show_ground_aircraft ? "On" : "Off");
    snprintf(row_values[HW_MENU_SWEEP], sizeof(row_values[HW_MENU_SWEEP]), "%s",
             owner->settings.show_sweep ? "On" : "Off");
    snprintf(row_values[HW_MENU_WIFI_SETUP], sizeof(row_values[HW_MENU_WIFI_SETUP]), "Start");
    snprintf(row_values[HW_MENU_REBOOT], sizeof(row_values[HW_MENU_REBOOT]), "Restart");

    for (int i = 0; i < HW_MENU_ITEM_COUNT; ++i) {
        if (labels[i]) {
            lv_label_set_text(labels[i], row_labels[i]);
        }
        if (values[i]) {
            lv_label_set_text(values[i], row_values[i]);
        }
        if (rows[i]) {
            bool is_selected = i == selected;
            lv_obj_set_style_bg_opa(rows[i], is_selected ? LV_OPA_70 : LV_OPA_20, 0);
            lv_obj_set_style_border_width(rows[i], is_selected ? 2 : 1, 0);
        }
    }

    if (help) {
        lv_label_set_text(help,
                          owner->settings.hardware_show_hints ?
                          "Rotate: move  Confirm/Push: select  Back: close" : "");
    }
}

bool HardwareControlService::menuVisible() const
{
    return menu && !lv_obj_has_flag(menu, LV_OBJ_FLAG_HIDDEN);
}

void HardwareControlService::showMenu()
{
    if (!owner || !menu || !owner->settings.hardware_controls_enabled) {
        return;
    }
    owner->hide_range_menu();
    owner->hide_data_menu();
    owner->hide_wifi_menu();
    last_use_us = esp_timer_get_time();
    refreshMenu();
    lv_obj_clear_flag(menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(menu);
}

void HardwareControlService::hideMenu()
{
    if (menu) {
        lv_obj_add_flag(menu, LV_OBJ_FLAG_HIDDEN);
    }
}

void HardwareControlService::moveSelection(int delta)
{
    if (!menuVisible()) {
        return;
    }
    selected += delta;
    while (selected < 0) {
        selected += HW_MENU_ITEM_COUNT;
    }
    selected %= HW_MENU_ITEM_COUNT;
    last_use_us = esp_timer_get_time();
    refreshMenu();
}

void HardwareControlService::selectItem()
{
    if (!owner) {
        return;
    }
    if (!menuVisible()) {
        showMenu();
        return;
    }

    bool changed = false;
    last_use_us = esp_timer_get_time();
    switch (selected) {
    case HW_MENU_RANGE:
        owner->change_range_by_delta(1);
        break;
    case HW_MENU_FILTER:
        owner->aircraft_filter = owner->aircraft_filter == AIRCRAFT_FILTER_ALL ? AIRCRAFT_FILTER_MILITARY :
                                 (owner->aircraft_filter == AIRCRAFT_FILTER_MILITARY ? AIRCRAFT_FILTER_INTERESTING :
                                  AIRCRAFT_FILTER_ALL);
        changed = true;
        break;
    case HW_MENU_HEADING:
        if (!owner->settings.show_aircraft_heading ||
            owner->settings.aircraft_heading_style == RADAR_HEADING_STYLE_NONE) {
            owner->settings.show_aircraft_heading = true;
            owner->settings.aircraft_heading_style = RADAR_HEADING_STYLE_ARROW;
        } else if (owner->settings.aircraft_heading_style == RADAR_HEADING_STYLE_ARROW) {
            owner->settings.aircraft_heading_style = RADAR_HEADING_STYLE_LINE;
        } else {
            owner->settings.show_aircraft_heading = false;
            owner->settings.aircraft_heading_style = RADAR_HEADING_STYLE_NONE;
        }
        owner->settings.visible.aircraft_heading = owner->settings.show_aircraft_heading;
        changed = true;
        break;
    case HW_MENU_AIRPORTS:
        owner->settings.show_airports = !owner->settings.show_airports;
        owner->settings.visible.airport = owner->settings.show_airports;
        changed = true;
        break;
    case HW_MENU_COUNTRIES:
        owner->settings.show_countries = !owner->settings.show_countries;
        owner->settings.visible.country_boundary = owner->settings.show_countries;
        changed = true;
        break;
    case HW_MENU_RUNWAYS:
        if (owner->settings.center_source == RADAR_CENTER_SOURCE_AIRPORT) {
            owner->settings.show_airport_runways = !owner->settings.show_airport_runways;
            owner->settings.visible.runway = owner->settings.show_airport_runways;
            if (!owner->settings.show_airport_runways) {
                owner->clear_active_runway_cache();
            } else if (owner->wifi_event_group) {
                xEventGroupSetBits(owner->wifi_event_group, FETCH_NOW_BIT);
            }
            changed = true;
        }
        break;
    case HW_MENU_GROUND:
        owner->settings.show_ground_aircraft = !owner->settings.show_ground_aircraft;
        changed = true;
        break;
    case HW_MENU_SWEEP:
        owner->settings.show_sweep = !owner->settings.show_sweep;
        owner->settings.visible.sweep = owner->settings.show_sweep;
        changed = true;
        break;
    case HW_MENU_WIFI_SETUP:
        hideMenu();
        owner->request_wifi_portal();
        break;
    case HW_MENU_REBOOT:
        owner->set_data_status("REBOOT");
        esp_restart();
        break;
    default:
        break;
    }

    if (changed) {
        owner->settings_generation++;
        owner->invalidate_aircraft_display();
        owner->refresh_data_menu();
    }
    refreshMenu();
}

void HardwareControlService::applyButtonAction(int action)
{
    if (!owner || !owner->settings.hardware_controls_enabled) {
        return;
    }

    switch (action) {
    case RADAR_HW_BUTTON_NONE:
        break;
    case RADAR_HW_BUTTON_BACK_CLOSE:
        if (menuVisible()) {
            hideMenu();
        } else {
            owner->hide_range_menu();
            owner->hide_data_menu();
            owner->hide_wifi_menu();
        }
        break;
    case RADAR_HW_BUTTON_RANGE_UP:
        owner->change_range_by_delta(1);
        break;
    case RADAR_HW_BUTTON_RANGE_DOWN:
        owner->change_range_by_delta(-1);
        break;
    case RADAR_HW_BUTTON_DATA_MENU:
        owner->toggle_data_menu();
        break;
    case RADAR_HW_BUTTON_WIFI_MENU:
        owner->toggle_wifi_menu();
        break;
    case RADAR_HW_BUTTON_MENU_SELECT:
    default:
        selectItem();
        break;
    }
}

bool HardwareControlService::consumeButtonPress(gpio_num_t gpio, int *last_level, int64_t *last_change_us)
{
    int level = gpio_get_level(gpio);
    int64_t now_us = esp_timer_get_time();
    if (level == *last_level) {
        return false;
    }
    if (now_us - *last_change_us < BUTTON_DEBOUNCE_US) {
        return false;
    }
    *last_change_us = now_us;
    *last_level = level;
    return level == 0;
}

void HardwareControlService::timerTick()
{
    if (!owner || !owner->settings.hardware_controls_enabled) {
        return;
    }

    if (consumeButtonPress(confirm_gpio, &confirm_last_level, &confirm_last_change_us)) {
        applyButtonAction(owner->settings.hardware_confirm_action);
    }
    if (consumeButtonPress(back_gpio, &back_last_level, &back_last_change_us)) {
        applyButtonAction(owner->settings.hardware_back_action);
    }
    if (consumeButtonPress(rotary_push_gpio, &push_last_level, &push_last_change_us)) {
        applyButtonAction(owner->settings.hardware_push_action);
    }

    if (menuVisible() &&
        owner->settings.hardware_menu_timeout_sec > 0 &&
        esp_timer_get_time() - last_use_us >
            (int64_t)owner->settings.hardware_menu_timeout_sec * 1000000LL) {
        hideMenu();
    }

    int raw_delta = encoder_pending_delta;
    if (raw_delta == 0) {
        return;
    }
    encoder_pending_delta = 0;
    encoder_event_accum += raw_delta;

    int delta = 0;
    if (encoder_event_accum >= 2) {
        delta = 1;
        encoder_event_accum -= 2;
    } else if (encoder_event_accum <= -2) {
        delta = -1;
        encoder_event_accum += 2;
    }
    if (delta == 0) {
        return;
    }

    ESP_LOGI(TAG, "Rotary step raw=%d accum=%d delta=%d", raw_delta, encoder_event_accum, delta);
    if (menuVisible()) {
        moveSelection(delta);
    } else if (owner->settings.hardware_rotary_action == RADAR_HW_ROTARY_MENU) {
        showMenu();
        moveSelection(delta);
    } else {
        owner->change_range_by_delta(delta);
    }
}
