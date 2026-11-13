#ifndef BOARD_H
#define BOARD_H

#include "driver/gpio.h"

/* LilyGO T-CAN485. Official CAN example: GPIO16 HIGH powers the transceiver,
 * GPIO23 LOW is high-speed on the SN65HVD231. RS485 enable pins disagree
 * between the README and the schematic, and v1 does not use that port. */

#define PIN_CAN_TX GPIO_NUM_27
#define PIN_CAN_RX GPIO_NUM_26
#define PIN_CAN_RS GPIO_NUM_23
#define PIN_CAN_PWR GPIO_NUM_16

#define PIN_SD_MISO GPIO_NUM_2
#define PIN_SD_MOSI GPIO_NUM_15
#define PIN_SD_SCLK GPIO_NUM_14
#define PIN_SD_CS GPIO_NUM_13

#define PIN_LED GPIO_NUM_4
#define PIN_BOOT GPIO_NUM_0

#define PIN_RS485_TX GPIO_NUM_22
#define PIN_RS485_RX GPIO_NUM_21

#define CANGO_SD_HZ 20000000
#define CANGO_VERSION "0.1.0"
#define CANGO_ARM_US 60000000u

void board_init(void);

#endif
