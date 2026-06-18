#include "leds.h"
#include "iot_configs.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const int s_pins[3] = { PIN_LED_RED, PIN_LED_GREEN, PIN_LED_BLUE };
static bool s_steady[3] = { false, false, false };

void leds_init(void)
{
    for (int i = 0; i < 3; i++) {
        gpio_reset_pin(s_pins[i]);
        gpio_set_direction(s_pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(s_pins[i], 0);
    }
}

void led_set(led_t led, bool on)
{
    s_steady[led] = on;
    gpio_set_level(s_pins[led], on ? 1 : 0);
}

void led_blink(led_t led, int duration_ms, int times)
{
    bool steady = s_steady[led];
    for (int i = 0; i < times; i++) {
        gpio_set_level(s_pins[led], steady ? 0 : 1);
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        gpio_set_level(s_pins[led], steady ? 1 : 0);
        if (i < times - 1) vTaskDelay(pdMS_TO_TICKS(duration_ms));
    }
    gpio_set_level(s_pins[led], steady ? 1 : 0);
}

void leds_state_ap_mode(void)
{
    led_set(LED_RED, true);
    led_set(LED_GREEN, false);
    led_set(LED_BLUE, false);
}

void leds_state_wifi_ok(void)
{
    led_set(LED_RED, false);
    led_set(LED_GREEN, true);
}

void leds_state_mqtt_ok(bool on)
{
    led_set(LED_BLUE, on);
}

void leds_state_error(void)
{
    led_blink(LED_RED, 200, 5);
}
