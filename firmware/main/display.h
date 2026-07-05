// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Modellbahn-Treff for Kids GmbH

#pragma once
#include <stdbool.h>

// Initialise I2C and the SSD1306 128×64 OLED (addr 0x3C, SDA=21, SCL=22).
void display_init(void);

// Advance to the next screen and clear content rows.
void display_next_screen(void);

// Redraw the current screen.
//   rpm     – currently commanded speed (sign = direction)
//   running – true when driver is enabled and motor is turning
//   wifi_ok – true when WiFi is connected
//   mqtt_ok – true when MQTT broker is connected
//   ip_str  – dotted-decimal IP string, or "---" when not connected
void display_update(float rpm, bool running, bool wifi_ok, bool mqtt_ok, const char *ip_str);
