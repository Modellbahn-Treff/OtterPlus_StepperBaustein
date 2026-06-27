#include "stepper.h"
#include "settings.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "stepper";

// ---------------------------------------------------------------------------
// Hardware constants (from schematic)
// ---------------------------------------------------------------------------

#define STEPPER_UART      UART_NUM_2
#define PIN_TX            GPIO_NUM_17
#define PIN_RX            GPIO_NUM_16
#define PIN_EN            GPIO_NUM_19   // active LOW  (LOW = driver on)
#define UART_BAUD         115200
#define UART_BUF          256

// TMC2208 internal oscillator frequency
#define TMC_CLK_HZ        12000000UL

// Microstep resolution — must match MRES in CHOPCONF (0 = 256 µsteps/full step)
#define TMC_MICROSTEPS    256

// ---------------------------------------------------------------------------
// TMC2208 register addresses
// ---------------------------------------------------------------------------

#define REG_GCONF         0x00
#define REG_GSTAT         0x01
#define REG_IFCNT         0x02
#define REG_IHOLD_IRUN    0x10
#define REG_VACTUAL       0x22
#define REG_CHOPCONF      0x6C

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static float   s_rpm     = 0.0f;
static bool    s_enabled = false;

// ---------------------------------------------------------------------------
// CRC (polynomial 0x07, LSB first per TMC2208 datasheet §4.1.3)
// ---------------------------------------------------------------------------

static uint8_t calc_crc(const uint8_t *data, int len) {
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        uint8_t b = data[i];
        for (int j = 0; j < 8; j++) {
            if ((crc >> 7) ^ (b & 1)) crc = (uint8_t)((crc << 1) ^ 0x07);
            else                       crc = (uint8_t)(crc << 1);
            b >>= 1;
        }
    }
    return crc;
}

// ---------------------------------------------------------------------------
// UART helpers
// The TMC2208 shares a single wire (PDN_UART).  TX (GPIO17) connects via R1
// (1 kΩ) to the wire; RX (GPIO16) connects directly.  Because TX and RX are
// on the same physical net, every byte we transmit is immediately echoed back
// on RX — we must drain those echo bytes before reading real responses.
// ---------------------------------------------------------------------------

static void tmc_write_reg(uint8_t reg, uint32_t val) {
    uint8_t buf[8];
    buf[0] = 0x05;
    buf[1] = 0x00;               // slave address 0
    buf[2] = reg | 0x80;         // write bit
    buf[3] = (uint8_t)(val >> 24);
    buf[4] = (uint8_t)(val >> 16);
    buf[5] = (uint8_t)(val >>  8);
    buf[6] = (uint8_t)(val      );
    buf[7] = calc_crc(buf, 7);

    uart_write_bytes(STEPPER_UART, (const char *)buf, 8);
    // Drain the echoed bytes (same wire)
    uint8_t echo[8];
    uart_read_bytes(STEPPER_UART, echo, 8, pdMS_TO_TICKS(15));
}

static bool tmc_read_reg(uint8_t reg, uint32_t *val_out) {
    uint8_t req[4];
    req[0] = 0x05;
    req[1] = 0x00;
    req[2] = reg & 0x7F;         // no write bit
    req[3] = calc_crc(req, 3);

    uart_flush_input(STEPPER_UART);
    uart_write_bytes(STEPPER_UART, (const char *)req, 4);

    // Read: 4 echo bytes + 8 response bytes = 12
    uint8_t data[12];
    int n = uart_read_bytes(STEPPER_UART, data, 12, pdMS_TO_TICKS(50));
    if (n < 12) {
        ESP_LOGW(TAG, "read_reg 0x%02X: short read (%d)", reg, n);
        return false;
    }

    uint8_t *resp = data + 4;   // skip echoed request
    if (resp[0] != 0x05 || resp[1] != 0xFF || resp[2] != (reg & 0x7F)) {
        ESP_LOGW(TAG, "read_reg 0x%02X: bad header %02X %02X %02X",
                 reg, resp[0], resp[1], resp[2]);
        return false;
    }
    if (calc_crc(resp, 7) != resp[7]) {
        ESP_LOGW(TAG, "read_reg 0x%02X: CRC mismatch", reg);
        return false;
    }

    *val_out = ((uint32_t)resp[3] << 24) | ((uint32_t)resp[4] << 16)
             | ((uint32_t)resp[5] <<  8) |  (uint32_t)resp[6];
    return true;
}

// ---------------------------------------------------------------------------
// VACTUAL conversion
//
// TMC2208 velocity unit: f_CLK / 2^24  full steps / second
//   → 1 VACTUAL unit ≈ 0.7153 full-steps/s  (at 12 MHz)
//
// VACTUAL = RPM × steps_per_rev / 60  ×  2^24 / f_CLK
// ---------------------------------------------------------------------------

static int32_t rpm_to_vactual(float rpm) {
    float usteps_per_sec = rpm * (float)steps_per_rev * TMC_MICROSTEPS / 60.0f;
    float vactual_f      = usteps_per_sec * 16777216.0f / (float)TMC_CLK_HZ;
    return (int32_t)vactual_f;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void stepper_init(void) {
    // --- EN pin: start HIGH (driver disabled) ---
    gpio_set_direction(PIN_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_EN, 1);

    // --- UART for TMC2208 ---
    uart_config_t cfg = {};
    cfg.baud_rate  = UART_BAUD;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    uart_param_config(STEPPER_UART, &cfg);
    uart_set_pin(STEPPER_UART, PIN_TX, PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(STEPPER_UART, UART_BUF, 0, 0, NULL, 0);

    // Short settle time after UART init before sending datagrams
    vTaskDelay(pdMS_TO_TICKS(10));

    // --- Configure TMC2208 ---

    // GCONF: pdn_disable=1 (use UART, not PDN pin), mstep_reg_select=1 (MRES from UART)
    tmc_write_reg(REG_GCONF, (1u << 6) | (1u << 7));

    // Verify UART link by reading IFCNT (should increment after each successful write)
    uint32_t ifcnt = 0;
    if (tmc_read_reg(REG_IFCNT, &ifcnt)) {
        ESP_LOGI(TAG, "TMC2208 UART OK, IFCNT=%lu", (unsigned long)ifcnt);
    } else {
        ESP_LOGE(TAG, "TMC2208 UART link failed — check wiring");
    }

    // IHOLD_IRUN: IHOLDDELAY=6, IRUN, IHOLD from settings
    uint32_t ihold_irun = ((uint32_t)6 << 16) | ((uint32_t)irun << 8) | ihold;
    tmc_write_reg(REG_IHOLD_IRUN, ihold_irun);

    // CHOPCONF: intpol=1 (256-µstep interpolation), MRES=0 (256), TOFF=3, TBL=1
    // 0x10000053 is the datasheet reset default with intpol set
    tmc_write_reg(REG_CHOPCONF, 0x10000053UL);

    // VACTUAL = 0: motor idle
    tmc_write_reg(REG_VACTUAL, 0);

    s_rpm     = 0.0f;
    s_enabled = false;

    ESP_LOGI(TAG, "TMC2208 initialised (steps_per_rev=%u, IRUN=%u, IHOLD=%u)",
             steps_per_rev, irun, ihold);
}

void stepper_set_rpm(float rpm) {
    if (!s_enabled) {
        gpio_set_level(PIN_EN, 0);  // enable driver (active LOW)
        s_enabled = true;
    }

    int32_t vactual = rpm_to_vactual(rpm);
    // VACTUAL is a signed 24-bit value — mask to fit the register
    tmc_write_reg(REG_VACTUAL, (uint32_t)(vactual & 0x00FFFFFF));
    s_rpm = rpm;

    ESP_LOGI(TAG, "RPM=%.2f  VACTUAL=%ld", rpm, (long)vactual);
}

void stepper_stop(void) {
    tmc_write_reg(REG_VACTUAL, 0);
    vTaskDelay(pdMS_TO_TICKS(5));       // let the last step complete
    gpio_set_level(PIN_EN, 1);          // disable driver, release coils
    s_rpm     = 0.0f;
    s_enabled = false;
    ESP_LOGI(TAG, "Motor stopped, driver disabled");
}

float stepper_get_rpm(void) {
    return s_rpm;
}

bool stepper_is_running(void) {
    return s_enabled && (s_rpm != 0.0f);
}
