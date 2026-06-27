#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SETTINGS_STR_LEN   64
#define SETTINGS_TOPIC_LEN 64

extern char ssid[SETTINGS_STR_LEN];
extern char password[SETTINGS_STR_LEN];
extern char mqtt_server[SETTINGS_STR_LEN];
extern char client_name[SETTINGS_STR_LEN];

extern uint8_t networkByte1;
extern uint8_t networkByte2;
extern uint8_t networkByte3;
extern uint8_t networkByte4;
extern uint8_t gatewayByte3;
extern uint8_t gatewayByte4;

// Motor parameters
extern uint16_t steps_per_rev;   // full steps per revolution (typ. 200)
extern uint8_t  irun;             // TMC2208 run current 0-31
extern uint8_t  ihold;            // TMC2208 hold current 0-31

// MQTT topics
extern char MqttSpeed[SETTINGS_TOPIC_LEN];      // payload: RPM as float string (sign = direction); last value is persisted
extern char MqttStartStop[SETTINGS_TOPIC_LEN];  // payload: "1"/"true"/"start"/"on" = start at last speed; "0"/"false"/"stop"/"off" = stop
extern char MqttStatus[SETTINGS_TOPIC_LEN];     // published on state change
extern char MqttSet[SETTINGS_TOPIC_LEN];        // JSON settings update
extern char MqttRefresh[SETTINGS_TOPIC_LEN];    // trigger status republish

// After-boot behaviour
typedef enum {
    BOOT_STOP       = 0,  // stay stopped (default)
    BOOT_START      = 1,  // start at last_speed
    BOOT_LAST_STATE = 2,  // restore running/stopped state from before power-off
} boot_behaviour_t;

extern boot_behaviour_t boot_behaviour;
extern float            last_speed;        // last commanded RPM; persisted to NVS
extern bool             last_was_running;  // was the motor running before power-off; persisted to NVS

// Input pin modes
typedef enum {
    PIN_MODE_DISABLED     = 0,
    PIN_MODE_TOGGLE       = 1,  // falling edge toggles start/stop; param: 0=coast (VACTUAL=0), 1=full stop (driver off)
    PIN_MODE_SWITCH       = 2,  // LOW=on at last_speed, HIGH=off (full stop)
    PIN_MODE_SWITCH_INV   = 3,  // HIGH=on at last_speed, LOW=off (full stop)
    PIN_MODE_BTN_FASTER   = 4,  // falling edge: increase speed by param RPM
    PIN_MODE_BTN_SLOWER   = 5,  // falling edge: decrease speed by param RPM
    PIN_MODE_BTN_STOP     = 6,  // falling edge: stop motor
    PIN_MODE_BTN_START    = 7,  // falling edge: start at last_speed
    PIN_MODE_BTN_SET_SPEED = 8, // falling edge: set speed to param RPM
} pin_mode_t;

typedef struct {
    uint8_t    gpio;   // GPIO number
    pin_mode_t mode;
    float      param;  // meaning depends on mode (see pin_mode_t comments)
} pin_cfg_t;

#define NUM_INPUT_PINS 3
extern pin_cfg_t pin_cfg[NUM_INPUT_PINS];

typedef enum {
    SETTINGS_OK,
    SETTINGS_OK_REBOOT,
    SETTINGS_ERR_JSON,
    SETTINGS_ERR_NVS,
} settings_result_t;

void              settings_load_from_nvs(void);
bool              settings_save_to_nvs(void);
void              settings_save_wifi_to_nvs(void);
void              settings_save_last_state(float rpm, bool running);
settings_result_t settings_apply_json(const char *json_str, char *tx_out, size_t tx_out_len);
