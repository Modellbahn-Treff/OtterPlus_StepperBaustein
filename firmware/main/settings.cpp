#include "settings.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include <string.h>
#include <stdio.h>

static const char *TAG    = "settings";
static const char *NVS_NS = "settings";

// ---------------------------------------------------------------------------
// Default values (compiled-in fallbacks)
// ---------------------------------------------------------------------------

char ssid[SETTINGS_STR_LEN]        = "SSID";
char password[SETTINGS_STR_LEN]    = "PASSWORD";
char mqtt_server[SETTINGS_STR_LEN] = "10.1.0.5";
char client_name[SETTINGS_STR_LEN] = "stepper_01";

uint8_t networkByte1 = 10;
uint8_t networkByte2 = 1;
uint8_t networkByte3 = 0;
uint8_t networkByte4 = 10;
uint8_t gatewayByte3 = 0;
uint8_t gatewayByte4 = 1;

uint16_t steps_per_rev = 200;
uint8_t  irun          = 15;
uint8_t  ihold         = 10;

char MqttSpeed[SETTINGS_TOPIC_LEN]     = "otter/Stepper/speed";
char MqttStartStop[SETTINGS_TOPIC_LEN] = "otter/Stepper/startstop";
char MqttStatus[SETTINGS_TOPIC_LEN]    = "otter/Stepper/status";
char MqttSet[SETTINGS_TOPIC_LEN]       = "otter/Stepper/Set";
char MqttRefresh[SETTINGS_TOPIC_LEN]   = "otter/Refresh";

boot_behaviour_t boot_behaviour  = BOOT_STOP;
float            last_speed      = 0.0f;
bool             last_was_running = false;

pin_cfg_t pin_cfg[NUM_INPUT_PINS] = {
    { .gpio = 4,  .mode = PIN_MODE_DISABLED, .param = 0.0f },
    { .gpio = 5,  .mode = PIN_MODE_DISABLED, .param = 0.0f },
    { .gpio = 18, .mode = PIN_MODE_DISABLED, .param = 0.0f },
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void float_to_u32(float f, uint32_t *out) { memcpy(out, &f, 4); }
static void u32_to_float(uint32_t u, float *out)  { memcpy(out, &u, 4); }

// ---------------------------------------------------------------------------
// NVS load — falls back to compiled defaults for any missing key
// ---------------------------------------------------------------------------

void settings_load_from_nvs(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved settings, using defaults");
        return;
    }

    size_t len;
    len = SETTINGS_STR_LEN;  nvs_get_str(h, "ssid",      ssid,        &len);
    len = SETTINGS_STR_LEN;  nvs_get_str(h, "password",  password,    &len);
    len = SETTINGS_STR_LEN;  nvs_get_str(h, "mqtt_srv",  mqtt_server, &len);
    len = SETTINGS_STR_LEN;  nvs_get_str(h, "client_nm", client_name, &len);

    nvs_get_u8(h, "net_b1",   &networkByte1);
    nvs_get_u8(h, "net_b2",   &networkByte2);
    nvs_get_u8(h, "net_b3",   &networkByte3);
    nvs_get_u8(h, "net_b4",   &networkByte4);
    nvs_get_u8(h, "gw_b3",    &gatewayByte3);
    nvs_get_u8(h, "gw_b4",    &gatewayByte4);

    uint16_t spr = steps_per_rev;
    if (nvs_get_u16(h, "steps_rev", &spr) == ESP_OK) steps_per_rev = spr;
    nvs_get_u8(h, "irun",  &irun);
    nvs_get_u8(h, "ihold", &ihold);

    len = SETTINGS_TOPIC_LEN; nvs_get_str(h, "mqtt_speed",     MqttSpeed,     &len);
    len = SETTINGS_TOPIC_LEN; nvs_get_str(h, "mqtt_startstop", MqttStartStop, &len);
    len = SETTINGS_TOPIC_LEN; nvs_get_str(h, "mqtt_status",    MqttStatus,    &len);
    len = SETTINGS_TOPIC_LEN; nvs_get_str(h, "mqtt_set",       MqttSet,       &len);
    len = SETTINGS_TOPIC_LEN; nvs_get_str(h, "mqtt_refresh",   MqttRefresh,   &len);

    uint8_t bb = (uint8_t)boot_behaviour;
    if (nvs_get_u8(h, "boot_beh", &bb) == ESP_OK) boot_behaviour = (boot_behaviour_t)bb;

    uint32_t bits = 0;
    if (nvs_get_u32(h, "last_speed", &bits) == ESP_OK) u32_to_float(bits, &last_speed);
    uint8_t lwr = 0;
    if (nvs_get_u8(h, "last_run", &lwr) == ESP_OK) last_was_running = (bool)lwr;

    for (int i = 0; i < NUM_INPUT_PINS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "pin%d_gpio", i);
        nvs_get_u8(h, key, &pin_cfg[i].gpio);
        snprintf(key, sizeof(key), "pin%d_mode", i);
        uint8_t m = (uint8_t)pin_cfg[i].mode;
        if (nvs_get_u8(h, key, &m) == ESP_OK) pin_cfg[i].mode = (pin_mode_t)m;
        snprintf(key, sizeof(key), "pin%d_param", i);
        uint32_t p = 0;
        if (nvs_get_u32(h, key, &p) == ESP_OK) u32_to_float(p, &pin_cfg[i].param);
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Settings loaded from NVS");
}

// ---------------------------------------------------------------------------
// NVS save
// ---------------------------------------------------------------------------

bool settings_save_to_nvs(void) {
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));

    nvs_set_str(h, "mqtt_srv",  mqtt_server);
    nvs_set_str(h, "client_nm", client_name);

    nvs_set_u8(h, "net_b1",   networkByte1);
    nvs_set_u8(h, "net_b2",   networkByte2);
    nvs_set_u8(h, "net_b3",   networkByte3);
    nvs_set_u8(h, "net_b4",   networkByte4);
    nvs_set_u8(h, "gw_b3",    gatewayByte3);
    nvs_set_u8(h, "gw_b4",    gatewayByte4);

    nvs_set_u16(h, "steps_rev", steps_per_rev);
    nvs_set_u8(h,  "irun",  irun);
    nvs_set_u8(h,  "ihold", ihold);

    nvs_set_str(h, "mqtt_speed",     MqttSpeed);
    nvs_set_str(h, "mqtt_startstop", MqttStartStop);
    nvs_set_str(h, "mqtt_status",    MqttStatus);
    nvs_set_str(h, "mqtt_set",       MqttSet);
    nvs_set_str(h, "mqtt_refresh",   MqttRefresh);

    nvs_set_u8(h, "boot_beh", (uint8_t)boot_behaviour);

    uint32_t bits = 0;
    float_to_u32(last_speed, &bits);
    nvs_set_u32(h, "last_speed", bits);
    nvs_set_u8(h, "last_run", (uint8_t)last_was_running);

    for (int i = 0; i < NUM_INPUT_PINS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "pin%d_gpio", i);
        nvs_set_u8(h, key, pin_cfg[i].gpio);
        snprintf(key, sizeof(key), "pin%d_mode", i);
        nvs_set_u8(h, key, (uint8_t)pin_cfg[i].mode);
        snprintf(key, sizeof(key), "pin%d_param", i);
        uint32_t p = 0;
        float_to_u32(pin_cfg[i].param, &p);
        nvs_set_u32(h, key, p);
    }

    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Settings saved to NVS");
    return true;
}

void settings_save_wifi_to_nvs(void) {
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    nvs_set_str(h, "ssid",     ssid);
    nvs_set_str(h, "password", password);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "WiFi credentials saved to NVS");
}

// Persist only the motor runtime state — called frequently, keeps the key set minimal.
void settings_save_last_state(float rpm, bool running) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t bits = 0;
    float_to_u32(rpm, &bits);
    nvs_set_u32(h, "last_speed", bits);
    nvs_set_u8(h,  "last_run",   (uint8_t)running);
    nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------------------
// Apply JSON blob to settings and persist
// ---------------------------------------------------------------------------

settings_result_t settings_apply_json(const char *json_str, char *tx_out, size_t tx_out_len) {
    if (tx_out && tx_out_len > 0) tx_out[0] = '\0';

    cJSON *root = cJSON_Parse(json_str);
    if (!root) return SETTINGS_ERR_JSON;

    cJSON *item;

    if (tx_out && (item = cJSON_GetObjectItem(root, "tx")) && cJSON_IsString(item))
        snprintf(tx_out, tx_out_len, "%s", item->valuestring);

    if ((item = cJSON_GetObjectItem(root, "mqtt_server"))    && cJSON_IsString(item)) snprintf(mqtt_server,   SETTINGS_STR_LEN,   "%s", item->valuestring);
    if ((item = cJSON_GetObjectItem(root, "client_name"))    && cJSON_IsString(item)) snprintf(client_name,   SETTINGS_STR_LEN,   "%s", item->valuestring);
    if ((item = cJSON_GetObjectItem(root, "networkByte1"))   && cJSON_IsNumber(item)) networkByte1   = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "networkByte2"))   && cJSON_IsNumber(item)) networkByte2   = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "networkByte3"))   && cJSON_IsNumber(item)) networkByte3   = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "networkByte4"))   && cJSON_IsNumber(item)) networkByte4   = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "gatewayByte3"))   && cJSON_IsNumber(item)) gatewayByte3   = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "gatewayByte4"))   && cJSON_IsNumber(item)) gatewayByte4   = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "steps_per_rev"))  && cJSON_IsNumber(item)) steps_per_rev  = (uint16_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "irun"))           && cJSON_IsNumber(item)) irun           = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "ihold"))          && cJSON_IsNumber(item)) ihold          = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "MqttSpeed"))      && cJSON_IsString(item)) snprintf(MqttSpeed,     SETTINGS_TOPIC_LEN, "%s", item->valuestring);
    if ((item = cJSON_GetObjectItem(root, "MqttStartStop"))  && cJSON_IsString(item)) snprintf(MqttStartStop, SETTINGS_TOPIC_LEN, "%s", item->valuestring);
    if ((item = cJSON_GetObjectItem(root, "MqttStatus"))     && cJSON_IsString(item)) snprintf(MqttStatus,    SETTINGS_TOPIC_LEN, "%s", item->valuestring);
    if ((item = cJSON_GetObjectItem(root, "MqttSet"))        && cJSON_IsString(item)) snprintf(MqttSet,       SETTINGS_TOPIC_LEN, "%s", item->valuestring);
    if ((item = cJSON_GetObjectItem(root, "MqttRefresh"))    && cJSON_IsString(item)) snprintf(MqttRefresh,   SETTINGS_TOPIC_LEN, "%s", item->valuestring);

    if ((item = cJSON_GetObjectItem(root, "boot_behaviour")) && cJSON_IsNumber(item)) boot_behaviour = (boot_behaviour_t)item->valueint;

    for (int i = 0; i < NUM_INPUT_PINS; i++) {
        char key[20];
        snprintf(key, sizeof(key), "pin%d_gpio", i);
        if ((item = cJSON_GetObjectItem(root, key)) && cJSON_IsNumber(item)) pin_cfg[i].gpio = (uint8_t)item->valueint;
        snprintf(key, sizeof(key), "pin%d_mode", i);
        if ((item = cJSON_GetObjectItem(root, key)) && cJSON_IsNumber(item)) pin_cfg[i].mode = (pin_mode_t)item->valueint;
        snprintf(key, sizeof(key), "pin%d_param", i);
        if ((item = cJSON_GetObjectItem(root, key)) && cJSON_IsNumber(item)) pin_cfg[i].param = (float)item->valuedouble;
    }

    bool do_reboot = (item = cJSON_GetObjectItem(root, "reboot")) && cJSON_IsTrue(item);

    cJSON_Delete(root);

    if (!settings_save_to_nvs()) return SETTINGS_ERR_NVS;

    return do_reboot ? SETTINGS_OK_REBOOT : SETTINGS_OK;
}
