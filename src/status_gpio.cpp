#include "port/port.h"
#include "status_gpio.h"

#include "config.h"

#define BUTTON_PRESS_MS 200

static volatile port::TimerId button_alarm_id = port::kNoTimer;

bool status_gpio_pin_valid(const uint8_t pin) {
    if (pin == STATUS_GPIO_DISABLED) return true;
    if (pin >= port::kNumGpioPins) return false;

#ifdef PICO_DEFAULT_UART_TX_PIN
    if (pin == PICO_DEFAULT_UART_TX_PIN) return false;
#endif
#ifdef PICO_DEFAULT_UART_RX_PIN
    if (pin == PICO_DEFAULT_UART_RX_PIN) return false;
#endif
#ifdef PICO_VSYS_PIN
    if (pin == PICO_VSYS_PIN) return false;
#endif
#ifdef PICO_VBUS_PIN
    if (pin == PICO_VBUS_PIN) return false;
#endif
#ifdef PICO_SMPS_MODE_PIN
    if (pin == PICO_SMPS_MODE_PIN) return false;
#endif
#ifdef CYW43_DEFAULT_PIN_WL_REG_ON
    if (pin == CYW43_DEFAULT_PIN_WL_REG_ON) return false;
#endif
#ifdef CYW43_DEFAULT_PIN_WL_DATA_OUT
    if (pin == CYW43_DEFAULT_PIN_WL_DATA_OUT) return false;
#endif
#ifdef CYW43_DEFAULT_PIN_WL_DATA_IN
    if (pin == CYW43_DEFAULT_PIN_WL_DATA_IN) return false;
#endif
#ifdef CYW43_DEFAULT_PIN_WL_HOST_WAKE
    if (pin == CYW43_DEFAULT_PIN_WL_HOST_WAKE) return false;
#endif
#ifdef CYW43_DEFAULT_PIN_WL_CLOCK
    if (pin == CYW43_DEFAULT_PIN_WL_CLOCK) return false;
#endif
#ifdef CYW43_DEFAULT_PIN_WL_CS
    if (pin == CYW43_DEFAULT_PIN_WL_CS) return false;
#endif

    return true;
}

void gpio_on_connect() {
    gpio_on_disconnect();

    const auto &config = get_config();
    if (config.status_gpio_pin == STATUS_GPIO_DISABLED) return;

    port::gpio_write(config.status_gpio_pin, true);

    if (config.status_gpio_mode == STATUS_GPIO_MODE_BUTTON) {
        button_alarm_id = port::timer_once_ms(BUTTON_PRESS_MS, [](void *user_data) {
            port::gpio_write((uint32_t) (uintptr_t) user_data, false);
            button_alarm_id = port::kNoTimer;
        }, (void *) (uintptr_t) config.status_gpio_pin);

        if (button_alarm_id == port::kNoTimer) {
            port::gpio_write(config.status_gpio_pin, false);
        }
    }
}

void gpio_on_disconnect() {
    if (button_alarm_id != port::kNoTimer) {
        port::timer_cancel(button_alarm_id);
        button_alarm_id = port::kNoTimer;
    }

    const auto &config = get_config();
    if (config.status_gpio_pin == STATUS_GPIO_DISABLED) return;

    port::gpio_init_output(config.status_gpio_pin);
    port::gpio_write(config.status_gpio_pin, false);
}
