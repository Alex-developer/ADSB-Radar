#include <string.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "WifiManager.hpp"
#include "RadarApp.hpp"

static const char *TAG = "wifi_manager";

/* Return a short label for the WiFi disconnect reasons that most often affect setup. */
static const char *wifi_disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
        return "auth expired";
    case WIFI_REASON_AUTH_LEAVE:
        return "auth leave";
    case WIFI_REASON_ASSOC_EXPIRE:
        return "association expired";
    case WIFI_REASON_ASSOC_TOOMANY:
        return "AP has too many clients";
    case WIFI_REASON_NOT_AUTHED:
        return "not authenticated";
    case WIFI_REASON_NOT_ASSOCED:
        return "not associated";
    case WIFI_REASON_ASSOC_LEAVE:
        return "association leave";
    case WIFI_REASON_ASSOC_NOT_AUTHED:
        return "association not authenticated";
    case WIFI_REASON_DISASSOC_PWRCAP_BAD:
        return "bad power capability";
    case WIFI_REASON_DISASSOC_SUPCHAN_BAD:
        return "unsupported channel";
    case WIFI_REASON_BSS_TRANSITION_DISASSOC:
        return "BSS transition";
    case WIFI_REASON_IE_INVALID:
        return "invalid IE";
    case WIFI_REASON_MIC_FAILURE:
        return "MIC failure";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4-way handshake timeout";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        return "group key update timeout";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
        return "IE differs in 4-way handshake";
    case WIFI_REASON_GROUP_CIPHER_INVALID:
        return "invalid group cipher";
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        return "invalid pairwise cipher";
    case WIFI_REASON_AKMP_INVALID:
        return "invalid AKM";
    case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
        return "unsupported RSN IE";
    case WIFI_REASON_INVALID_RSN_IE_CAP:
        return "invalid RSN IE capability";
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return "802.1X auth failed";
    case WIFI_REASON_CIPHER_SUITE_REJECTED:
        return "cipher suite rejected";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "beacon timeout";
    case WIFI_REASON_NO_AP_FOUND:
        return "AP not found";
    case WIFI_REASON_AUTH_FAIL:
        return "authentication failed";
    case WIFI_REASON_ASSOC_FAIL:
        return "association failed";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "handshake timeout";
    case WIFI_REASON_CONNECTION_FAIL:
        return "connection failed";
    case WIFI_REASON_AP_TSF_RESET:
        return "AP TSF reset";
    case WIFI_REASON_ROAMING:
        return "roaming";
    default:
        return "unknown";
    }
}

/*
 * WiFi operations are centralised here because the hosted WiFi driver is easier
 * to keep stable when start/stop/recovery paths are serialised. UI callbacks
 * set event bits; the fetch task consumes those bits and performs the actual
 * transition.
 */

/* Request captive portal mode from the network worker task. */
void WifiManager::requestPortal()
{
    if (!owner) {
        return;
    }

    owner->set_data_status("SET WIFI");
    owner->set_portal_status("Starting setup network");
    if (owner->wifi_event_group) {
        xEventGroupSetBits(owner->wifi_event_group, WIFI_PORTAL_REQUEST_BIT | FETCH_NOW_BIT);
    }
}

/* Consume and clear a pending captive portal request bit. */
bool WifiManager::consumePortalRequest()
{
    if (!owner || !owner->wifi_event_group) {
        return false;
    }

    EventBits_t bits = xEventGroupGetBits(owner->wifi_event_group);
    if ((bits & WIFI_PORTAL_REQUEST_BIT) == 0) {
        return false;
    }

    xEventGroupClearBits(owner->wifi_event_group, WIFI_PORTAL_REQUEST_BIT);
    return true;
}

/* Build the setup access-point SSID from the device MAC address. */
void WifiManager::initSetupApSsid()
{
    if (!owner) {
        return;
    }

    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_BASE) == ESP_OK) {
        snprintf(owner->setup_ap_ssid, sizeof(owner->setup_ap_ssid),
                 "RadarSetup-%02X%02X", mac[4], mac[5]);
    }
}

/* Load saved station credentials, falling back to build-time defaults. */
bool WifiManager::loadCredentials()
{
    if (!owner) {
        return false;
    }

    owner->wifi_ssid[0] = '\0';
    owner->wifi_password[0] = '\0';

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CRED_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t ssid_len = sizeof(owner->wifi_ssid);
        err = nvs_get_str(handle, WIFI_CRED_SSID_KEY, owner->wifi_ssid, &ssid_len);
        if (err == ESP_OK && owner->wifi_ssid[0] != '\0') {
            size_t pass_len = sizeof(owner->wifi_password);
            if (nvs_get_str(handle, WIFI_CRED_PASS_KEY, owner->wifi_password, &pass_len) != ESP_OK) {
                owner->wifi_password[0] = '\0';
            }
            nvs_close(handle);
            return true;
        }
        nvs_close(handle);
    }

    if (strlen(CONFIG_RADAR_WIFI_SSID) > 0) {
        snprintf(owner->wifi_ssid, sizeof(owner->wifi_ssid), "%s", CONFIG_RADAR_WIFI_SSID);
        snprintf(owner->wifi_password, sizeof(owner->wifi_password), "%s", CONFIG_RADAR_WIFI_PASSWORD);
        return true;
    }

    return false;
}

/* Save station credentials to NVS and update the live owner fields. */
esp_err_t WifiManager::saveCredentials(const char *ssid, const char *password)
{
    if (!owner || !ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CRED_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, WIFI_CRED_SSID_KEY, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_CRED_PASS_KEY, password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        snprintf(owner->wifi_ssid, sizeof(owner->wifi_ssid), "%s", ssid);
        snprintf(owner->wifi_password, sizeof(owner->wifi_password), "%s", password ? password : "");
    }

    return err;
}

/* Sort WiFi scan results with strongest RSSI first. */
int WifiManager::scanCompareRssi(const void *left, const void *right)
{
    const wifi_ap_record_t *a = (const wifi_ap_record_t *)left;
    const wifi_ap_record_t *b = (const wifi_ap_record_t *)right;
    return (int)b->rssi - (int)a->rssi;
}

/* Return whether an SSID is already present in the de-duplicated result set. */
bool WifiManager::ssidAlreadyListed(const wifi_ap_record_t *aps, uint16_t count, const char *ssid)
{
    for (uint16_t i = 0; i < count; ++i) {
        if (strncmp((const char *)aps[i].ssid, ssid, sizeof(aps[i].ssid)) == 0) {
            return true;
        }
    }
    return false;
}

/* Scan nearby WiFi networks and return de-duplicated results for the UI. */
uint16_t WifiManager::scanNetworks(wifi_ap_record_t *aps, uint16_t max_count)
{
    if (!owner || !aps || max_count == 0) {
        return 0;
    }

    wifi_ap_record_t *raw = (wifi_ap_record_t *)heap_caps_calloc(max_count, sizeof(raw[0]),
                                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw) {
        raw = (wifi_ap_record_t *)heap_caps_calloc(max_count, sizeof(raw[0]), MALLOC_CAP_8BIT);
    }
    if (!raw) {
        ESP_LOGW(TAG, "No memory for WiFi scan results");
        return 0;
    }

    uint16_t found = 0;
    if (owner->wifi_mutex && xSemaphoreTake(owner->wifi_mutex, pdMS_TO_TICKS(12000)) != pdTRUE) {
        free(raw);
        return 0;
    }

    wifi_scan_config_t scan_config = {};
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err == ESP_OK) {
        uint16_t raw_count = max_count;
        err = esp_wifi_scan_get_ap_records(&raw_count, raw);
        if (err == ESP_OK) {
            /*
             * Many access points advertise the same SSID on several radios.
             * Keep the strongest entry for each name so the setup UI stays
             * readable on a small phone screen.
             */
            qsort(raw, raw_count, sizeof(raw[0]), WifiManager::scanCompareRssi);
            for (uint16_t i = 0; i < raw_count && found < max_count; ++i) {
                raw[i].ssid[sizeof(raw[i].ssid) - 1] = '\0';
                if (raw[i].ssid[0] == '\0' ||
                    WifiManager::ssidAlreadyListed(aps, found, (const char *)raw[i].ssid)) {
                    continue;
                }
                aps[found++] = raw[i];
            }
        }
    } else {
        ESP_LOGW(TAG, "WiFi scan failed in setup portal: %s", esp_err_to_name(err));
    }

    if (owner->wifi_mutex) {
        xSemaphoreGive(owner->wifi_mutex);
    }
    free(raw);
    ESP_LOGI(TAG, "WiFi setup scan found %u networks", found);
    return found;
}

/* Configure DHCP to advertise the setup AP as the DNS server. */
void WifiManager::configurePortalDhcpDns()
{
    if (!owner || !owner->wifi_ap_netif) {
        return;
    }

    esp_netif_dns_info_t dns = {};
    dns.ip.type = IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = inet_addr(WIFI_SETUP_AP_IP);

    uint8_t dhcps_offer_option = DHCPS_OFFER_DNS;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(owner->wifi_ap_netif));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(owner->wifi_ap_netif, ESP_NETIF_OP_SET,
                                                         ESP_NETIF_DOMAIN_NAME_SERVER,
                                                         &dhcps_offer_option,
                                                         sizeof(dhcps_offer_option)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_dns_info(owner->wifi_ap_netif, ESP_NETIF_DNS_MAIN, &dns));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(owner->wifi_ap_netif));
}

/* Start AP+portal mode and stop normal station services while setup is active. */
bool WifiManager::startPortal()
{
    if (!owner || !owner->wifi_event_group || !owner->wifi_mutex) {
        return false;
    }
    if (owner->wifi_portal_active) {
        owner->set_portal_status("Connect to %s then open %s", owner->setup_ap_ssid, WIFI_SETUP_AP_IP);
        return true;
    }

    owner->settings_server.stop();
    owner->set_data_status("SET WIFI");
    owner->set_portal_status("Starting setup network");
    xEventGroupClearBits(owner->wifi_event_group, WIFI_CONNECTED_BIT);

    if (xSemaphoreTake(owner->wifi_mutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
        owner->set_portal_status("WiFi busy. Try again");
        return false;
    }

    owner->wifi_recovering = true;
    owner->wifi_portal_active = true;
    xEventGroupSetBits(owner->wifi_event_group, WIFI_PORTAL_ACTIVE_BIT);

    if (owner->wifi_started) {
        esp_err_t stop_err = esp_wifi_stop();
        if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "WiFi stop before setup failed: %s", esp_err_to_name(stop_err));
        }
        owner->wifi_started = false;
    }

    wifi_config_t ap_config = {};
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "%s", owner->setup_ap_ssid);
    ap_config.ap.ssid_len = strlen(owner->setup_ap_ssid);
    ap_config.ap.channel = WIFI_SETUP_AP_CHANNEL;
    ap_config.ap.max_connection = WIFI_SETUP_AP_MAX_CONN;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err == ESP_OK) {
        owner->wifi_started = true;
        esp_wifi_set_ps(WIFI_PS_NONE);
        configurePortalDhcpDns();
    }

    owner->wifi_recovering = false;
    xSemaphoreGive(owner->wifi_mutex);

    if (err != ESP_OK) {
        owner->wifi_portal_active = false;
        xEventGroupClearBits(owner->wifi_event_group, WIFI_PORTAL_ACTIVE_BIT);
        owner->set_portal_status("Setup failed: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "Starting WiFi setup AP failed: %s", esp_err_to_name(err));
        return false;
    }

    bool http_ok = owner->captive_portal.startHttpServer();
    bool dns_ok = owner->captive_portal.startDnsServer();
    if (!http_ok) {
        owner->set_portal_status("Portal web server failed");
        stopPortal();
        return false;
    }

    owner->set_portal_status(dns_ok ? "Connect to %s then open %s" : "Open %s after connecting",
                             dns_ok ? owner->setup_ap_ssid : WIFI_SETUP_AP_IP,
                             WIFI_SETUP_AP_IP);
    ESP_LOGI(TAG, "WiFi setup portal active: SSID=%s", owner->setup_ap_ssid);
    return true;
}

/* Stop captive portal services and reset WiFi state bits. */
void WifiManager::stopPortal()
{
    if (!owner) {
        return;
    }

    owner->captive_portal.stopServices();

    if (!owner->wifi_event_group || !owner->wifi_mutex) {
        return;
    }

    if (xSemaphoreTake(owner->wifi_mutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
        return;
    }

    owner->wifi_recovering = true;
    if (owner->wifi_started) {
        esp_err_t err = esp_wifi_stop();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "WiFi stop after setup failed: %s", esp_err_to_name(err));
        }
        owner->wifi_started = false;
    }
    owner->wifi_portal_active = false;
    xEventGroupClearBits(owner->wifi_event_group, WIFI_CONNECTED_BIT |
                         WIFI_PORTAL_ACTIVE_BIT | WIFI_PORTAL_REQUEST_BIT);
    owner->wifi_recovering = false;
    xSemaphoreGive(owner->wifi_mutex);
}

/* Start station mode using the currently loaded credentials. */
bool WifiManager::startStation()
{
    if (!owner) {
        return false;
    }
    if (owner->wifi_ssid[0] == '\0') {
        owner->set_data_status("SET WIFI");
        return false;
    }
    if (!owner->wifi_event_group || !owner->wifi_mutex) {
        return false;
    }

    owner->set_data_status("WIFI");
    xEventGroupClearBits(owner->wifi_event_group, WIFI_CONNECTED_BIT);

    if (xSemaphoreTake(owner->wifi_mutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
        owner->set_data_status("WIFI BUSY");
        return false;
    }

    owner->wifi_recovering = true;
    if (owner->wifi_started) {
        esp_err_t stop_err = esp_wifi_stop();
        if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "WiFi stop before station start failed: %s", esp_err_to_name(stop_err));
        }
        owner->wifi_started = false;
    }

    wifi_config_t wifi_config = {};
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", owner->wifi_ssid);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", owner->wifi_password);
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.failure_retry_cnt = CONFIG_RADAR_WIFI_MAX_RETRY;
    wifi_config.sta.threshold.authmode = owner->wifi_password[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err == ESP_OK) {
        owner->wifi_started = true;
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
    owner->wifi_recovering = false;

    xSemaphoreGive(owner->wifi_mutex);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Starting station WiFi failed: %s", esp_err_to_name(err));
        owner->set_data_status("WIFI ERR");
        return false;
    }

    return true;
}

/* Restart station WiFi after repeated HTTP/TLS failures. */
bool WifiManager::recoverAfterHttpFailure()
{
    if (!owner || !owner->wifi_event_group || owner->wifi_portal_active) {
        return false;
    }

    owner->set_data_status("NET RST");
    ESP_LOGW(TAG, "Resetting WiFi after HTTP failure");

    if (owner->wifi_mutex && xSemaphoreTake(owner->wifi_mutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
        return false;
    }

    owner->wifi_recovering = true;
    xEventGroupClearBits(owner->wifi_event_group, WIFI_CONNECTED_BIT);

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "WiFi disconnect during recovery failed: %s", esp_err_to_name(err));
    }

    vTaskDelay(pdMS_TO_TICKS(300));

    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "WiFi stop during recovery failed: %s", esp_err_to_name(err));
    } else {
        owner->wifi_started = false;
    }

    vTaskDelay(pdMS_TO_TICKS(700));

    err = esp_wifi_start();
    if (err == ESP_OK) {
        owner->wifi_started = true;
    }
    owner->wifi_recovering = false;

    if (owner->wifi_mutex) {
        xSemaphoreGive(owner->wifi_mutex);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi start during recovery failed: %s", esp_err_to_name(err));
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(owner->wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_HTTP_RESET_TIMEOUT_MS));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

/* Handle ESP-IDF WiFi and IP events for station and portal modes. */
void WifiManager::handleEvent(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (!owner) {
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "Portal client joined: " MACSTR, MAC2STR(event->mac));
        owner->set_portal_status("Open %s in a browser", WIFI_SETUP_AP_IP);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "Portal client left: " MACSTR, MAC2STR(event->mac));
        if (owner->wifi_portal_active) {
            owner->set_portal_status("Connect to %s then open %s", owner->setup_ap_ssid, WIFI_SETUP_AP_IP);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!owner->wifi_portal_active && owner->wifi_ssid[0] != '\0') {
            ESP_LOGI(TAG, "Connecting to WiFi SSID '%s'", owner->wifi_ssid);
            owner->set_data_status("WIFI");
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        uint8_t reason = event ? event->reason : 0;
        xEventGroupClearBits(owner->wifi_event_group, WIFI_CONNECTED_BIT);
        if (!owner->wifi_portal_active) {
            owner->settings_server.stop();
        }
        if (owner->wifi_recovering || owner->wifi_portal_active) {
            ESP_LOGW(TAG, "STA disconnected during WiFi transition: reason=%u (%s)",
                     reason, wifi_disconnect_reason_name(reason));
            return;
        }
        owner->wifi_retry_count++;
        if (owner->wifi_retry_count > CONFIG_RADAR_WIFI_MAX_RETRY) {
            owner->wifi_retry_count = 1;
        }
        ESP_LOGW(TAG, "STA disconnected: reason=%u (%s), retry %d/%d",
                 reason, wifi_disconnect_reason_name(reason),
                 owner->wifi_retry_count, CONFIG_RADAR_WIFI_MAX_RETRY);
        owner->set_data_status("WIFI RETRY");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        owner->wifi_retry_count = 0;
        xEventGroupSetBits(owner->wifi_event_group, WIFI_CONNECTED_BIT);
        owner->set_data_status("ONLINE");
        if (!owner->wifi_portal_active) {
            owner->settings_server.start();
        }
    }
}

/* Initialise netifs, WiFi driver, event handlers, and setup SSID state. */
bool WifiManager::init()
{
    if (!owner) {
        return false;
    }

    if (!owner->wifi_event_group) {
        owner->wifi_event_group = xEventGroupCreate();
    }
    if (!owner->wifi_mutex) {
        owner->wifi_mutex = xSemaphoreCreateMutex();
    }
    if (!owner->wifi_event_group || !owner->wifi_mutex) {
        owner->set_data_status("WIFI MEM");
        return false;
    }

    initSetupApSsid();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    if (!owner->wifi_sta_netif) {
        owner->wifi_sta_netif = esp_netif_create_default_wifi_sta();
    }
    if (!owner->wifi_ap_netif) {
        owner->wifi_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &RadarApp::wifi_event_handler_entry, owner, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &RadarApp::wifi_event_handler_entry, owner, NULL));

    return true;
}
