#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "measure.h"

/* ---- Ajustes (revisalos antes de compilar) ---------------------------------- */
#define PIN_STIM  GPIO_NUM_25         /* estimulo: inicia cada medicion            */
#define PIN_VIS   GPIO_NUM_26         /* sensor visual                             */
#define PIN_AUD   GPIO_NUM_27         /* sensor auditivo                           */
#define EDGE      GPIO_INTR_NEGEDGE   /* flanco activo (pull-up + sensor que baja) */
#define PULLUP_INTERNAL 1             /* 0 si usas pull-ups externos solamente     */
#define DEAD_US    100000             /* zona muerta del pin de estimulo: 100 ms   */
#define TIMEOUT_US 2000000            /* si un sensor no responde en 2 s: "sin respuesta" */
#define HIST_N 20
/* ----------------------------------------------------------------------------- */

typedef struct { uint32_t n; int32_t vis_us, aud_us; } trial_t;   /* -1 = sin respuesta */

extern const char app_html_start[] asm("_binary_app_html_start");

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int64_t s_t_stim, s_t_vis, s_t_aud, s_last_stim;
static volatile bool s_armed;
static trial_t s_hist[HIST_N];
static uint32_t s_count;
static httpd_handle_t s_srv;

/* --------------------------------------------------------------- ISRs ------ */
/* Solo guardan el instante (esp_timer, 1 us); el WiFi no agrega jitter. */
static void IRAM_ATTR isr_stim(void *a)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_stim < DEAD_US) return;
    s_last_stim = now;
    s_t_vis = 0; s_t_aud = 0; s_t_stim = now;
    s_armed = true;
}
static void IRAM_ATTR isr_vis(void *a)
{
    int64_t now = esp_timer_get_time();
    if (s_armed && !s_t_vis) s_t_vis = now;     /* solo el primer flanco tras el estimulo */
}
static void IRAM_ATTR isr_aud(void *a)
{
    int64_t now = esp_timer_get_time();
    if (s_armed && !s_t_aud) s_t_aud = now;
}

/* ------------------------------------------------------------ resultados --- */
static int fmt_trial(char *b, size_t n, const trial_t *t)
{
    char v[16], a[16];
    if (t->vis_us >= 0) snprintf(v, sizeof(v), "%.1f", t->vis_us / 1000.0); else strcpy(v, "null");
    if (t->aud_us >= 0) snprintf(a, sizeof(a), "%.1f", t->aud_us / 1000.0); else strcpy(a, "null");
    return snprintf(b, n, "{\"n\":%u,\"vis\":%s,\"aud\":%s}", (unsigned)t->n, v, a);
}

static void broadcast(const char *txt)
{
    int cl[7]; size_t fds = 7;
    if (!s_srv || httpd_get_client_list(s_srv, &fds, cl) != ESP_OK) return;
    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)txt, .len = strlen(txt) };
    for (size_t i = 0; i < fds; i++)
        if (httpd_ws_get_fd_info(s_srv, cl[i]) == HTTPD_WS_CLIENT_WEBSOCKET)
            httpd_ws_send_frame_async(s_srv, cl[i], &f);
}

static void meas_task(void *arg)
{
    for (;;) {
        vTaskDelay(1);// (pdMS_TO_TICKS(5));
        if (!s_armed) continue;
        int64_t ts, tv, ta, now = esp_timer_get_time();
        portENTER_CRITICAL(&s_mux);
        ts = s_t_stim; tv = s_t_vis; ta = s_t_aud;
        portEXIT_CRITICAL(&s_mux);
        if (!(tv && ta) && now - ts < TIMEOUT_US) continue;

        bool fin = false;                       /* no cerrar si entro un estimulo nuevo */
        portENTER_CRITICAL(&s_mux);
        if (s_armed && s_t_stim == ts) { s_armed = false; fin = true; }
        portEXIT_CRITICAL(&s_mux);
        if (!fin) continue;

        trial_t t = { .n = s_count + 1,
                      .vis_us = tv ? (int32_t)(tv - ts) : -1,
                      .aud_us = ta ? (int32_t)(ta - ts) : -1 };
        s_hist[s_count % HIST_N] = t;
        s_count++;
        char buf[64];
        fmt_trial(buf, sizeof(buf), &t);
        broadcast(buf);
    }
}

/* ----------------------------------------------------------------- HTTP ---- */
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, app_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t history_handler(httpd_req_t *req)
{
    char *b = malloc(HIST_N * 56 + 4);
    if (!b) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    size_t o = 0;
    b[o++] = '[';
    uint32_t total = s_count, cnt = total < HIST_N ? total : HIST_N;
    for (uint32_t i = 0; i < cnt; i++) {
        if (i) b[o++] = ',';
        o += fmt_trial(b + o, 56, &s_hist[(total - cnt + i) % HIST_N]);
    }
    b[o++] = ']'; b[o] = 0;
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, b);
    free(b);
    return r;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) return ESP_OK;          /* handshake */
    httpd_ws_frame_t f = { 0 };
    return httpd_ws_recv_frame(req, &f, 0);              /* la pagina no envia nada */
}

void measure_register(httpd_handle_t srv)
{
    s_srv = srv;
    const httpd_uri_t u[] = {
        { .uri = "/",        .method = HTTP_GET, .handler = root_handler },
        { .uri = "/history", .method = HTTP_GET, .handler = history_handler },
        { .uri = "/ws",      .method = HTTP_GET, .handler = ws_handler, .is_websocket = true },
    };
    for (int i = 0; i < 3; i++) httpd_register_uri_handler(srv, &u[i]);
}

void measure_start(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_STIM) | (1ULL << PIN_VIS) | (1ULL << PIN_AUD),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = PULLUP_INTERNAL ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .intr_type = EDGE,
    };
    gpio_config(&io);
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(PIN_STIM, isr_stim, NULL);
    gpio_isr_handler_add(PIN_VIS, isr_vis, NULL);
    gpio_isr_handler_add(PIN_AUD, isr_aud, NULL);
    xTaskCreate(meas_task, "meas", 3072, NULL, 6, NULL);
}
