/** AquaGen Lite — status LED state machine (RED=AP/error, GREEN=WiFi, BLUE=MQTT). */
#ifndef LEDS_H
#define LEDS_H

#include <stdbool.h>

typedef enum {
    LED_RED,
    LED_GREEN,
    LED_BLUE,
} led_t;

void leds_init(void);
void led_set(led_t led, bool on);
void led_blink(led_t led, int duration_ms, int times);   // blinks then restores prior steady state

// Convenience high-level states matching the SOP LED table.
void leds_state_ap_mode(void);        // RED on, others off
void leds_state_wifi_ok(void);        // GREEN on, RED off
void leds_state_mqtt_ok(bool on);     // BLUE follows MQTT
void leds_state_error(void);          // RED blink

#endif // LEDS_H
