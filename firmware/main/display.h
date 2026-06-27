#pragma once
#include <stdbool.h>

// Initialise I2C and the SSD1306 128×64 OLED (addr 0x3C, SDA=21, SCL=22).
void display_init(void);

// Redraw the status screen.
//   rpm     – currently commanded speed (sign = direction)
//   running – true when driver is enabled and motor is turning
//   wifi_ok – true when WiFi is connected
//   mqtt_ok – true when MQTT broker is connected
void display_update(float rpm, bool running, bool wifi_ok, bool mqtt_ok);
