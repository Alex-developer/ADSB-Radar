#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

class RadarApp;

/*
 * SoftAP captive portal used for first-time WiFi setup and "change WiFi".
 *
 * The portal serves a minimal form and a DNS responder that redirects common
 * captive-portal probes back to the ESP. It is intentionally separate from the
 * main settings server because station WiFi may not be connected yet.
 */
class CaptivePortal {
public:
    /* Attach the owning application that stores credentials and portal state. */
    void bind(RadarApp *app) { owner = app; }

    /* Serve the setup form. */
    esp_err_t getHandler(httpd_req_t *req);

    /* Save submitted credentials and request a station reconnect. */
    esp_err_t saveHandler(httpd_req_t *req);

    /* Start the HTTP server bound to the setup AP. */
    bool startHttpServer();

    /* DNS responder task body for captive-portal redirection. */
    void dnsServerTask(void *arg);

    /* Start the DNS responder. */
    bool startDnsServer();

    /* Stop HTTP and DNS services for the setup AP. */
    void stopServices();

private:
    RadarApp *owner = nullptr;
};
