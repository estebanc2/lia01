#pragma once
#include "driver/gpio.h"

#if CONFIG_IDF_TARGET_ESP32
#define PIN_MARCA GPIO_NUM_25
#define PIN_VIS   GPIO_NUM_26     /* estimulo visual             */
#define PIN_AUD_L GPIO_NUM_27     /* estimulo auditivo izquierdo */
#define PIN_AUD_R GPIO_NUM_32     /* estimulo auditivo derecho   */
#define PIN_LED   GPIO_NUM_2      /* LED de captura              */
#define LED_RGB   0               /* 0 = LED simple, 1 = LED RGB direccionable (WS2812) */
#define PIN_STOP  GPIO_NUM_0      /* boton que termina la captura (BOOT en las placas de desarrollo) */
#else                             /* ESP32-C3 */
#define PIN_MARCA GPIO_NUM_4
#define PIN_VIS   GPIO_NUM_5
#define PIN_AUD_L GPIO_NUM_6
#define PIN_AUD_R GPIO_NUM_7
#define PIN_LED   GPIO_NUM_8      /* ESP32-C3-DevKitC-02: LED RGB direccionable en el GPIO8 */
#define LED_RGB   1               /* en una placa propia con LED simple: 0 y el pin que uses */
#define PIN_STOP  GPIO_NUM_9
#endif
#define LED_ACTIVE_HIGH 1         /* LED simple: 0 si enciende con nivel bajo */
#define LED_BRIGHT 12             /* LED RGB: brillo, de 0 a 255 (bajo, para no molestar al sensor visual) */
