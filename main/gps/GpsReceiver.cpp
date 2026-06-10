#include "GpsReceiver.hpp"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "usb/usb_host.h"

static const char *TAG = "gps_receiver";

/* Start the USB GPS worker task and create its state mutex. */
bool GpsReceiver::start()
{
    if (task_handle) {
        return true;
    }
    if (!mutex) {
        mutex = xSemaphoreCreateMutex();
    }
    if (!mutex) {
        return false;
    }

    if (xTaskCreateWithCaps(GpsReceiver::taskEntry, "gps_usb", 8192, this, 4, &task_handle,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        task_handle = nullptr;
        return false;
    }
    return true;
}

/* Copy the latest GPS state and add computed stale/receiving status fields. */
void GpsReceiver::getSnapshot(gps_snapshot_t *out) const
{
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    if (mutex && xSemaphoreTake(mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        *out = state;
        xSemaphoreGive(mutex);
    }
    fillComputedStatus(out, nowMs());
}

/* Return the latest fresh GPS fix when one is available. */
bool GpsReceiver::getFix(double *lat, double *lon) const
{
    gps_snapshot_t snapshot = {};
    getSnapshot(&snapshot);
    if (!snapshot.has_fix || snapshot.fix_stale) {
        return false;
    }
    if (lat) {
        *lat = snapshot.lat;
    }
    if (lon) {
        *lon = snapshot.lon;
    }
    return true;
}

/* FreeRTOS task entry that forwards to the receiver instance. */
void GpsReceiver::taskEntry(void *arg)
{
    GpsReceiver *receiver = static_cast<GpsReceiver *>(arg);
    if (receiver) {
        receiver->task();
    }
    vTaskDeleteWithCaps(NULL);
}

/* USB CDC data callback that forwards incoming bytes to the receiver instance. */
bool GpsReceiver::dataCallback(const uint8_t *data, size_t data_len, void *user_arg)
{
    GpsReceiver *receiver = static_cast<GpsReceiver *>(user_arg);
    return receiver ? receiver->onData(data, data_len) : true;
}

/* USB CDC event callback that forwards connection events to the receiver instance. */
void GpsReceiver::eventCallback(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    GpsReceiver *receiver = static_cast<GpsReceiver *>(user_ctx);
    if (receiver) {
        receiver->onEvent(event);
    }
}

/* Log USB descriptor information when a new CDC candidate appears. */
void GpsReceiver::newDeviceCallback(usb_device_handle_t usb_dev)
{
    const usb_device_desc_t *device_desc = nullptr;
    if (usb_host_get_device_descriptor(usb_dev, &device_desc) == ESP_OK && device_desc) {
        ESP_LOGI(TAG, "USB device connected VID=0x%04x PID=0x%04x",
                 device_desc->idVendor, device_desc->idProduct);
    }
}

/* Own the USB host and CDC open loop for a GPS device on the USB-A port. */
void GpsReceiver::task()
{
    ESP_LOGI(TAG, "Starting USB GPS receiver");
    esp_err_t err = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, false);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "USB host start failed: %s", esp_err_to_name(err));
        markDriverStarted(false, false);
        return;
    }

    const cdc_acm_host_driver_config_t driver_config = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 9,
        .xCoreID = tskNO_AFFINITY,
        .new_dev_cb = GpsReceiver::newDeviceCallback,
    };
    err = cdc_acm_host_install(&driver_config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "CDC-ACM host install failed: %s", esp_err_to_name(err));
        markDriverStarted(true, false);
        return;
    }
    markDriverStarted(true, true);

    for (;;) {
        cdc_acm_dev_hdl_t dev = nullptr;
        /*
         * USB GPS receivers are not consistent about which CDC interface carries
         * NMEA. Try the first few interfaces rather than hard-coding one model's
         * descriptor layout.
         */
        for (uint8_t interface_idx = 0; interface_idx < 4 && !dev; ++interface_idx) {
            const cdc_acm_host_open_config_t open_config = {
                .vid = CDC_HOST_ANY_VID,
                .pid = CDC_HOST_ANY_PID,
                .interface_idx = interface_idx,
                .dev_addr = CDC_HOST_ANY_DEV_ADDR,
                .connection_timeout_ms = 1500,
                .out_buffer_size = 0,
                .in_buffer_size = 256,
                .event_cb = GpsReceiver::eventCallback,
                .data_cb = GpsReceiver::dataCallback,
                .user_arg = this,
            };
            esp_err_t open_err = cdc_acm_host_open(&open_config, &dev);
            if (open_err != ESP_OK && open_err != ESP_ERR_NOT_FOUND) {
                ESP_LOGD(TAG, "CDC open interface %u failed: %s",
                         interface_idx, esp_err_to_name(open_err));
            }
        }

        if (!dev) {
            setDeviceConnected(false);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        cdc_dev = dev;
        setDeviceConnected(true);
        ESP_LOGI(TAG, "USB GPS CDC device opened");

        const cdc_acm_line_coding_t line_coding = {
            .dwDTERate = 9600,
            .bCharFormat = 0,
            .bParityType = 0,
            .bDataBits = 8,
        };
        ESP_ERROR_CHECK_WITHOUT_ABORT(cdc_acm_host_line_coding_set(dev, &line_coding));
        ESP_ERROR_CHECK_WITHOUT_ABORT(cdc_acm_host_set_control_line_state(dev, true, true));

        while (isDeviceConnected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        cdc_dev = nullptr;
        ESP_LOGI(TAG, "USB GPS device closed");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* Buffer arbitrary CDC bytes into complete NMEA sentences. */
bool GpsReceiver::onData(const uint8_t *data, size_t data_len)
{
    if (!data || data_len == 0) {
        return true;
    }

    if (mutex && xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        state.last_rx_ms = nowMs();
        state.device_connected = true;
        xSemaphoreGive(mutex);
    }

    /*
     * CDC delivers arbitrary byte chunks, not whole NMEA sentences. Keep a
     * small line buffer and parse only after CR/LF terminates a sentence.
     */
    for (size_t i = 0; i < data_len; ++i) {
        char ch = (char)data[i];
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            if (line_len > 0) {
                line[line_len] = '\0';
                parseLine(line);
                line_len = 0;
            }
            continue;
        }
        if (line_len + 1 < NMEA_LINE_MAX) {
            line[line_len++] = ch;
        } else {
            line_len = 0;
        }
    }
    return true;
}

/* Apply CDC error and disconnect events to receiver state. */
void GpsReceiver::onEvent(const cdc_acm_host_dev_event_data_t *event)
{
    if (!event) {
        return;
    }

    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGW(TAG, "CDC GPS error: %d", event->data.error);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGI(TAG, "USB GPS disconnected");
        setDeviceConnected(false);
        ESP_ERROR_CHECK_WITHOUT_ABORT(cdc_acm_host_close(event->data.cdc_hdl));
        if (cdc_dev == event->data.cdc_hdl) {
            cdc_dev = nullptr;
        }
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
    case CDC_ACM_HOST_NETWORK_CONNECTION:
        break;
#ifdef CDC_HOST_SUSPEND_RESUME_API_SUPPORTED
    case CDC_ACM_HOST_DEVICE_SUSPENDED:
    case CDC_ACM_HOST_DEVICE_RESUMED:
        break;
#endif
    default:
        break;
    }
}

/* Validate and dispatch one NMEA sentence to the supported sentence parser. */
void GpsReceiver::parseLine(const char *nmea)
{
    if (!nmea || nmea[0] != '$' || !checksumOk(nmea)) {
        return;
    }

    char copy[NMEA_LINE_MAX];
    snprintf(copy, sizeof(copy), "%s", nmea);
    char *star = strchr(copy, '*');
    if (star) {
        *star = '\0';
    }

    char *fields[24] = {};
    size_t count = 0;
    char *cursor = copy;
    while (cursor && count < (sizeof(fields) / sizeof(fields[0]))) {
        fields[count++] = cursor;
        char *comma = strchr(cursor, ',');
        if (!comma) {
            break;
        }
        *comma = '\0';
        cursor = comma + 1;
    }
    if (count == 0 || strlen(fields[0]) < 6) {
        return;
    }

    const char *type = fields[0] + 1;
    if (strcmp(type + 2, "GGA") == 0) {
        parseGga(fields, count);
    } else if (strcmp(type + 2, "RMC") == 0) {
        parseRmc(fields, count);
    }

    if (mutex && xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        state.sentence_count++;
        xSemaphoreGive(mutex);
    }
}

/* Parse a GGA sentence for fix quality, satellites, HDOP, and position. */
void GpsReceiver::parseGga(char **fields, size_t count)
{
    if (count < 7) {
        return;
    }

    double lat = 0.0;
    double lon = 0.0;
    int fix_quality = parseInt(fields[6], 0);
    int satellites = count > 7 ? parseInt(fields[7], 0) : 0;
    float hdop = count > 8 ? parseFloat(fields[8], NAN) : NAN;
    bool valid = fix_quality > 0 &&
                 count > 5 &&
                 parseCoordinate(fields[2], fields[3], true, &lat) &&
                 parseCoordinate(fields[4], fields[5], false, &lon);
    publishFix(valid, lat, lon, satellites, hdop);
}

/* Parse an RMC sentence for active-fix and position data. */
void GpsReceiver::parseRmc(char **fields, size_t count)
{
    if (count < 7) {
        return;
    }

    double lat = 0.0;
    double lon = 0.0;
    bool valid = fields[2] && fields[2][0] == 'A' &&
                 parseCoordinate(fields[3], fields[4], true, &lat) &&
                 parseCoordinate(fields[5], fields[6], false, &lon);
    publishFix(valid, lat, lon, -1, NAN);
}

/* Publish the latest fix or no-fix state to the shared snapshot. */
void GpsReceiver::publishFix(bool valid, double lat, double lon, int satellites, float hdop)
{
    if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }

    state.device_connected = true;
    if (satellites >= 0) {
        state.satellites = satellites;
    }
    if (!isnan(hdop)) {
        state.hdop = hdop;
    }
    if (valid) {
        state.has_fix = true;
        state.lat = lat;
        state.lon = lon;
        state.last_fix_ms = nowMs();
    } else {
        state.has_fix = false;
    }

    xSemaphoreGive(mutex);
}

/* Publish USB device connection state and clear stale fix data on disconnect. */
void GpsReceiver::setDeviceConnected(bool connected)
{
    if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }

    state.device_connected = connected;
    if (!connected) {
        state.has_fix = false;
        state.last_rx_ms = 0;
        state.last_fix_ms = 0;
        line_len = 0;
    }

    xSemaphoreGive(mutex);
}

/* Read the current device connection state under the receiver mutex. */
bool GpsReceiver::isDeviceConnected() const
{
    bool connected = false;
    if (mutex && xSemaphoreTake(mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        connected = state.device_connected;
        xSemaphoreGive(mutex);
    }
    return connected;
}

/* Publish USB host and CDC driver readiness. */
void GpsReceiver::markDriverStarted(bool usb_started, bool driver_started)
{
    if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    state.usb_host_started = usb_started;
    state.driver_started = driver_started;
    xSemaphoreGive(mutex);
}

/* Return monotonic time in milliseconds. */
int64_t GpsReceiver::nowMs()
{
    return esp_timer_get_time() / 1000;
}

/* Validate an NMEA checksum when the sentence includes one. */
bool GpsReceiver::checksumOk(const char *line)
{
    const char *star = strchr(line, '*');
    if (!star) {
        return true;
    }
    if (star[1] == '\0' || star[2] == '\0') {
        return false;
    }

    uint8_t checksum = 0;
    for (const char *p = line + 1; p < star; ++p) {
        checksum ^= (uint8_t)*p;
    }

    char expected[3];
    expected[0] = star[1];
    expected[1] = star[2];
    expected[2] = '\0';
    char *end = nullptr;
    unsigned long parsed = strtoul(expected, &end, 16);
    return end && *end == '\0' && parsed == checksum;
}

/* Convert an NMEA ddmm.mmmm coordinate plus hemisphere into signed degrees. */
bool GpsReceiver::parseCoordinate(const char *value, const char *hemisphere, bool latitude, double *out)
{
    if (!value || !hemisphere || !out || value[0] == '\0' || hemisphere[0] == '\0') {
        return false;
    }

    char *end = nullptr;
    double raw = strtod(value, &end);
    if (end == value || raw <= 0.0) {
        return false;
    }

    int degrees = (int)(raw / 100.0);
    double minutes = raw - ((double)degrees * 100.0);
    if ((latitude && degrees > 90) || (!latitude && degrees > 180) || minutes < 0.0 || minutes >= 60.0) {
        return false;
    }

    double result = (double)degrees + (minutes / 60.0);
    if (hemisphere[0] == 'S' || hemisphere[0] == 'W') {
        result = -result;
    } else if (hemisphere[0] != 'N' && hemisphere[0] != 'E') {
        return false;
    }
    *out = result;
    return true;
}

/* Parse an integer NMEA field with a fallback. */
int GpsReceiver::parseInt(const char *value, int fallback)
{
    if (!value || value[0] == '\0') {
        return fallback;
    }
    char *end = nullptr;
    long parsed = strtol(value, &end, 10);
    return end == value ? fallback : (int)parsed;
}

/* Parse a floating-point NMEA field with a fallback. */
float GpsReceiver::parseFloat(const char *value, float fallback)
{
    if (!value || value[0] == '\0') {
        return fallback;
    }
    char *end = nullptr;
    float parsed = strtof(value, &end);
    return end == value ? fallback : parsed;
}

/* Derive human-readable GPS status strings from timestamps and fix state. */
void GpsReceiver::fillComputedStatus(gps_snapshot_t *snapshot, int64_t now_ms)
{
    if (!snapshot) {
        return;
    }

    snapshot->receiving = snapshot->device_connected &&
                          snapshot->last_rx_ms > 0 &&
                          now_ms - snapshot->last_rx_ms <= RX_STALE_MS;
    snapshot->fix_stale = !snapshot->has_fix ||
                          snapshot->last_fix_ms <= 0 ||
                          now_ms - snapshot->last_fix_ms > FIX_STALE_MS;

    if (!snapshot->usb_host_started || !snapshot->driver_started) {
        snprintf(snapshot->status, sizeof(snapshot->status), "GPS USB ERR");
        snprintf(snapshot->detail, sizeof(snapshot->detail), "USB host not ready");
    } else if (!snapshot->device_connected) {
        snprintf(snapshot->status, sizeof(snapshot->status), "GPS NO DEV");
        snprintf(snapshot->detail, sizeof(snapshot->detail), "No GPS on USB-A");
    } else if (!snapshot->receiving) {
        snprintf(snapshot->status, sizeof(snapshot->status), "GPS NO DATA");
        snprintf(snapshot->detail, sizeof(snapshot->detail), "GPS connected, waiting for NMEA");
    } else if (!snapshot->has_fix || snapshot->fix_stale) {
        snprintf(snapshot->status, sizeof(snapshot->status), "GPS WAIT");
        snprintf(snapshot->detail, sizeof(snapshot->detail), "Waiting for lock (%d sats)", snapshot->satellites);
    } else {
        snprintf(snapshot->status, sizeof(snapshot->status), "GPS LOCK");
        snprintf(snapshot->detail, sizeof(snapshot->detail), "GPS lock (%d sats)",
                 snapshot->satellites);
    }
}
