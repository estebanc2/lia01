#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "session.h"
#include "net.h"
#include "measure.h"
#include "pins.h"

static volatile session_state_t s_state = S_IDLE;
static bool s_net_up;

session_state_t session_state(void) { return s_state; }

bool session_start(void)
{
    if (s_state != S_IDLE) return false;
    if (!measure_capture_begin()) return false;      /* borra la flash: tarda unos segundos */
    s_state = measure_wifi_off() ? S_WAIT : S_CAPTURE;
    return true;
}

void session_stop(void)
{
    if (s_state == S_IDLE) return;
    s_state = S_IDLE;
    measure_capture_end();
    if (!s_net_up) { net_start(); s_net_up = true; }  /* vuelve el AP con los datos disponibles */
}

static void control_task(void *arg)
{
    gpio_config_t led = { .pin_bit_mask = 1ULL << PIN_LED, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&led);
    gpio_config_t btn = { .pin_bit_mask = 1ULL << PIN_STOP, .mode = GPIO_MODE_INPUT,
                          .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&btn);

    int quiet = 0, tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        tick++;

        /* LED: un destello de 100 ms cada 2 s mientras se captura */
        int on = (s_state == S_CAPTURE) && (tick % 20 == 0);
        gpio_set_level(PIN_LED, LED_ACTIVE_HIGH ? on : !on);

        /* boton: termina la captura (o cancela la espera) */
        if (gpio_get_level(PIN_STOP) == 0) {
            if (s_state != S_IDLE) session_stop();
            while (gpio_get_level(PIN_STOP) == 0) vTaskDelay(pdMS_TO_TICKS(50));
        }

        /* con "apagar WiFi": cuando no queda ningun dispositivo conectado por 2 s, se apaga el radio */
        if (s_state == S_WAIT) {
            quiet = (net_clients() == 0) ? quiet + 1 : 0;
            if (quiet >= 20) {
                net_stop();
                s_net_up = false;
                s_state = S_CAPTURE;
                quiet = 0;
            }
        } else {
            quiet = 0;
        }
    }
}

void session_init(void)
{
    net_start();
    s_net_up = true;
    xTaskCreate(control_task, "control", 3072, NULL, 4, NULL);
}
