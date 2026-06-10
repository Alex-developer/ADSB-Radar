#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "CaptivePortal.hpp"
#include "RadarApp.hpp"

static const char *TAG = "captive_portal";

/*
 * The captive portal is intentionally simple and self-contained. It avoids
 * Bootstrap and external assets because the user may be connected only to the
 * ESP's setup AP with no internet route.
 */

/* Serve the captive portal page or redirect probes back to the setup address. */
esp_err_t CaptivePortal::getHandler(httpd_req_t *req)
{
    if (!owner) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Portal GET %s", req->uri);

    if (strcmp(req->uri, "/") != 0 && strcmp(req->uri, "/index.html") != 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://" WIFI_SETUP_AP_IP "/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    wifi_ap_record_t *aps = (wifi_ap_record_t *)heap_caps_calloc(MAX_WIFI_SCAN_RESULTS, sizeof(aps[0]),
                                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!aps) {
        aps = (wifi_ap_record_t *)heap_caps_calloc(MAX_WIFI_SCAN_RESULTS, sizeof(aps[0]), MALLOC_CAP_8BIT);
    }
    uint16_t ap_count = aps ? owner->wifi_manager.scanNetworks(aps, MAX_WIFI_SCAN_RESULTS) : 0;
    if (!aps) {
        ESP_LOGW(TAG, "No memory for portal network list");
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req,
                             "<!doctype html><html><head><meta name=\"viewport\" "
                             "content=\"width=device-width,initial-scale=1\">"
                             "<title>Radar WiFi Setup</title><style>"
                             "body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;"
                             "background:#06110b;color:#e8fff0;margin:0;padding:24px}"
                             "main{max-width:480px;margin:auto}"
                             "h1{font-size:26px;margin:0 0 8px}"
                             "p{color:#a7f3d0;line-height:1.4}"
                             "label{display:block;margin:18px 0 6px;color:#93c5fd;font-size:13px;"
                             "text-transform:uppercase;letter-spacing:.08em}"
                             "select,input,button{box-sizing:border-box;width:100%;font-size:18px;"
                             "border-radius:8px;border:1px solid #2dd4bf;padding:12px;"
                             "background:#0b1f17;color:#f8fafc}"
                             "button{margin-top:22px;background:#2dd4bf;color:#03110a;font-weight:700}"
                             ".hint{font-size:14px;color:#7dd3bf}.box{border:1px solid #164e35;"
                             "border-radius:8px;padding:14px;margin-top:18px;background:#081a12}"
                             "a{color:#86efac}</style></head><body><main>"
                             "<h1>Radar WiFi Setup</h1>"
                             "<p>Choose the WiFi network the radar should use for aircraft data.</p>"
                             "<form method=\"post\" action=\"/save\">"
                             "<label for=\"ssid\">Network</label><select id=\"ssid\" name=\"ssid\">"
                             "<option value=\"\">Select a network</option>");

    for (uint16_t i = 0; i < ap_count; ++i) {
        char ssid[33];
        snprintf(ssid, sizeof(ssid), "%s", (const char *)aps[i].ssid);
        char suffix[32];
        snprintf(suffix, sizeof(suffix), " (%d dBm)", aps[i].rssi);
        httpd_resp_sendstr_chunk(req, "<option value=\"");
        RadarApp::http_send_escaped(req, ssid);
        httpd_resp_sendstr_chunk(req, "\">");
        RadarApp::http_send_escaped(req, ssid);
        RadarApp::http_send_escaped(req, suffix);
        httpd_resp_sendstr_chunk(req, "</option>");
    }

    if (ap_count == 0) {
        httpd_resp_sendstr_chunk(req, "<option value=\"\">No networks found</option>");
    }

    httpd_resp_sendstr_chunk(req,
                             "</select><label for=\"manual\">Manual SSID</label>"
                             "<input id=\"manual\" name=\"manual\" maxlength=\"32\" "
                             "placeholder=\"Use only if the network is hidden\">"
                             "<label for=\"password\">Password</label>"
                             "<input id=\"password\" name=\"password\" maxlength=\"64\" "
                             "type=\"password\" autocomplete=\"current-password\">"
                             "<button type=\"submit\">Save and connect</button></form>"
                             "<div class=\"box\"><p class=\"hint\">If the list is empty, wait a few "
                             "seconds and refresh this page. Open networks can use a blank password.</p>"
                             "<p class=\"hint\"><a href=\"/\">Refresh networks</a></p></div>"
                             "</main></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    free(aps);
    return ESP_OK;
}

/* Save WiFi credentials posted by the captive portal form. */
esp_err_t CaptivePortal::saveHandler(httpd_req_t *req)
{
    if (!owner) {
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || req->content_len >= 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
        return ESP_OK;
    }

    char *body = (char *)heap_caps_calloc((size_t)req->content_len + 1, 1,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        body = (char *)heap_caps_calloc((size_t)req->content_len + 1, 1, MALLOC_CAP_8BIT);
    }
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_OK;
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_OK;
        }
        received += ret;
    }
    body[received] = '\0';

    char selected[33];
    char manual[33];
    char password[65];
    RadarApp::form_get_value(body, "ssid", selected, sizeof(selected));
    RadarApp::form_get_value(body, "manual", manual, sizeof(manual));
    RadarApp::form_get_value(body, "password", password, sizeof(password));

    const char *ssid = manual[0] ? manual : selected;
    if (!ssid || ssid[0] == '\0') {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_OK;
    }
    if (password[0] != '\0' && strlen(password) < 8) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password must be blank or at least 8 characters");
        return ESP_OK;
    }

    esp_err_t err = owner->wifi_manager.saveCredentials(ssid, password);
    free(body);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Saving WiFi credentials failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Saved WiFi credentials from portal");
    owner->set_data_status("WIFI SAVE");
    owner->set_portal_status("Saved. Reconnecting to WiFi");
    if (owner->wifi_event_group) {
        xEventGroupSetBits(owner->wifi_event_group, WIFI_CREDENTIALS_CHANGED_BIT);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req,
                       "<!doctype html><html><head><meta name=\"viewport\" "
                       "content=\"width=device-width,initial-scale=1\">"
                       "<title>Saved</title><style>body{font-family:system-ui;"
                       "background:#06110b;color:#e8fff0;padding:24px}</style></head>"
                       "<body><h1>Saved</h1><p>The radar is reconnecting. You can return to its display.</p>"
                       "</body></html>");
    return ESP_OK;
}

/* Start the HTTP server that serves the setup form on the AP interface. */
bool CaptivePortal::startHttpServer()
{
    if (!owner) {
        return false;
    }
    if (owner->portal_httpd) {
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 4;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&owner->portal_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Portal HTTP server failed: %s", esp_err_to_name(err));
        owner->portal_httpd = NULL;
        return false;
    }

    httpd_uri_t save_uri = {};
    save_uri.uri = "/save";
    save_uri.method = HTTP_POST;
    save_uri.handler = RadarApp::portal_save_handler_entry;
    save_uri.user_ctx = NULL;

    httpd_uri_t portal_uri = {};
    portal_uri.uri = "/*";
    portal_uri.method = HTTP_GET;
    portal_uri.handler = RadarApp::portal_get_handler_entry;
    portal_uri.user_ctx = NULL;

    if (httpd_register_uri_handler(owner->portal_httpd, &save_uri) != ESP_OK ||
        httpd_register_uri_handler(owner->portal_httpd, &portal_uri) != ESP_OK) {
        httpd_stop(owner->portal_httpd);
        owner->portal_httpd = NULL;
        return false;
    }

    owner->settings_http_active = false;
    return true;
}

/* Run a minimal DNS responder that points all queries at the setup AP. */
void CaptivePortal::dnsServerTask(void *arg)
{
    (void)arg;
    if (!owner) {
        vTaskDeleteWithCaps(NULL);
        return;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGW(TAG, "DNS socket failed");
        owner->portal_dns_running = false;
        owner->portal_dns_task_handle = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    struct timeval timeout = {};
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in listen_addr = {};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(53);
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        ESP_LOGW(TAG, "DNS bind failed");
        close(sock);
        owner->portal_dns_running = false;
        owner->portal_dns_task_handle = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    uint8_t rx[512];
    uint8_t tx[512];
    /*
     * Reply to every query with the setup AP address. That is enough for most
     * phones and laptops to offer the captive portal page automatically.
     */
    while (owner->portal_dns_running) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx, sizeof(rx), 0,
                           (struct sockaddr *)&source_addr, &socklen);
        if (len < 12) {
            continue;
        }

        int q_end = 12;
        while (q_end < len && rx[q_end] != 0) {
            q_end += rx[q_end] + 1;
        }
        if (q_end + 4 >= len) {
            continue;
        }

        int question_len = (q_end - 12) + 1 + 4;
        int response_len = 12 + question_len + 16;
        if (response_len > (int)sizeof(tx)) {
            continue;
        }

        memcpy(tx, rx, 12 + question_len);
        tx[2] = 0x81;
        tx[3] = 0x80;
        tx[6] = 0x00;
        tx[7] = 0x01;
        tx[8] = 0x00;
        tx[9] = 0x00;
        tx[10] = 0x00;
        tx[11] = 0x00;

        int p = 12 + question_len;
        tx[p++] = 0xc0;
        tx[p++] = 0x0c;
        tx[p++] = 0x00;
        tx[p++] = 0x01;
        tx[p++] = 0x00;
        tx[p++] = 0x01;
        tx[p++] = 0x00;
        tx[p++] = 0x00;
        tx[p++] = 0x00;
        tx[p++] = 0x3c;
        tx[p++] = 0x00;
        tx[p++] = 0x04;
        uint32_t ip = inet_addr(WIFI_SETUP_AP_IP);
        memcpy(&tx[p], &ip, sizeof(ip));
        p += 4;

        sendto(sock, tx, p, 0, (struct sockaddr *)&source_addr, socklen);
    }

    close(sock);
    owner->portal_dns_task_handle = NULL;
    vTaskDeleteWithCaps(NULL);
}

/* Start the captive portal DNS responder task. */
bool CaptivePortal::startDnsServer()
{
    if (!owner) {
        return false;
    }
    if (owner->portal_dns_running) {
        return true;
    }

    owner->portal_dns_running = true;
    if (xTaskCreateWithCaps(RadarApp::dns_server_task_entry, "portal_dns", DNS_TASK_STACK, owner,
                            DNS_TASK_PRIORITY, &owner->portal_dns_task_handle,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        owner->portal_dns_running = false;
        owner->portal_dns_task_handle = NULL;
        return false;
    }
    return true;
}

/* Stop captive portal HTTP and DNS services. */
void CaptivePortal::stopServices()
{
    if (!owner) {
        return;
    }

    if (owner->portal_httpd) {
        httpd_stop(owner->portal_httpd);
        owner->portal_httpd = NULL;
    }
    owner->settings_http_active = false;

    owner->portal_dns_running = false;
    for (int i = 0; i < 10 && owner->portal_dns_task_handle; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
