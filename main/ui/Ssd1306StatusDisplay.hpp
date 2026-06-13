#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Tiny text-only SSD1306 status display.
 *
 * The radar already owns the main LVGL screen, so this class deliberately uses
 * a fixed 128x64 monochrome framebuffer and a small built-in 5x7 font. It is
 * only used for always-visible network status on an optional I2C OLED.
 */
class Ssd1306StatusDisplay {
public:
    /* Initialise the optional SSD1306 at the configured 7-bit address on the BSP I2C bus. */
    esp_err_t init(uint8_t i2c_address = 0x3c);

    /* Show a short status message, such as WIFI or SET WIFI. */
    void showStatus(const char *status);

    /* Show an IP address, split over two larger lines where possible. */
    void showIp(const char *ip, bool setup_ap);

    /* Show a compact multi-line network and radar status dashboard. */
    void showDashboard(const char *wifi, const char *status1,
                       const char *status2, const char *status3,
                       const char *status4);

private:
    static constexpr int WIDTH = 128;
    static constexpr int HEIGHT = 64;
    static constexpr int BUFFER_SIZE = WIDTH * HEIGHT / 8;
    static constexpr uint8_t COLUMN_OFFSET = 2;

    bool available = false;
    void *device = nullptr;
    uint8_t address = 0x3c;
    uint8_t buffer[BUFFER_SIZE] = {};

    /* Send command bytes to the SSD1306 command stream. */
    esp_err_t writeCommands(const uint8_t *commands, size_t count);

    /* Send one span of framebuffer bytes. */
    esp_err_t writeData(const uint8_t *data, size_t count);

    /* Clear the local framebuffer. */
    void clearBuffer();

    /* Push the local framebuffer to the display. */
    void flush();

    /* Draw one scaled character using the built-in 5x7 font. */
    void drawChar(int x, int y, char c, int scale);

    /* Draw a text string using the built-in 5x7 font. */
    void drawText(int x, int y, const char *text, int scale);

    /* Draw one framebuffer pixel. */
    void setPixel(int x, int y, bool on);

    /* Return a 5-column font glyph for a supported character. */
    static const uint8_t *glyph(char c);
};
