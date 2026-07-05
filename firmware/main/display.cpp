// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Modellbahn-Treff for Kids GmbH

#include "display.h"
#include "settings.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "display";

// ---------------------------------------------------------------------------
// Hardware constants
// ---------------------------------------------------------------------------

#define OLED_SDA      GPIO_NUM_21
#define OLED_SCL      GPIO_NUM_22
#define OLED_ADDR     0x3C
#define OLED_W        128
#define OLED_PAGES    8    // 64 px / 8 px per page

// ---------------------------------------------------------------------------
// 5×8 font — ASCII 0x20 to 0x7E, 5 column bytes per glyph.
// Each byte is a column, bit 0 = top pixel.
// Source: classic Adafruit GFX / public domain embedded font.
// ---------------------------------------------------------------------------

static const uint8_t font5x8[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // 0x20  ' '
    {0x00,0x00,0x5F,0x00,0x00}, // 0x21  '!'
    {0x00,0x07,0x00,0x07,0x00}, // 0x22  '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // 0x23  '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // 0x24  '$'
    {0x23,0x13,0x08,0x64,0x62}, // 0x25  '%'
    {0x36,0x49,0x55,0x22,0x50}, // 0x26  '&'
    {0x00,0x05,0x03,0x00,0x00}, // 0x27  '''
    {0x00,0x1C,0x22,0x41,0x00}, // 0x28  '('
    {0x00,0x41,0x22,0x1C,0x00}, // 0x29  ')'
    {0x14,0x08,0x3E,0x08,0x14}, // 0x2A  '*'
    {0x08,0x08,0x3E,0x08,0x08}, // 0x2B  '+'
    {0x00,0x50,0x30,0x00,0x00}, // 0x2C  ','
    {0x08,0x08,0x08,0x08,0x08}, // 0x2D  '-'
    {0x00,0x60,0x60,0x00,0x00}, // 0x2E  '.'
    {0x20,0x10,0x08,0x04,0x02}, // 0x2F  '/'
    {0x3E,0x51,0x49,0x45,0x3E}, // 0x30  '0'
    {0x00,0x42,0x7F,0x40,0x00}, // 0x31  '1'
    {0x42,0x61,0x51,0x49,0x46}, // 0x32  '2'
    {0x21,0x41,0x45,0x4B,0x31}, // 0x33  '3'
    {0x18,0x14,0x12,0x7F,0x10}, // 0x34  '4'
    {0x27,0x45,0x45,0x45,0x39}, // 0x35  '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // 0x36  '6'
    {0x01,0x71,0x09,0x05,0x03}, // 0x37  '7'
    {0x36,0x49,0x49,0x49,0x36}, // 0x38  '8'
    {0x06,0x49,0x49,0x29,0x1E}, // 0x39  '9'
    {0x00,0x36,0x36,0x00,0x00}, // 0x3A  ':'
    {0x00,0x56,0x36,0x00,0x00}, // 0x3B  ';'
    {0x08,0x14,0x22,0x41,0x00}, // 0x3C  '<'
    {0x14,0x14,0x14,0x14,0x14}, // 0x3D  '='
    {0x00,0x41,0x22,0x14,0x08}, // 0x3E  '>'
    {0x02,0x01,0x51,0x09,0x06}, // 0x3F  '?'
    {0x32,0x49,0x79,0x41,0x3E}, // 0x40  '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 0x41  'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 0x42  'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 0x43  'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 0x44  'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 0x45  'E'
    {0x7F,0x09,0x09,0x09,0x01}, // 0x46  'F'
    {0x3E,0x41,0x49,0x49,0x7A}, // 0x47  'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 0x48  'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 0x49  'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 0x4A  'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 0x4B  'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 0x4C  'L'
    {0x7F,0x02,0x0C,0x02,0x7F}, // 0x4D  'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 0x4E  'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 0x4F  'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 0x50  'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 0x51  'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 0x52  'R'
    {0x46,0x49,0x49,0x49,0x31}, // 0x53  'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 0x54  'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 0x55  'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 0x56  'V'
    {0x3F,0x40,0x38,0x40,0x3F}, // 0x57  'W'
    {0x63,0x14,0x08,0x14,0x63}, // 0x58  'X'
    {0x07,0x08,0x70,0x08,0x07}, // 0x59  'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 0x5A  'Z'
    {0x00,0x7F,0x41,0x41,0x00}, // 0x5B  '['
    {0x02,0x04,0x08,0x10,0x20}, // 0x5C  '\'
    {0x00,0x41,0x41,0x7F,0x00}, // 0x5D  ']'
    {0x04,0x02,0x01,0x02,0x04}, // 0x5E  '^'
    {0x40,0x40,0x40,0x40,0x40}, // 0x5F  '_'
    {0x00,0x01,0x02,0x04,0x00}, // 0x60  '`'
    {0x20,0x54,0x54,0x54,0x78}, // 0x61  'a'
    {0x7F,0x48,0x44,0x44,0x38}, // 0x62  'b'
    {0x38,0x44,0x44,0x44,0x20}, // 0x63  'c'
    {0x38,0x44,0x44,0x48,0x7F}, // 0x64  'd'
    {0x38,0x54,0x54,0x54,0x18}, // 0x65  'e'
    {0x08,0x7E,0x09,0x01,0x02}, // 0x66  'f'
    {0x0C,0x52,0x52,0x52,0x3E}, // 0x67  'g'
    {0x7F,0x08,0x04,0x04,0x78}, // 0x68  'h'
    {0x00,0x44,0x7D,0x40,0x00}, // 0x69  'i'
    {0x20,0x40,0x44,0x3D,0x00}, // 0x6A  'j'
    {0x7F,0x10,0x28,0x44,0x00}, // 0x6B  'k'
    {0x00,0x41,0x7F,0x40,0x00}, // 0x6C  'l'
    {0x7C,0x04,0x18,0x04,0x78}, // 0x6D  'm'
    {0x7C,0x08,0x04,0x04,0x78}, // 0x6E  'n'
    {0x38,0x44,0x44,0x44,0x38}, // 0x6F  'o'
    {0x7C,0x14,0x14,0x14,0x08}, // 0x70  'p'
    {0x08,0x14,0x14,0x18,0x7C}, // 0x71  'q'
    {0x7C,0x08,0x04,0x04,0x08}, // 0x72  'r'
    {0x48,0x54,0x54,0x54,0x20}, // 0x73  's'
    {0x04,0x3F,0x44,0x40,0x20}, // 0x74  't'
    {0x3C,0x40,0x40,0x20,0x7C}, // 0x75  'u'
    {0x1C,0x20,0x40,0x20,0x1C}, // 0x76  'v'
    {0x3C,0x40,0x30,0x40,0x3C}, // 0x77  'w'
    {0x44,0x28,0x10,0x28,0x44}, // 0x78  'x'
    {0x0C,0x50,0x50,0x50,0x3C}, // 0x79  'y'
    {0x44,0x64,0x54,0x4C,0x44}, // 0x7A  'z'
    {0x00,0x08,0x36,0x41,0x00}, // 0x7B  '{'
    {0x00,0x00,0x7F,0x00,0x00}, // 0x7C  '|'
    {0x00,0x41,0x36,0x08,0x00}, // 0x7D  '}'
    {0x10,0x08,0x08,0x10,0x08}, // 0x7E  '~'
};

// ---------------------------------------------------------------------------
// I2C / SSD1306 internals
// ---------------------------------------------------------------------------

static bool s_ok = false;

static esp_err_t oled_write(const uint8_t *data, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (OLED_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, data, len, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static void oled_cmd(uint8_t cmd) {
    uint8_t buf[2] = {0x00, cmd};   // control byte 0x00 = command stream
    oled_write(buf, 2);
}

// Set write cursor to (page, col) using horizontal addressing mode.
static void oled_set_pos(uint8_t page, uint8_t col) {
    uint8_t buf[6] = {
        0x00,
        (uint8_t)(0xB0 | (page & 0x07)),           // page address
        (uint8_t)(0x00 | (col & 0x0F)),             // lower nibble of col
        (uint8_t)(0x10 | ((col >> 4) & 0x0F)),      // upper nibble of col
    };
    oled_write(buf, 4);
}

// Write raw pixel bytes for one page row.
static void oled_write_data(const uint8_t *data, size_t len) {
    // Prefix with 0x40 (data stream control byte)
    uint8_t buf[len + 1];
    buf[0] = 0x40;
    memcpy(buf + 1, data, len);
    oled_write(buf, len + 1);
}

// Clear the entire display.
static void oled_clear(void) {
    static const uint8_t zeros[OLED_W] = {};
    for (uint8_t p = 0; p < OLED_PAGES; p++) {
        oled_set_pos(p, 0);
        oled_write_data(zeros, OLED_W);
    }
}

// Draw a single character at (page, col). Each glyph is 6 px wide (5 + 1 gap).
static void oled_draw_char(uint8_t page, uint8_t col, char c) {
    if (c < 0x20 || c > 0x7E) c = ' ';
    const uint8_t *glyph = font5x8[c - 0x20];
    uint8_t buf[6] = {glyph[0], glyph[1], glyph[2], glyph[3], glyph[4], 0x00};
    oled_set_pos(page, col);
    oled_write_data(buf, 6);
}

// Draw a string at (page, col). Returns column after last character.
static uint8_t oled_draw_str(uint8_t page, uint8_t col, const char *s) {
    while (*s && col < OLED_W) {
        oled_draw_char(page, col, *s++);
        col += 6;
    }
    return col;
}

// Erase the rest of a row from col to end.
static void oled_clear_eol(uint8_t page, uint8_t col) {
    if (col >= OLED_W) return;
    uint8_t zeros[OLED_W - col];
    memset(zeros, 0, sizeof(zeros));
    oled_set_pos(page, col);
    oled_write_data(zeros, sizeof(zeros));
}

// ---------------------------------------------------------------------------
// Screen state
// ---------------------------------------------------------------------------

typedef enum {
    SCREEN_MOTOR   = 0,
    SCREEN_NETWORK = 1,
    SCREEN_CONFIG  = 2,
    SCREEN_COUNT   = 3,
} display_screen_t;

static display_screen_t s_screen = SCREEN_MOTOR;

static void oled_clear_content(void) {
    static const uint8_t zeros[OLED_W] = {};
    for (uint8_t p = 1; p < OLED_PAGES; p++) {
        oled_set_pos(p, 0);
        oled_write_data(zeros, OLED_W);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void display_init(void) {
    i2c_config_t conf = {};
    conf.mode             = I2C_MODE_MASTER;
    conf.sda_io_num       = OLED_SDA;
    conf.scl_io_num       = OLED_SCL;
    conf.sda_pullup_en    = GPIO_PULLUP_DISABLE;
    conf.scl_pullup_en    = GPIO_PULLUP_DISABLE;
    conf.master.clk_speed = 400000;

    esp_err_t err = i2c_param_config(I2C_NUM_0, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C config failed: %s", esp_err_to_name(err));
        return;
    }
    err = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(err));
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    // SSD1306 initialisation sequence
    static const uint8_t init_cmds[] = {
        0x00,        // control byte: command stream
        0xAE,        // display OFF
        0xD5, 0x80,  // clock divide / oscillator frequency
        0xA8, 0x3F,  // multiplex ratio = 64 rows
        0xD3, 0x00,  // display offset = 0
        0x40,        // display start line = 0
        0x8D, 0x14,  // charge pump enable
        0x20, 0x02,  // page addressing mode
        0xA1,        // segment remap: col 127 → SEG0
        0xC8,        // COM output scan: remapped (top→bottom)
        0xDA, 0x12,  // COM pin config: alternative
        0x81, 0xCF,  // contrast = 207
        0xD9, 0xF1,  // pre-charge period
        0xDB, 0x40,  // VCOMH deselect level
        0xA4,        // display RAM content (not all-on)
        0xA6,        // normal display (not inverted)
        0xAF,        // display ON
    };
    oled_write(init_cmds, sizeof(init_cmds));

    oled_clear();
    s_ok = true;

    // Static header on row 0
    oled_draw_str(0, 0, "StepperBaustein");

    ESP_LOGI(TAG, "SSD1306 initialised");
}

void display_next_screen(void) {
    if (!s_ok) return;
    s_screen = (display_screen_t)((s_screen + 1) % SCREEN_COUNT);
    oled_clear_content();
}

void display_update(float rpm, bool running, bool wifi_ok, bool mqtt_ok, const char *ip_str) {
    if (!s_ok) return;

    char buf[24];
    uint8_t col;

    switch (s_screen) {

        case SCREEN_MOTOR:
            // Row 2: speed value
            oled_draw_str(2, 0, "Speed:");
            snprintf(buf, sizeof(buf), "%.2f rpm  ", rpm < 0 ? -rpm : rpm);
            col = oled_draw_str(2, 42, buf);
            oled_clear_eol(2, col);

            // Row 3: direction
            oled_draw_str(3, 0, "Dir:  ");
            if (!running)    oled_draw_str(3, 42, "---  ");
            else if (rpm >= 0) oled_draw_str(3, 42, "FWD  ");
            else               oled_draw_str(3, 42, "BWD  ");

            // Row 4: motor status
            oled_draw_str(4, 0, "Motor:");
            oled_draw_str(4, 42, running ? "RUN  " : "STOP ");

            // Row 6: connectivity
            oled_draw_str(6, 0,  wifi_ok ? "WiFi:OK  " : "WiFi:--  ");
            oled_draw_str(6, 66, mqtt_ok ? "MQTT:OK" : "MQTT:--");
            break;

        case SCREEN_NETWORK:
            // Row 2: WiFi status
            oled_draw_str(2, 0, "WiFi: ");
            col = oled_draw_str(2, 36, wifi_ok ? "OK   " : "--   ");
            oled_clear_eol(2, col);

            // Row 3: IP address
            oled_draw_str(3, 0, "IP:   ");
            col = oled_draw_str(3, 36, ip_str ? ip_str : "---");
            oled_clear_eol(3, col);

            // Row 4: MQTT broker
            oled_draw_str(4, 0, "MQTT: ");
            col = oled_draw_str(4, 36, mqtt_server);
            oled_clear_eol(4, col);

            // Row 5: device ID
            oled_draw_str(5, 0, "ID:   ");
            col = oled_draw_str(5, 36, client_name);
            oled_clear_eol(5, col);
            break;

        case SCREEN_CONFIG: {
            // Row 2: steps per revolution
            oled_draw_str(2, 0, "Steps:");
            snprintf(buf, sizeof(buf), "%u    ", steps_per_rev);
            col = oled_draw_str(2, 42, buf);
            oled_clear_eol(2, col);

            // Row 3: run current
            oled_draw_str(3, 0, "Irun: ");
            snprintf(buf, sizeof(buf), "%u    ", irun);
            col = oled_draw_str(3, 42, buf);
            oled_clear_eol(3, col);

            // Row 4: hold current
            oled_draw_str(4, 0, "Ihold:");
            snprintf(buf, sizeof(buf), "%u    ", ihold);
            col = oled_draw_str(4, 42, buf);
            oled_clear_eol(4, col);

            // Row 5: boot behavior
            oled_draw_str(5, 0, "Boot: ");
            const char *boot_str;
            switch (boot_behaviour) {
                case BOOT_START:      boot_str = "START"; break;
                case BOOT_LAST_STATE: boot_str = "LAST "; break;
                default:              boot_str = "STOP "; break;
            }
            col = oled_draw_str(5, 42, boot_str);
            oled_clear_eol(5, col);
            break;
        }

        default:
            break;
    }

    // Row 7: page indicator centered
    snprintf(buf, sizeof(buf), "< %d/%d >", s_screen + 1, (int)SCREEN_COUNT);
    uint8_t ind_col = (OLED_W - (uint8_t)(strlen(buf) * 6)) / 2;
    oled_draw_str(7, ind_col, buf);
}
