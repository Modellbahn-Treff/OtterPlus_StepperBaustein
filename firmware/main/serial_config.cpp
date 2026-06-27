#include "serial_config.h"
#include "settings.h"
#include "stepper.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

static const char *TAG = "serial_cfg";

#define LINE_BUF_SIZE 1024
#define UART_BUF_SIZE 1024

static void uart_print(const char *s) {
    uart_write_bytes(UART_NUM_0, s, strlen(s));
}

static void uart_println(const char *s) {
    uart_print(s);
    uart_print("\r\n");
}

// ---------------------------------------------------------------------------
// Command: get
// ---------------------------------------------------------------------------

static void cmd_get(void) {
    cJSON *root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "ssid",         ssid);
    cJSON_AddStringToObject(root, "password",      "***");
    cJSON_AddStringToObject(root, "mqtt_server",   mqtt_server);
    cJSON_AddStringToObject(root, "client_name",   client_name);
    cJSON_AddNumberToObject(root, "networkByte1",  networkByte1);
    cJSON_AddNumberToObject(root, "networkByte2",  networkByte2);
    cJSON_AddNumberToObject(root, "networkByte3",  networkByte3);
    cJSON_AddNumberToObject(root, "networkByte4",  networkByte4);
    cJSON_AddNumberToObject(root, "gatewayByte3",  gatewayByte3);
    cJSON_AddNumberToObject(root, "gatewayByte4",  gatewayByte4);
    cJSON_AddNumberToObject(root, "steps_per_rev", steps_per_rev);
    cJSON_AddNumberToObject(root, "irun",          irun);
    cJSON_AddNumberToObject(root, "ihold",         ihold);
    cJSON_AddStringToObject(root, "MqttSpeed",      MqttSpeed);
    cJSON_AddStringToObject(root, "MqttStartStop", MqttStartStop);
    cJSON_AddStringToObject(root, "MqttStatus",    MqttStatus);
    cJSON_AddStringToObject(root, "MqttSet",       MqttSet);
    cJSON_AddStringToObject(root, "MqttRefresh",   MqttRefresh);
    cJSON_AddNumberToObject(root, "boot_behaviour", (int)boot_behaviour);
    cJSON_AddNumberToObject(root, "last_speed",     last_speed);
    cJSON_AddBoolToObject(root,   "last_was_running", last_was_running);

    for (int i = 0; i < NUM_INPUT_PINS; i++) {
        char key[20];
        snprintf(key, sizeof(key), "pin%d_gpio", i);
        cJSON_AddNumberToObject(root, key, pin_cfg[i].gpio);
        snprintf(key, sizeof(key), "pin%d_mode", i);
        cJSON_AddNumberToObject(root, key, (int)pin_cfg[i].mode);
        snprintf(key, sizeof(key), "pin%d_param", i);
        cJSON_AddNumberToObject(root, key, pin_cfg[i].param);
    }

    // Live motor state
    cJSON_AddNumberToObject(root, "current_rpm",   stepper_get_rpm());
    cJSON_AddBoolToObject(root,   "running",        stepper_is_running());

    char *s = cJSON_Print(root);
    uart_println(s);
    cJSON_free(s);
    cJSON_Delete(root);
}

// ---------------------------------------------------------------------------
// Command: set {...}
// ---------------------------------------------------------------------------

static void cmd_set(const char *json_str) {
    char tx[64];
    settings_result_t result = settings_apply_json(json_str, tx, sizeof(tx));
    if (result == SETTINGS_ERR_JSON)      uart_println("ERROR: JSON parse failed");
    else if (result == SETTINGS_ERR_NVS)  uart_println("ERROR: NVS write failed");
    else                                   uart_println("OK: Settings saved. Restart to apply.");
}

// ---------------------------------------------------------------------------
// Command: wifi {...}
// ---------------------------------------------------------------------------

static void cmd_wifi(const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) { uart_println("ERROR: JSON parse failed"); return; }

    cJSON *item;
    bool changed = false;
    if ((item = cJSON_GetObjectItem(root, "ssid"))     && cJSON_IsString(item)) { snprintf(ssid,     SETTINGS_STR_LEN, "%s", item->valuestring); changed = true; }
    if ((item = cJSON_GetObjectItem(root, "password")) && cJSON_IsString(item)) { snprintf(password, SETTINGS_STR_LEN, "%s", item->valuestring); changed = true; }
    cJSON_Delete(root);

    if (!changed) { uart_println("ERROR: No 'ssid' or 'password' key found"); return; }
    settings_save_wifi_to_nvs();
    uart_println("OK: WiFi credentials saved. Restarting now...");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

// ---------------------------------------------------------------------------
// Command: motor set rpm / stop
// ---------------------------------------------------------------------------

static void cmd_motor(const char *args) {
    // motor rpm <value>   or   motor stop
    char sub[16] = {};
    sscanf(args, "%15s", sub);
    if (strncasecmp(sub, "stop", 4) == 0) {
        stepper_stop();
        uart_println("OK: Motor stopped");
    } else if (strncasecmp(sub, "rpm", 3) == 0) {
        float rpm = 0.0f;
        if (sscanf(args + 3, "%f", &rpm) == 1) {
            stepper_set_rpm(rpm);
            char msg[48];
            snprintf(msg, sizeof(msg), "OK: Motor set to %.2f rpm", rpm);
            uart_println(msg);
        } else {
            uart_println("ERROR: usage: motor rpm <value>");
        }
    } else {
        uart_println("ERROR: usage: motor rpm <value>  |  motor stop");
    }
}

// ---------------------------------------------------------------------------
// Command: reset
// ---------------------------------------------------------------------------

static void cmd_reset(void) {
    nvs_handle_t h;
    if (nvs_open("settings", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    uart_println("OK: Settings erased. Restarting with factory defaults...");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

// ---------------------------------------------------------------------------
// Command: help
// ---------------------------------------------------------------------------

static void cmd_help(void) {
    uart_println("");
    uart_println("Serial Configuration Console — StepperBaustein");
    uart_println("-----------------------------------------------");
    uart_println("  get                          - Print all settings and live motor state as JSON");
    uart_println("  set {JSON}                   - Update settings, save to NVS");
    uart_println("  wifi {\"ssid\":\"x\",\"password\":\"y\"} - Update WiFi credentials, restart");
    uart_println("  motor rpm <value>            - Set motor speed in RPM (sign = direction)");
    uart_println("  motor stop                   - Stop motor and disable driver");
    uart_println("  restart                      - Restart the device");
    uart_println("  reset                        - Erase saved settings, restart with defaults");
    uart_println("  help                         - Show this message");
    uart_println("");
    uart_println("set JSON keys (all optional):");
    uart_println("  mqtt_server, client_name");
    uart_println("  networkByte1-4, gatewayByte3-4");
    uart_println("  steps_per_rev (default 200), irun (0-31), ihold (0-31)");
    uart_println("  MqttSpeed, MqttStartStop, MqttStatus, MqttSet, MqttRefresh");
    uart_println("  boot_behaviour: 0=stop, 1=start at last_speed, 2=restore last state");
    uart_println("  pin0_gpio, pin0_mode, pin0_param  (same for pin1, pin2)");
    uart_println("    modes: 0=off, 1=toggle, 2=switch, 3=switch_inv,");
    uart_println("           4=btn_faster, 5=btn_slower, 6=btn_stop, 7=btn_start, 8=btn_set_speed");
    uart_println("    param: toggle→0=coast/1=full-stop; faster/slower→delta RPM; set_speed→target RPM");
    uart_println("  reboot: true  — restart after saving");
    uart_println("");
    uart_println("MQTT topics (defaults):");
    uart_println("  otter/Stepper/speed     payload: RPM as float (sign = direction); last value persisted");
    uart_println("  otter/Stepper/startstop payload: 1/true/start/on = start, 0/false/stop/off = stop");
    uart_println("  otter/Stepper/status    published on state change");
}

// ---------------------------------------------------------------------------
// Dispatcher
// ---------------------------------------------------------------------------

static void dispatch(char *line) {
    while (*line && isspace((unsigned char)*line)) line++;
    if (*line == '\0') return;

    if (strncasecmp(line, "get", 3) == 0 && (line[3] == '\0' || isspace((unsigned char)line[3]))) {
        cmd_get();
    } else if (strncasecmp(line, "set", 3) == 0 && isspace((unsigned char)line[3])) {
        cmd_set(line + 4);
    } else if (strncasecmp(line, "wifi", 4) == 0 && isspace((unsigned char)line[4])) {
        cmd_wifi(line + 5);
    } else if (strncasecmp(line, "motor", 5) == 0 && isspace((unsigned char)line[5])) {
        cmd_motor(line + 6);
    } else if (strncasecmp(line, "restart", 7) == 0) {
        uart_println("Restarting...");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else if (strncasecmp(line, "reset", 5) == 0) {
        cmd_reset();
    } else if (strncasecmp(line, "help", 4) == 0) {
        cmd_help();
    } else {
        uart_println("ERROR: Unknown command. Type 'help' for usage.");
    }
}

// ---------------------------------------------------------------------------
// UART task
// ---------------------------------------------------------------------------

static void serial_config_task(void *) {
    uart_config_t cfg = {};
    cfg.baud_rate  = 115200;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    uart_param_config(UART_NUM_0, &cfg);
    uart_driver_install(UART_NUM_0, UART_BUF_SIZE, 0, 0, NULL, 0);

    uart_println("\r\nSerial config ready. Type 'help' for commands.");

    static char line[LINE_BUF_SIZE];
    int pos = 0;

    while (true) {
        uint8_t ch;
        if (uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(100)) <= 0) continue;

        uart_write_bytes(UART_NUM_0, (const char *)&ch, 1);

        if (ch == '\n' || ch == '\r') {
            uart_print("\r\n");
            line[pos] = '\0';
            if (pos > 0) dispatch(line);
            pos = 0;
        } else if (ch == 127 || ch == '\b') {
            if (pos > 0) { pos--; uart_print("\b \b"); }
        } else if (pos < LINE_BUF_SIZE - 1) {
            line[pos++] = (char)ch;
        }
    }
}

void serial_config_start(void) {
    xTaskCreate(serial_config_task, "serial_cfg", 8192, NULL, 5, NULL);
}
