#pragma once
#include "driver/gpio.h"

#if CONFIG_IDF_TARGET_ESP32
#define PIN_MARCA GPIO_NUM_25
#define PIN_VIS   GPIO_NUM_26     /* estimulo visual   */
#define PIN_AUD   GPIO_NUM_27     /* estimulo auditivo */
#define PIN_LED   GPIO_NUM_2      /* LED de captura    */
#define PIN_STOP  GPIO_NUM_0      /* boton que termina la captura (BOOT en las placas de desarrollo) */
#else                             /* ESP32-C3 */
#define PIN_MARCA GPIO_NUM_4
#define PIN_VIS   GPIO_NUM_5
#define PIN_AUD   GPIO_NUM_6
#define PIN_LED   GPIO_NUM_7
#define PIN_STOP  GPIO_NUM_9
#endif
#define LED_ACTIVE_HIGH 1         /* 0 si el LED enciende con nivel bajo */
