#include "Ssd1306StatusDisplay.hpp"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "ssd1306_status";

/* Return a compact 5x7 glyph, stored as vertical columns. */
const uint8_t *Ssd1306StatusDisplay::glyph(char ch)
{
    static const uint8_t space[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t dash[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t dot[5] = {0x00, 0x60, 0x60, 0x00, 0x00};
    static const uint8_t colon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const uint8_t zero[5] = {0x3e, 0x51, 0x49, 0x45, 0x3e};
    static const uint8_t one[5] = {0x00, 0x42, 0x7f, 0x40, 0x00};
    static const uint8_t two[5] = {0x42, 0x61, 0x51, 0x49, 0x46};
    static const uint8_t three[5] = {0x21, 0x41, 0x45, 0x4b, 0x31};
    static const uint8_t four[5] = {0x18, 0x14, 0x12, 0x7f, 0x10};
    static const uint8_t five[5] = {0x27, 0x45, 0x45, 0x45, 0x39};
    static const uint8_t six[5] = {0x3c, 0x4a, 0x49, 0x49, 0x30};
    static const uint8_t seven[5] = {0x01, 0x71, 0x09, 0x05, 0x03};
    static const uint8_t eight[5] = {0x36, 0x49, 0x49, 0x49, 0x36};
    static const uint8_t nine[5] = {0x06, 0x49, 0x49, 0x29, 0x1e};
    static const uint8_t a[5] = {0x7e, 0x11, 0x11, 0x11, 0x7e};
    static const uint8_t b[5] = {0x7f, 0x49, 0x49, 0x49, 0x36};
    static const uint8_t letter_c[5] = {0x3e, 0x41, 0x41, 0x41, 0x22};
    static const uint8_t d[5] = {0x7f, 0x41, 0x41, 0x22, 0x1c};
    static const uint8_t e[5] = {0x7f, 0x49, 0x49, 0x49, 0x41};
    static const uint8_t f[5] = {0x7f, 0x09, 0x09, 0x09, 0x01};
    static const uint8_t g[5] = {0x3e, 0x41, 0x49, 0x49, 0x7a};
    static const uint8_t h[5] = {0x7f, 0x08, 0x08, 0x08, 0x7f};
    static const uint8_t i[5] = {0x00, 0x41, 0x7f, 0x41, 0x00};
    static const uint8_t j[5] = {0x20, 0x40, 0x41, 0x3f, 0x01};
    static const uint8_t k[5] = {0x7f, 0x08, 0x14, 0x22, 0x41};
    static const uint8_t l[5] = {0x7f, 0x40, 0x40, 0x40, 0x40};
    static const uint8_t m[5] = {0x7f, 0x02, 0x0c, 0x02, 0x7f};
    static const uint8_t n[5] = {0x7f, 0x04, 0x08, 0x10, 0x7f};
    static const uint8_t o[5] = {0x3e, 0x41, 0x41, 0x41, 0x3e};
    static const uint8_t p[5] = {0x7f, 0x09, 0x09, 0x09, 0x06};
    static const uint8_t q[5] = {0x3e, 0x41, 0x51, 0x21, 0x5e};
    static const uint8_t r[5] = {0x7f, 0x09, 0x19, 0x29, 0x46};
    static const uint8_t s[5] = {0x46, 0x49, 0x49, 0x49, 0x31};
    static const uint8_t t[5] = {0x01, 0x01, 0x7f, 0x01, 0x01};
    static const uint8_t u[5] = {0x3f, 0x40, 0x40, 0x40, 0x3f};
    static const uint8_t v[5] = {0x1f, 0x20, 0x40, 0x20, 0x1f};
    static const uint8_t w[5] = {0x7f, 0x20, 0x18, 0x20, 0x7f};
    static const uint8_t x[5] = {0x63, 0x14, 0x08, 0x14, 0x63};
    static const uint8_t y[5] = {0x07, 0x08, 0x70, 0x08, 0x07};
    static const uint8_t z[5] = {0x61, 0x51, 0x49, 0x45, 0x43};

    switch (toupper((unsigned char)ch)) {
    case '0': return zero; case '1': return one; case '2': return two; case '3': return three;
    case '4': return four; case '5': return five; case '6': return six; case '7': return seven;
    case '8': return eight; case '9': return nine; case 'A': return a; case 'B': return b;
    case 'C': return letter_c; case 'D': return d; case 'E': return e; case 'F': return f;
    case 'G': return g; case 'H': return h; case 'I': return i; case 'J': return j;
    case 'K': return k; case 'L': return l; case 'M': return m; case 'N': return n;
    case 'O': return o; case 'P': return p; case 'Q': return q; case 'R': return r;
    case 'S': return s; case 'T': return t; case 'U': return u; case 'V': return v;
    case 'W': return w; case 'X': return x; case 'Y': return y; case 'Z': return z;
    case '.': return dot; case ':': return colon; case '-': return dash; default: return space;
    }
}

esp_err_t Ssd1306StatusDisplay::writeCommands(const uint8_t *commands, size_t count)
{
    if (!available || !device || !commands || count == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t packet[33];
    while (count > 0) {
        size_t chunk = count > sizeof(packet) - 1 ? sizeof(packet) - 1 : count;
        packet[0] = 0x00;
        memcpy(packet + 1, commands, chunk);
        esp_err_t err = i2c_master_transmit((i2c_master_dev_handle_t)device, packet, chunk + 1, 100);
        if (err != ESP_OK) {
            return err;
        }
        commands += chunk;
        count -= chunk;
    }
    return ESP_OK;
}

esp_err_t Ssd1306StatusDisplay::writeData(const uint8_t *data, size_t count)
{
    if (!available || !device || !data || count == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t packet[17];
    while (count > 0) {
        size_t chunk = count > sizeof(packet) - 1 ? sizeof(packet) - 1 : count;
        packet[0] = 0x40;
        memcpy(packet + 1, data, chunk);
        esp_err_t err = i2c_master_transmit((i2c_master_dev_handle_t)device, packet, chunk + 1, 100);
        if (err != ESP_OK) {
            return err;
        }
        data += chunk;
        count -= chunk;
    }
    return ESP_OK;
}

esp_err_t Ssd1306StatusDisplay::init(uint8_t i2c_address)
{
    if (device) {
        i2c_master_bus_rm_device((i2c_master_dev_handle_t)device);
        device = nullptr;
    }
    available = false;
    address = i2c_address;

    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BSP I2C init failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }

    err = i2c_master_probe(bus, address, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SSD1306 not found at 0x%02x: %s", address, esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = address;
    dev_cfg.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ;
    i2c_master_dev_handle_t handle = nullptr;
    err = i2c_master_bus_add_device(bus, &dev_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SSD1306 add device failed: %s", esp_err_to_name(err));
        return err;
    }

    device = handle;
    available = true;

    static const uint8_t init_cmds[] = {
        0xae, 0xd5, 0x80, 0xa8, 0x3f, 0xd3, 0x00, 0x40,
        0x8d, 0x14, 0x20, 0x02, 0xa1, 0xc8, 0xda, 0x12,
        0x81, 0xcf, 0xd9, 0xf1, 0xdb, 0x40, 0xa4, 0xa6,
        0x2e, 0xaf
    };
    err = writeCommands(init_cmds, sizeof(init_cmds));
    if (err != ESP_OK) {
        available = false;
        ESP_LOGW(TAG, "SSD1306 init commands failed: %s", esp_err_to_name(err));
        return err;
    }

    showStatus("STARTING");
    return ESP_OK;
}

void Ssd1306StatusDisplay::clearBuffer()
{
    memset(buffer, 0, sizeof(buffer));
}

void Ssd1306StatusDisplay::setPixel(int x, int y, bool on)
{
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) {
        return;
    }
    uint8_t mask = (uint8_t)(1U << (y & 7));
    uint8_t *cell = &buffer[x + ((y >> 3) * WIDTH)];
    if (on) {
        *cell |= mask;
    } else {
        *cell &= (uint8_t)~mask;
    }
}

void Ssd1306StatusDisplay::drawChar(int x, int y, char c, int scale)
{
    const uint8_t *g = glyph(c);
    for (int col = 0; col < 5; ++col) {
        for (int row = 0; row < 7; ++row) {
            if ((g[col] & (1U << row)) == 0) {
                continue;
            }
            for (int sx = 0; sx < scale; ++sx) {
                for (int sy = 0; sy < scale; ++sy) {
                    setPixel(x + (col * scale) + sx, y + (row * scale) + sy, true);
                }
            }
        }
    }
}

void Ssd1306StatusDisplay::drawText(int x, int y, const char *text, int scale)
{
    if (!text) {
        return;
    }
    int cursor = x;
    while (*text && cursor < WIDTH) {
        drawChar(cursor, y, *text++, scale);
        cursor += 6 * scale;
    }
}

void Ssd1306StatusDisplay::flush()
{
    if (!available) {
        return;
    }

    /*
     * Use page addressing rather than horizontal addressing. Some common
     * 128x64 modules sold as SSD1306 use SH1106-compatible page writes; page
     * mode is also accepted by SSD1306, so it is the safer option here.
     */
    for (uint8_t page = 0; page < HEIGHT / 8; ++page) {
        const uint8_t column = COLUMN_OFFSET;
        const uint8_t address_cmds[] = {
            (uint8_t)(0xb0 | page),
            (uint8_t)(0x00 | (column & 0x0f)),
            (uint8_t)(0x10 | (column >> 4))
        };
        if (writeCommands(address_cmds, sizeof(address_cmds)) != ESP_OK) {
            available = false;
            return;
        }
        if (writeData(&buffer[page * WIDTH], WIDTH) != ESP_OK) {
            available = false;
            return;
        }
    }
}

void Ssd1306StatusDisplay::showStatus(const char *status)
{
    if (!available) {
        return;
    }
    showDashboard("WIFI --", status && status[0] ? status : "WAITING", "", "", "");
}

void Ssd1306StatusDisplay::showIp(const char *ip, bool setup_ap)
{
    if (!available || !ip || ip[0] == '\0') {
        showStatus("NO WIFI");
        return;
    }

    char wifi[24];
    snprintf(wifi, sizeof(wifi), "%s %s", setup_ap ? "SETUP" : "WIFI", ip);
    showDashboard(wifi, "RADAR ONLINE", "", "", "");
}

void Ssd1306StatusDisplay::showDashboard(const char *wifi, const char *status1,
                                         const char *status2, const char *status3,
                                         const char *status4)
{
    if (!available) {
        return;
    }
    clearBuffer();
    drawText(0, 0, wifi && wifi[0] ? wifi : "WIFI --", 1);
    drawText(0, 14, status1 && status1[0] ? status1 : "RADAR WAIT", 1);
    drawText(0, 26, status2 ? status2 : "", 1);
    drawText(0, 38, status3 ? status3 : "", 1);
    drawText(0, 50, status4 ? status4 : "", 1);
    flush();
}
