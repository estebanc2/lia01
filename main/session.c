#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "soc/soc_caps.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "session.h"
#include "net.h"
#include "measure.h"
#include "pins.h"

static volatile session_state_t s_state = S_IDLE;
static bool s_net_up;
static const char *TAG = "sesion";

/* ------------------------------------------------------------------- LED --- */
#if LED_RGB
/* LED RGB direccionable (WS2812) manejado con el periferico RMT: sin componentes externos. */
static rmt_channel_handle_t s_led_ch;
static rmt_encoder_handle_t s_led_enc;
static uint8_t s_px[3];                       /* debe seguir valido mientras se transmite */

static void led_init(void)
{
    rmt_tx_channel_config_t c = {
        .gpio_num = PIN_LED, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10000000,
        .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL, .trans_queue_depth = 2,
    };
    rmt_bytes_encoder_config_t e = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },   /* 0,3 us alto + 0,9 us bajo */
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },   /* 0,9 us alto + 0,3 us bajo */
        .flags.msb_first = 1,
    };
    esp_err_t err = rmt_new_tx_channel(&c, &s_led_ch);
    if (err == ESP_OK) err = rmt_new_bytes_encoder(&e, &s_led_enc);
    if (err == ESP_OK) err = rmt_enable(s_led_ch);
    if (err != ESP_OK) { ESP_LOGE(TAG, "LED RGB no disponible: %s", esp_err_to_name(err)); s_led_ch = NULL; }
}

static void led_write(int r, int g, int b)
{
    if (!s_led_ch) return;
    s_px[0] = g; s_px[1] = r; s_px[2] = b;    /* el WS2812 espera el orden verde, rojo, azul */
    rmt_transmit_config_t t = { .loop_count = 0 };
    esp_err_t err = rmt_transmit(s_led_ch, s_led_enc, s_px, sizeof(s_px), &t);
    if (err != ESP_OK) ESP_LOGE(TAG, "LED RGB: %s", esp_err_to_name(err));
}
#else
static void led_init(void)
{
    gpio_config_t led = { .pin_bit_mask = 1ULL << PIN_LED, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&led);
}

static void led_write(int r, int g, int b)
{
    int on = (r | g | b) != 0;
    gpio_set_level(PIN_LED, LED_ACTIVE_HIGH ? on : !on);
}
#endif

static void led_set(int r, int g, int b)      /* solo escribe si el color cambia */
{
    static uint32_t last = 0xFFFFFFFF;
    uint32_t c = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    if (c == last) return;
    last = c;
    led_write(r, g, b);
}

/* --------------------------------------------------------------- sesion ---- */
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
    led_init();
    /* prueba al arrancar: rojo, verde y azul (confirma que el LED funciona) */
    led_set(64, 0, 0); vTaskDelay(pdMS_TO_TICKS(300));
    led_set(0, 64, 0); vTaskDelay(pdMS_TO_TICKS(300));
    led_set(0, 0, 64); vTaskDelay(pdMS_TO_TICKS(300));
    led_set(0, 0, 0);
    gpio_config_t btn = { .pin_bit_mask = 1ULL << PIN_STOP, .mode = GPIO_MODE_INPUT,
                          .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&btn);

    int quiet = 0, tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        tick++;

        /* LED: captura = destello verde de 100 ms cada 2 s; espera = ambar fijo; configuracion = azul tenue */
        if (s_state == S_CAPTURE)   { if (tick % 20 == 0) led_set(0, LED_BRIGHT, 0); else led_set(0, 0, 0); }
        else if (s_state == S_WAIT) led_set(LED_BRIGHT, LED_BRIGHT / 2, 0);
        else                        led_set(0, 0, LED_BRIGHT);

        /* boton: termina la captura (o cancela la espera) */
        if (gpio_get_level(PIN_STOP) == 0) {
            if (s_state != S_IDLE) session_stop();
            while (gpio_get_level(PIN_STOP) == 0) vTaskDelay(pdMS_TO_TICKS(50));
        }

        /* con "apagar WiFi": cuando no queda ningun dispositivo conectado por 2 s, se apaga el radio */
        if (s_state == S_WAIT) {
            int n = net_clients();
            quiet = (n == 0) ? quiet + 1 : 0;
            if (tick % 10 == 0) ESP_LOGI(TAG, "esperando la desconexion: %d dispositivo(s) conectado(s)", n);
            if (quiet >= 20) {
                ESP_LOGI(TAG, "WiFi apagado: comienza la captura");
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
