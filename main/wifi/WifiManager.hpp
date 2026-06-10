#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi.h"

class RadarApp;

/*
 * WiFi state machine for the hosted ESP32-C6 WiFi module.
 *
 * The ESP32-P4 talks to the WiFi coprocessor through ESP Hosted over SDIO.
 * This manager keeps station setup, captive portal setup, credential storage,
 * scans, and recovery in one place so the radar and settings services can treat
 * WiFi as a small set of state transitions.
 */
class WifiManager {
public:
    /* Attach the owning application that holds WiFi state and event groups. */
    void bind(RadarApp *app) { owner = app; }

    /* Ask the main loop to start the captive portal. */
    void requestPortal();

    /* Consume one pending portal request. */
    bool consumePortalRequest();

    /* Build a setup AP SSID that is stable but board-specific. */
    void initSetupApSsid();

    /* Load saved station credentials from NVS. */
    bool loadCredentials();

    /* Save station credentials to NVS. */
    esp_err_t saveCredentials(const char *ssid, const char *password);

    /* Scan nearby APs, sorted by strongest signal and de-duplicated by SSID. */
    uint16_t scanNetworks(wifi_ap_record_t *aps, uint16_t max_count);

    /* Start the setup AP and captive portal services. */
    bool startPortal();

    /* Stop the setup AP and captive portal services. */
    void stopPortal();

    /* Start station mode using saved credentials. */
    bool startStation();

    /* Recover WiFi after an HTTP/TLS failure without rebooting the app. */
    bool recoverAfterHttpFailure();

    /* ESP event callback body for WiFi and IP events. */
    void handleEvent(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

    /* Create netifs, event groups, and default WiFi state. */
    bool init();

private:
    RadarApp *owner = nullptr;

    /* Compare AP records with strongest RSSI first. */
    static int scanCompareRssi(const void *left, const void *right);

    /* Return whether an SSID already exists in the de-duplicated scan list. */
    static bool ssidAlreadyListed(const wifi_ap_record_t *aps, uint16_t count, const char *ssid);

    /* Make the setup AP advertise itself as DNS server for captive probes. */
    void configurePortalDhcpDns();
};
