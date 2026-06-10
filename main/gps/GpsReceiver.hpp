#pragma once

#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/cdc_acm_host.h"

#define GPS_STATUS_TEXT_LEN 24
#define GPS_DETAIL_TEXT_LEN 72

/*
 * Snapshot of the USB GPS receiver state.
 *
 * The snapshot is copied out under a mutex so callers can read it without
 * holding a lock. Coordinates are valid only when has_fix is true and
 * fix_stale is false.
 */
typedef struct {
    bool usb_host_started;
    bool driver_started;
    bool device_connected;
    bool receiving;
    bool has_fix;
    bool fix_stale;
    double lat;
    double lon;
    int satellites;
    float hdop;
    uint32_t sentence_count;
    int64_t last_rx_ms;
    int64_t last_fix_ms;
    char status[GPS_STATUS_TEXT_LEN];
    char detail[GPS_DETAIL_TEXT_LEN];
} gps_snapshot_t;

/*
 * USB CDC NMEA GPS receiver.
 *
 * The Waveshare board exposes a USB-A host port. This class watches for a CDC
 * ACM GPS dongle, parses GGA/RMC sentences, and publishes a small status
 * snapshot for the radar and settings page. It does not own the radar centre;
 * RadarPositionProvider decides whether to use the GPS fix.
 */
class GpsReceiver {
public:
    /* Construct an idle GPS receiver; start() performs all USB setup. */
    GpsReceiver() = default;

    /* Start the USB host task and CDC driver. Safe to call once during app start. */
    bool start();

    /* Copy the latest receiver state into out. */
    void getSnapshot(gps_snapshot_t *out) const;

    /* Return a fresh fix if one is available. */
    bool getFix(double *lat, double *lon) const;

private:
    static constexpr size_t NMEA_LINE_MAX = 128;
    static constexpr int64_t RX_STALE_MS = 5000;
    static constexpr int64_t FIX_STALE_MS = 60000;

    mutable SemaphoreHandle_t mutex = nullptr;
    TaskHandle_t task_handle = nullptr;
    cdc_acm_dev_hdl_t cdc_dev = nullptr;
    gps_snapshot_t state = {};
    char line[NMEA_LINE_MAX] = {};
    size_t line_len = 0;

    /* Forward the FreeRTOS task entry to the owning receiver. */
    static void taskEntry(void *arg);

    /* Forward CDC data from the USB host driver to the owning receiver. */
    static bool dataCallback(const uint8_t *data, size_t data_len, void *user_arg);

    /* Forward CDC device events to the owning receiver. */
    static void eventCallback(const cdc_acm_host_dev_event_data_t *event, void *user_ctx);

    /* Handle notification that a new USB CDC device is available. */
    static void newDeviceCallback(usb_device_handle_t usb_dev);

    /* USB worker task that installs drivers and pumps host events. */
    void task();

    /* Consume raw bytes from the GPS CDC endpoint and split them into NMEA lines. */
    bool onData(const uint8_t *data, size_t data_len);

    /* Apply CDC connection and disconnection events to receiver state. */
    void onEvent(const cdc_acm_host_dev_event_data_t *event);

    /* Parse one complete NMEA sentence after checksum validation. */
    void parseLine(const char *line);

    /* Parse a GGA sentence for fix, satellite, HDOP, and position fields. */
    void parseGga(char **fields, size_t count);

    /* Parse an RMC sentence for fix and position fields. */
    void parseRmc(char **fields, size_t count);

    /* Publish a parsed fix or no-fix state to the shared snapshot. */
    void publishFix(bool valid, double lat, double lon, int satellites, float hdop);

    /* Publish whether a CDC GPS device is currently connected. */
    void setDeviceConnected(bool connected);

    /* Return the current connected flag under the receiver lock. */
    bool isDeviceConnected() const;

    /* Publish the USB host and CDC driver startup state. */
    void markDriverStarted(bool usb_started, bool driver_started);

    /* Return the current monotonic time in milliseconds. */
    static int64_t nowMs();

    /* Validate the NMEA checksum at the end of a sentence. */
    static bool checksumOk(const char *line);

    /* Convert an NMEA coordinate field into signed decimal degrees. */
    static bool parseCoordinate(const char *value, const char *hemisphere, bool latitude, double *out);

    /* Parse an integer with a fallback for empty or malformed fields. */
    static int parseInt(const char *value, int fallback);

    /* Parse a floating-point value with a fallback for empty or malformed fields. */
    static float parseFloat(const char *value, float fallback);

    /* Fill human-readable status strings based on timestamps and fix state. */
    static void fillComputedStatus(gps_snapshot_t *snapshot, int64_t now_ms);
};
