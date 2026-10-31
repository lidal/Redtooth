#include "board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/rmt_tx.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Adafruit ESP32-C6 Feather
#define NEOPIXEL_PIN   9
#define I2C_POWER_PIN  20  // powers the NeoPixel and the I2C bus with the battery gauge
#define I2C_SDA_PIN    19
#define I2C_SCL_PIN    18

#define MAX17048_ADDR    0x36
#define MAX17048_REG_SOC 0x04  // state of charge, 1/256 % units

#define LED_BRIGHTNESS 80  // of 255

#define VB_BUTTON_MASK 0x3FFF  // bits 14 and 15 aren't buttons

static const char *TAG = "board";

static rmt_channel_handle_t s_led_channel;
static rmt_encoder_handle_t s_led_encoder;
static i2c_master_dev_handle_t s_gauge;

static void led_init(void)
{
    rmt_tx_channel_config_t channel_config = {
        .gpio_num = NEOPIXEL_PIN,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  // 0.1 us ticks
        .mem_block_symbols = 48,
        .trans_queue_depth = 2,
    };
    // WS2812 bits: 0 = 0.3 us high + 0.9 us low, 1 = 0.9 us high + 0.3 us low
    rmt_bytes_encoder_config_t encoder_config = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },
        .flags.msb_first = 1,
    };
    if (rmt_new_tx_channel(&channel_config, &s_led_channel) != ESP_OK ||
        rmt_new_bytes_encoder(&encoder_config, &s_led_encoder) != ESP_OK ||
        rmt_enable(s_led_channel) != ESP_OK) {
        ESP_LOGW(TAG, "NeoPixel setup failed");
        s_led_channel = NULL;
    }
}

static void gauge_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = -1,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_config, &bus) != ESP_OK ||
        i2c_master_probe(bus, MAX17048_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "MAX17048 battery gauge not found - battery level disabled.");
        return;
    }
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MAX17048_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &device_config, &s_gauge) != ESP_OK) s_gauge = NULL;
}

void board_init(void)
{
    // board_deep_sleep() left the power switch pin held and on the LP IO mux
    gpio_hold_dis(POWER_SWITCH_PIN);
    rtc_gpio_deinit(POWER_SWITCH_PIN);

    gpio_config_t inputs = {
        .pin_bit_mask = (1ULL << POWER_SWITCH_PIN) | (1ULL << VB_DATA_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&inputs);
    gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << VB_LATCH_PIN) | (1ULL << VB_CLOCK_PIN) | (1ULL << I2C_POWER_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&outputs);
    gpio_set_level(VB_LATCH_PIN, 0);
    gpio_set_level(VB_CLOCK_PIN, 1);

    // Deep sleep turned the NeoPixel/I2C power off
    gpio_set_level(I2C_POWER_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    led_init();
    gauge_init();
}

uint16_t vb_read_buttons(void)
{
    gpio_set_level(VB_LATCH_PIN, 1);
    esp_rom_delay_us(12);
    gpio_set_level(VB_LATCH_PIN, 0);
    esp_rom_delay_us(6);

    uint16_t pressed = 0;
    for (int i = 0; i < 16; i++) {
        gpio_set_level(VB_CLOCK_PIN, 0);
        esp_rom_delay_us(6);
        if (gpio_get_level(VB_DATA_PIN) == 0) pressed |= 1u << i;  // 0 = pressed
        gpio_set_level(VB_CLOCK_PIN, 1);
        esp_rom_delay_us(6);
    }
    return pressed & VB_BUTTON_MASK;
}

void led_set(uint32_t rgb)
{
    if (!s_led_channel) return;
    uint8_t grb[3] = {
        ((rgb >> 8) & 0xFF) * LED_BRIGHTNESS / 255,
        ((rgb >> 16) & 0xFF) * LED_BRIGHTNESS / 255,
        (rgb & 0xFF) * LED_BRIGHTNESS / 255,
    };
    rmt_transmit_config_t transmit_config = { 0 };
    rmt_transmit(s_led_channel, s_led_encoder, grb, sizeof(grb), &transmit_config);
    rmt_tx_wait_all_done(s_led_channel, 10);
}

bool battery_percent(float *percent)
{
    if (!s_gauge) return false;
    uint8_t reg = MAX17048_REG_SOC;
    uint8_t value[2];
    if (i2c_master_transmit_receive(s_gauge, &reg, 1, value, sizeof(value), 50) != ESP_OK) return false;
    *percent = value[0] + value[1] / 256.0f;
    return true;
}

bool power_switch_off(void)
{
    return gpio_get_level(POWER_SWITCH_PIN) == 0;
}

void board_deep_sleep(void)
{
    led_set(0);
    gpio_set_level(I2C_POWER_PIN, 0);

    rtc_gpio_init(POWER_SWITCH_PIN);
    rtc_gpio_set_direction(POWER_SWITCH_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(POWER_SWITCH_PIN);
    gpio_hold_en(POWER_SWITCH_PIN);

    esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(1ULL << POWER_SWITCH_PIN, ESP_GPIO_WAKEUP_GPIO_HIGH);
    esp_deep_sleep_start();
}
