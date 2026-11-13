#include "board.h"

#include "driver/gpio.h"

void board_init(void)
{
    gpio_config_t pwr = {
        .pin_bit_mask = (1ULL << PIN_CAN_PWR) | (1ULL << PIN_CAN_RS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr);
    gpio_set_level(PIN_CAN_PWR, 1);
    gpio_set_level(PIN_CAN_RS, 0);

    gpio_config_t boot = {
        .pin_bit_mask = (1ULL << PIN_BOOT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot);
}
