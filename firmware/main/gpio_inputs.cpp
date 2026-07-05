// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Modellbahn-Treff for Kids GmbH

#include "gpio_inputs.h"
#include "settings.h"
#include "stepper.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "gpio_in";

#define DEBOUNCE_US 50000  // 50 ms

typedef struct {
    uint8_t pin_idx;
    int     level;
} gpio_event_t;

static QueueHandle_t s_queue = NULL;
static int64_t       s_last_trigger_us[NUM_INPUT_PINS] = {};
static uint8_t       s_active_gpio[NUM_INPUT_PINS]     = { 0xFF, 0xFF, 0xFF };
static bool          s_isr_installed[NUM_INPUT_PINS]   = {};
static bool          s_isr_service_up                  = false;

// ---------------------------------------------------------------------------
// ISR — only runs for edge-triggered pins
// ---------------------------------------------------------------------------

static void IRAM_ATTR gpio_isr(void *arg) {
    uint8_t idx = (uint8_t)(uintptr_t)arg;
    gpio_event_t evt = {
        .pin_idx = idx,
        .level   = gpio_get_level((gpio_num_t)s_active_gpio[idx]),
    };
    xQueueSendFromISR(s_queue, &evt, NULL);
}

// ---------------------------------------------------------------------------
// Event handler (runs in task context — safe to call stepper functions)
// ---------------------------------------------------------------------------

static void handle_event(uint8_t idx, int level) {
    int64_t now = esp_timer_get_time();
    if (now - s_last_trigger_us[idx] < DEBOUNCE_US) return;
    s_last_trigger_us[idx] = now;

    const pin_cfg_t *cfg = &pin_cfg[idx];

    switch (cfg->mode) {
        case PIN_MODE_TOGGLE:
            if (level != 0) return;  // active-low button: act on falling edge
            if (stepper_is_running()) {
                if (cfg->param >= 0.5f) {
                    stepper_stop();
                    last_was_running = false;
                    settings_save_last_state(last_speed, false);
                } else {
                    stepper_set_rpm(0.0f);
                    last_was_running = false;
                    settings_save_last_state(last_speed, false);
                }
            } else {
                if (last_speed != 0.0f) {
                    stepper_set_rpm(last_speed);
                    last_was_running = true;
                    settings_save_last_state(last_speed, true);
                }
            }
            break;

        case PIN_MODE_SWITCH:
            if (level == 0) {  // LOW = on
                if (last_speed != 0.0f) {
                    stepper_set_rpm(last_speed);
                    last_was_running = true;
                    settings_save_last_state(last_speed, true);
                }
            } else {           // HIGH = off
                stepper_stop();
                last_was_running = false;
                settings_save_last_state(last_speed, false);
            }
            break;

        case PIN_MODE_SWITCH_INV:
            if (level != 0) {  // HIGH = on
                if (last_speed != 0.0f) {
                    stepper_set_rpm(last_speed);
                    last_was_running = true;
                    settings_save_last_state(last_speed, true);
                }
            } else {           // LOW = off
                stepper_stop();
                last_was_running = false;
                settings_save_last_state(last_speed, false);
            }
            break;

        case PIN_MODE_BTN_FASTER:
            if (level != 0) return;
            {
                float new_rpm = stepper_get_rpm() + cfg->param;
                if (new_rpm >  120.0f) new_rpm =  120.0f;
                if (new_rpm < -120.0f) new_rpm = -120.0f;
                stepper_set_rpm(new_rpm);
                last_speed = new_rpm;
                last_was_running = true;
                settings_save_last_state(new_rpm, true);
            }
            break;

        case PIN_MODE_BTN_SLOWER:
            if (level != 0) return;
            {
                float new_rpm = stepper_get_rpm() - cfg->param;
                if (new_rpm >  120.0f) new_rpm =  120.0f;
                if (new_rpm < -120.0f) new_rpm = -120.0f;
                stepper_set_rpm(new_rpm);
                last_speed = new_rpm;
                last_was_running = true;
                settings_save_last_state(new_rpm, true);
            }
            break;

        case PIN_MODE_BTN_STOP:
            if (level != 0) return;
            stepper_stop();
            last_was_running = false;
            settings_save_last_state(last_speed, false);
            break;

        case PIN_MODE_BTN_START:
            if (level != 0) return;
            if (last_speed != 0.0f) {
                stepper_set_rpm(last_speed);
                last_was_running = true;
                settings_save_last_state(last_speed, true);
            }
            break;

        case PIN_MODE_BTN_SET_SPEED:
            if (level != 0) return;
            stepper_set_rpm(cfg->param);
            last_speed = cfg->param;
            last_was_running = (cfg->param != 0.0f);
            settings_save_last_state(cfg->param, last_was_running);
            break;

        default:
            break;
    }
}

static void gpio_input_task(void *) {
    gpio_event_t evt;
    while (true) {
        if (xQueueReceive(s_queue, &evt, portMAX_DELAY))
            handle_event(evt.pin_idx, evt.level);
    }
}

// ---------------------------------------------------------------------------
// Pin setup / teardown
// ---------------------------------------------------------------------------

static void uninstall_pin(int idx) {
    if (!s_isr_installed[idx]) return;
    gpio_isr_handler_remove((gpio_num_t)s_active_gpio[idx]);
    gpio_set_intr_type((gpio_num_t)s_active_gpio[idx], GPIO_INTR_DISABLE);
    s_isr_installed[idx] = false;
    s_active_gpio[idx]   = 0xFF;
}

static void configure_pin(int idx) {
    const pin_cfg_t *cfg = &pin_cfg[idx];
    gpio_num_t pin = (gpio_num_t)cfg->gpio;

    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << pin);
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&io);

    if (cfg->mode == PIN_MODE_DISABLED) {
        ESP_LOGI(TAG, "pin%d (GPIO%d): disabled", idx, cfg->gpio);
        return;
    }

    gpio_int_type_t intr;
    if (cfg->mode == PIN_MODE_SWITCH || cfg->mode == PIN_MODE_SWITCH_INV) {
        intr = GPIO_INTR_ANYEDGE;
    } else {
        intr = GPIO_INTR_NEGEDGE;  // all button modes: active-low with internal pull-up
    }

    gpio_set_intr_type(pin, intr);
    gpio_isr_handler_add(pin, gpio_isr, (void *)(uintptr_t)idx);
    s_active_gpio[idx]   = cfg->gpio;
    s_isr_installed[idx] = true;
    s_last_trigger_us[idx] = 0;

    ESP_LOGI(TAG, "pin%d (GPIO%d): mode=%d param=%.2f", idx, cfg->gpio, cfg->mode, cfg->param);

    // For switch modes, read and apply the current level immediately.
    if (cfg->mode == PIN_MODE_SWITCH || cfg->mode == PIN_MODE_SWITCH_INV) {
        gpio_event_t evt = { .pin_idx = (uint8_t)idx, .level = gpio_get_level(pin) };
        xQueueSend(s_queue, &evt, 0);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void gpio_inputs_init(void) {
    if (!s_queue) {
        s_queue = xQueueCreate(16, sizeof(gpio_event_t));
        xTaskCreate(gpio_input_task, "gpio_in", 2048, NULL, 5, NULL);
    }

    if (!s_isr_service_up) {
        gpio_install_isr_service(0);
        s_isr_service_up = true;
    }

    for (int i = 0; i < NUM_INPUT_PINS; i++)
        configure_pin(i);
}

void gpio_inputs_reinit(void) {
    for (int i = 0; i < NUM_INPUT_PINS; i++)
        uninstall_pin(i);
    for (int i = 0; i < NUM_INPUT_PINS; i++)
        configure_pin(i);
}
