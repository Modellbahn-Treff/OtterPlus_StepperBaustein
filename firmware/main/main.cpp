// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Modellbahn-Treff for Kids GmbH

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "settings.h"
#include "stepper.h"
#include "display.h"
#include "serial_config.h"
#include "gpio_inputs.h"

static const char *TAG = "main";

esp_mqtt_client_handle_t mqtt_client = nullptr;

static bool s_wifi_ok = false;
static bool s_mqtt_ok = false;
static char s_ip_str[16] = "---";

// ---------------------------------------------------------------------------
// Screen button — GPIO26, active-low
// ---------------------------------------------------------------------------

static volatile bool s_screen_btn_flag = false;

static void IRAM_ATTR screen_btn_isr(void *) {
    s_screen_btn_flag = true;
}

static void setup_screen_button(void) {
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << GPIO_NUM_26);
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_NEGEDGE;
    gpio_config(&io);
    gpio_isr_handler_add(GPIO_NUM_26, screen_btn_isr, nullptr);
}

// ---------------------------------------------------------------------------
// Publish current motor state to MqttStatus
// ---------------------------------------------------------------------------

static void publish_status(void) {
    if (!mqtt_client || !s_mqtt_ok) return;
    char payload[64];
    snprintf(payload, sizeof(payload),
             "{\"rpm\":%.2f,\"running\":%s}",
             stepper_get_rpm(),
             stepper_is_running() ? "true" : "false");
    esp_mqtt_client_enqueue(mqtt_client, MqttStatus, payload, 0, 1, 0, true);
}

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------

static bool payload_is_start(const char *msg) {
    return strcmp(msg, "1") == 0
        || strcasecmp(msg, "true")  == 0
        || strcasecmp(msg, "start") == 0
        || strcasecmp(msg, "on")    == 0;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            s_mqtt_ok = true;
            esp_mqtt_client_subscribe(mqtt_client, MqttSpeed,     0);
            esp_mqtt_client_subscribe(mqtt_client, MqttStartStop, 0);
            esp_mqtt_client_subscribe(mqtt_client, MqttSet,       0);
            esp_mqtt_client_subscribe(mqtt_client, MqttRefresh,   0);
            publish_status();
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT disconnected");
            s_mqtt_ok = false;
            break;

        case MQTT_EVENT_DATA: {
            // topic and data are NOT null-terminated — copy to local buffers
            char topic[event->topic_len + 1];
            char msg[event->data_len + 1];
            memcpy(topic, event->topic, event->topic_len);
            topic[event->topic_len] = '\0';
            memcpy(msg, event->data, event->data_len);
            msg[event->data_len] = '\0';

            ESP_LOGI(TAG, "MQTT ← [%s] %s", topic, msg);

            if (strcmp(topic, MqttSpeed) == 0) {
                float rpm = strtof(msg, nullptr);
                if (rpm >  120.0f) rpm =  120.0f;
                if (rpm < -120.0f) rpm = -120.0f;
                stepper_set_rpm(rpm);
                last_speed      = rpm;
                last_was_running = true;
                settings_save_last_state(rpm, true);
                publish_status();
            }
            else if (strcmp(topic, MqttStartStop) == 0) {
                if (payload_is_start(msg)) {
                    if (last_speed != 0.0f) stepper_set_rpm(last_speed);
                    last_was_running = true;
                    settings_save_last_state(last_speed, true);
                } else {
                    stepper_stop();
                    last_was_running = false;
                    settings_save_last_state(last_speed, false);
                }
                publish_status();
            }
            else if (strcmp(topic, MqttSet) == 0) {
                char tx[64];
                settings_result_t result = settings_apply_json(msg, tx, sizeof(tx));

                char ack_topic[SETTINGS_TOPIC_LEN + 5];
                snprintf(ack_topic, sizeof(ack_topic), "%s/ack", MqttSet);

                char ack_payload[160];
                if (result == SETTINGS_OK || result == SETTINGS_OK_REBOOT) {
                    snprintf(ack_payload, sizeof(ack_payload), "{\"tx\":\"%s\"}", tx);
                    gpio_inputs_reinit();
                } else if (result == SETTINGS_ERR_NVS) {
                    snprintf(ack_payload, sizeof(ack_payload), "{\"tx\":\"%s\",\"error\":\"NVS write failed\"}", tx);
                } else {
                    snprintf(ack_payload, sizeof(ack_payload), "{\"tx\":\"%s\",\"error\":\"JSON parse error\"}", tx);
                }
                esp_mqtt_client_enqueue(mqtt_client, ack_topic, ack_payload, 0, 0, 0, true);

                if (result == SETTINGS_OK_REBOOT) {
                    xTaskCreate([](void *) {
                        vTaskDelay(pdMS_TO_TICKS(500));
                        esp_restart();
                    }, "reboot", 2048, nullptr, 5, nullptr);
                }
            }
            else if (strcmp(topic, MqttRefresh) == 0) {
                publish_status();
            }
            break;
        }

        default:
            break;
    }
}

static void setup_mqtt(void) {
    char uri[80];
    snprintf(uri, sizeof(uri), "mqtt://%s", mqtt_server);

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri    = uri;
    cfg.credentials.client_id = client_name;
    cfg.buffer.size           = 1024;

    mqtt_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(mqtt_client,
                                   (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, nullptr);
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_ok = false;
        s_mqtt_ok = false;
        ESP_LOGI(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "WiFi connected, IP: %s", s_ip_str);
        s_wifi_ok = true;
        esp_mqtt_client_start(mqtt_client);
    }
}

static void setup_wifi(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();

    // Static IP
    esp_netif_dhcpc_stop(sta);
    esp_netif_ip_info_t ip = {};
    ip.ip.addr      = ESP_IP4TOADDR(networkByte1, networkByte2, networkByte3, networkByte4);
    ip.gw.addr      = ESP_IP4TOADDR(networkByte1, networkByte2, gatewayByte3, gatewayByte4);
    ip.netmask.addr = ESP_IP4TOADDR(255, 255, 0, 0);
    esp_netif_set_ip_info(sta, &ip);

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wcfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,  wifi_event_handler, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr, nullptr);

    wifi_config_t wifi_cfg = {};
    strncpy((char *)wifi_cfg.sta.ssid,     ssid,     sizeof(wifi_cfg.sta.ssid));
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password));

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    ESP_LOGI(TAG, "Connecting to %s", ssid);
    esp_wifi_start();
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    settings_load_from_nvs();

    display_init();
    stepper_init();
    gpio_inputs_init();
    setup_screen_button();

    // Apply after-boot behaviour
    switch (boot_behaviour) {
        case BOOT_START:
            if (last_speed != 0.0f) {
                ESP_LOGI(TAG, "Boot: starting at %.2f RPM (boot_behaviour=START)", last_speed);
                stepper_set_rpm(last_speed);
            }
            break;
        case BOOT_LAST_STATE:
            if (last_was_running && last_speed != 0.0f) {
                ESP_LOGI(TAG, "Boot: restoring last state at %.2f RPM", last_speed);
                stepper_set_rpm(last_speed);
            }
            break;
        default:
            break;
    }

    setup_mqtt();
    setup_wifi();
    vTaskDelay(pdMS_TO_TICKS(10000));
    serial_config_start();

    // Main loop — refresh display every 250 ms
    TickType_t s_screen_btn_last = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (s_screen_btn_flag) {
            s_screen_btn_flag = false;
            TickType_t now = xTaskGetTickCount();
            if ((now - s_screen_btn_last) >= pdMS_TO_TICKS(400)) {
                s_screen_btn_last = now;
                display_next_screen();
            }
        }
        display_update(stepper_get_rpm(), stepper_is_running(), s_wifi_ok, s_mqtt_ok, s_ip_str);
    }
}
