#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include <time.h>
#include "esp_partition.h"
#include "pins.h"
#include "measure.h"
#include "session.h"

/* ---- Pines (revisalos antes de compilar) ------------------------------------ */
#define PULLUP_INTERNAL 1             /* 0 si usas pull-ups externos solamente */
#define DEF_GAP_MS 100                /* valores por defecto; se cambian desde la pagina */
#define DEF_MAX_MS 2000
#define HIST_N 20
/* ----------------------------------------------------------------------------- */

#define NONE INT32_MIN                /* "sin dato" */
static const gpio_num_t PINS[3] = { PIN_MARCA, PIN_VIS, PIN_AUD };

typedef struct { uint32_t gap_ms, max_ms; uint8_t rise[3], wifi_off, src; } cfg_t;     /* rise: 0 = bajada, 1 = subida */
typedef struct { uint32_t n; double mean, m2; int32_t min, max; } stat_t;
typedef struct { uint32_t n; int m; int32_t v, a; int64_t ts_ms; } trial_t;            /* m: llego la marca; v, a en us */

extern const char app_html_start[] asm("_binary_app_html_start");

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;   /* protege lo que tocan las ISR */
static SemaphoreHandle_t s_dmx;                              /* protege historial y estadisticas */
static cfg_t s_cfg = { DEF_GAP_MS, DEF_MAX_MS, { 0, 0, 0 }, 1, 1 };
static volatile int64_t s_gap_us = DEF_GAP_MS * 1000LL, s_max_us = DEF_MAX_MS * 1000LL;
static volatile int64_t s_t0, s_t[3], s_last[3];
static volatile bool s_armed;
static volatile int64_t s_epoch_off_us;     /* reloj real - esp_timer, en us; 0 = sin sincronizar */
static trial_t s_hist[HIST_N];
static stat_t s_st[2];                                       /* [0] visual, [1] auditivo */
static uint32_t s_count;
static httpd_handle_t s_srv;

/* --------------------------------------------------------------- ISRs ------ */
/* El primer flanco de cualquiera de las tres senales abre el experimento (t0).
 * Despues cada senal guarda su primer flanco. Para abrir uno nuevo, la senal debe
 * llevar al menos "gap" sin flancos (cubre rebotes y tonos que oscilan). */
static void IRAM_ATTR edge_at(int ch, int64_t now)
{
    portENTER_CRITICAL_SAFE(&s_mux);
    if (s_armed) {
        if (!s_t[ch] && now - s_t0 <= s_max_us) s_t[ch] = now;
    } else if (now - s_last[ch] >= s_gap_us) {
        s_armed = true;
        s_t0 = now;
        s_t[0] = s_t[1] = s_t[2] = 0;
        s_t[ch] = now;
    }
    s_last[ch] = now;
    portEXIT_CRITICAL_SAFE(&s_mux);
}
static void IRAM_ATTR on_edge(int ch) { edge_at(ch, esp_timer_get_time()); }
static void IRAM_ATTR isr_marca(void *a) { on_edge(0); }
static void IRAM_ATTR isr_vis(void *a)   { on_edge(1); }
static void IRAM_ATTR isr_aud(void *a)   { on_edge(2); }

/* ------------------------------------------------------------ resultados --- */
static void stat_add(stat_t *s, int32_t x)    /* media y desviacion en una pasada (Welford) */
{
    s->n++;
    double d = x - s->mean;
    s->mean += d / s->n;
    s->m2 += d * (x - s->mean);
    if (s->n == 1 || x < s->min) s->min = x;
    if (s->n == 1 || x > s->max) s->max = x;
}

static int fmt_val(char *b, int32_t v) { return v == NONE ? sprintf(b, "null") : sprintf(b, "%d", (int)v); }

static int fmt_trial(char *b, size_t n, const trial_t *t)
{
    char v[16], a[16], ts[24] = "null";
    fmt_val(v, t->v); fmt_val(a, t->a);
    if (t->ts_ms) snprintf(ts, sizeof(ts), "%lld", (long long)t->ts_ms);
    return snprintf(b, n, "{\"n\":%u,\"m\":%d,\"v\":%s,\"a\":%s,\"ts\":%s}", (unsigned)t->n, t->m, v, a, ts);
}

static int fmt_stat(char *b, size_t n, const stat_t *s)
{
    if (!s->n) return snprintf(b, n, "{\"n\":0}");
    char sd[24] = "null";
    if (s->n > 1) snprintf(sd, sizeof(sd), "%.1f", sqrt(s->m2 / (s->n - 1)));   /* desviacion muestral */
    return snprintf(b, n, "{\"n\":%u,\"mean\":%.1f,\"std\":%s,\"min\":%d,\"max\":%d}",
                    (unsigned)s->n, s->mean, sd, (int)s->min, (int)s->max);
}

static int fmt_stats(char *b, size_t n, const stat_t *s)
{
    int o = snprintf(b, n, "\"sv\":");
    o += fmt_stat(b + o, n - o, &s[0]);
    o += snprintf(b + o, n - o, ",\"sa\":");
    o += fmt_stat(b + o, n - o, &s[1]);
    return o;
}

/* ---------------------------------------------------------- almacenamiento --- */
/* Los experimentos de una captura se guardan en la particion "datos" (registros de 24 bytes). */
typedef struct { uint32_t n; int32_t v, a; uint32_t m; int64_t ts; } rec_t;
static const esp_partition_t *s_part;
static uint32_t s_stored;
static volatile bool s_capturing;

static void storage_append(const trial_t *t)
{
    if (!s_part || (s_stored + 1) * sizeof(rec_t) > s_part->size) return;      /* llena */
    rec_t r = { .n = t->n, .v = t->v, .a = t->a, .m = t->m, .ts = t->ts_ms };
    if (esp_partition_write(s_part, s_stored * sizeof(rec_t), &r, sizeof(r)) == ESP_OK) s_stored++;
}

static esp_err_t storage_erase(void)
{
    if (!s_part) return ESP_FAIL;
    s_stored = 0;
    return esp_partition_erase_range(s_part, 0, s_part->size);
}

/* Reconstruye contador, historial y estadisticas desde la flash (al arrancar y al terminar). */
static void storage_load(void)
{
    xSemaphoreTake(s_dmx, portMAX_DELAY);
    s_count = s_stored = 0;
    memset(s_st, 0, sizeof(s_st));
    rec_t r;
    while (s_part && (s_stored + 1) * sizeof(r) <= s_part->size &&
           esp_partition_read(s_part, s_stored * sizeof(r), &r, sizeof(r)) == ESP_OK &&
           r.n == s_stored + 1) {                       /* numeracion consecutiva: descarta datos viejos o basura */
        trial_t t = { .n = r.n, .m = r.m, .v = r.v, .a = r.a, .ts_ms = r.ts };
        s_hist[(t.n - 1) % HIST_N] = t;
        if (t.v != NONE) stat_add(&s_st[0], t.v);
        if (t.a != NONE) stat_add(&s_st[1], t.a);
        s_count = t.n;
        s_stored++;
    }
    xSemaphoreGive(s_dmx);
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
        vTaskDelay(1);
        int64_t now = esp_timer_get_time(), t[3], t0 = 0;
        bool fin = false;
        portENTER_CRITICAL(&s_mux);
        if (s_armed && ((s_t[0] && s_t[1] && s_t[2]) || now - s_t0 >= s_max_us)) {
            fin = true;
            s_armed = false;
            memcpy(t, (const void *)s_t, sizeof(t));
            t0 = s_t0;
        }
        portEXIT_CRITICAL(&s_mux);
        if (!fin) continue;

        /* delta = instante del estimulo - instante de la marca (negativo si la marca llega despues) */
        trial_t tr = { .m = t[0] != 0, .ts_ms = s_epoch_off_us ? (s_epoch_off_us + t0) / 1000 : 0,
                       .v = (t[0] && t[1]) ? (int32_t)(t[1] - t[0]) : NONE,
                       .a = (t[0] && t[2]) ? (int32_t)(t[2] - t[0]) : NONE };
        stat_t snap[2];
        xSemaphoreTake(s_dmx, portMAX_DELAY);
        tr.n = ++s_count;
        s_hist[(tr.n - 1) % HIST_N] = tr;
        if (s_capturing) storage_append(&tr);
        if (tr.v != NONE) stat_add(&s_st[0], tr.v);
        if (tr.a != NONE) stat_add(&s_st[1], tr.a);
        memcpy(snap, s_st, sizeof(snap));
        xSemaphoreGive(s_dmx);

        char tj[96], sj[256], msg[400];
        fmt_trial(tj, sizeof(tj), &tr);
        fmt_stats(sj, sizeof(sj), snap);
        snprintf(msg, sizeof(msg), "{\"t\":%s,%s}", tj, sj);
        broadcast(msg);
    }
}

/* -------------------------------------------------------- configuracion ---- */
static void apply_cfg(const cfg_t *c)
{
    portENTER_CRITICAL(&s_mux);
    s_cfg = *c;
    s_gap_us = c->gap_ms * 1000LL;
    s_max_us = c->max_ms * 1000LL;
    s_armed = false;
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < 3; i++) {
        gpio_set_intr_type(PINS[i], c->rise[i] ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE);
        if (i == 0 && c->src) gpio_intr_disable(PINS[i]); else gpio_intr_enable(PINS[i]);
    }
}

static void cfg_load(cfg_t *c)
{
    nvs_handle_t h; cfg_t t; size_t n = sizeof(t);
    if (nvs_open("medidor", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "cfg", &t, &n) == ESP_OK && n == sizeof(t) &&
        t.gap_ms >= 1 && t.gap_ms <= 10000 && t.max_ms >= 10 && t.max_ms <= 60000) *c = t;
    nvs_close(h);
}

static void cfg_save(const cfg_t *c)
{
    nvs_handle_t h;
    if (nvs_open("medidor", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "cfg", c, sizeof(*c));
    nvs_commit(h);
    nvs_close(h);
}

/* ----------------------------------------------------------------- HTTP ---- */
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, app_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t state_handler(httpd_req_t *req)
{
    const size_t SZ = HIST_N * 80 + 512;
    char *b = malloc(SZ);
    if (!b) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    trial_t h[HIST_N]; stat_t snap[2]; uint32_t total;
    xSemaphoreTake(s_dmx, portMAX_DELAY);
    total = s_count;
    memcpy(h, s_hist, sizeof(h));
    memcpy(snap, s_st, sizeof(snap));
    xSemaphoreGive(s_dmx);

    size_t o = snprintf(b, SZ, "{\"mode\":%d,\"stored\":%u,\"cfg\":{\"gap\":%u,\"max\":%u,\"r\":[%d,%d,%d],\"wo\":%d,\"src\":%d},\"h\":[",
                        (int)session_state(), (unsigned)s_stored, (unsigned)s_cfg.gap_ms, (unsigned)s_cfg.max_ms,
                        s_cfg.rise[0], s_cfg.rise[1], s_cfg.rise[2], s_cfg.wifi_off, s_cfg.src);
    uint32_t cnt = total < HIST_N ? total : HIST_N;
    for (uint32_t i = 0; i < cnt; i++) {
        if (i) b[o++] = ',';
        o += fmt_trial(b + o, 80, &h[(total - cnt + i) % HIST_N]);
    }
    o += snprintf(b + o, SZ - o, "],");
    o += fmt_stats(b + o, SZ - o, snap);
    b[o++] = '}'; b[o] = 0;
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, b);
    free(b);
    return r;
}

static esp_err_t config_handler(httpd_req_t *req)
{
    char buf[160];
    int len = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (len <= 0) return ESP_FAIL;
    buf[len] = 0;
    cJSON *j = cJSON_Parse(buf);
    cfg_t c = s_cfg;
    const cJSON *g = cJSON_GetObjectItem(j, "gap"), *m = cJSON_GetObjectItem(j, "max"),
                *r = cJSON_GetObjectItem(j, "r"), *w = cJSON_GetObjectItem(j, "wifi_off"), *sr = cJSON_GetObjectItem(j, "src");
    if (cJSON_IsNumber(g)) c.gap_ms = g->valueint;
    if (cJSON_IsNumber(m)) c.max_ms = m->valueint;
    if (cJSON_IsNumber(w)) c.wifi_off = w->valueint != 0;
    if (cJSON_IsNumber(sr)) c.src = sr->valueint != 0;
    for (int i = 0; i < 3 && cJSON_IsArray(r); i++) {
        const cJSON *e = cJSON_GetArrayItem(r, i);
        if (cJSON_IsNumber(e)) c.rise[i] = e->valueint != 0;
    }
    cJSON_Delete(j);
    if (c.gap_ms < 1 || c.gap_ms > 10000 || c.max_ms < 10 || c.max_ms > 60000)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "valores fuera de rango");
    apply_cfg(&c);
    cfg_save(&c);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t clear_handler(httpd_req_t *req)
{
    portENTER_CRITICAL(&s_mux);
    s_armed = false;
    portEXIT_CRITICAL(&s_mux);
    if (session_state() == S_IDLE && s_stored > 0) storage_erase();   /* borra tambien lo guardado */
    xSemaphoreTake(s_dmx, portMAX_DELAY);
    s_count = 0;
    memset(s_st, 0, sizeof(s_st));
    xSemaphoreGive(s_dmx);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* La pagina envia la hora del navegador al cargar y al conectar; el equipo la usa para fechar
 * los experimentos. Los tiempos medidos (delta) no dependen de este reloj. */
static esp_err_t time_handler(httpd_req_t *req)
{
    char buf[64];
    int len = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (len <= 0) return ESP_FAIL;
    buf[len] = 0;
    cJSON *j = cJSON_Parse(buf);
    const cJSON *ms = cJSON_GetObjectItem(j, "ms");
    if (cJSON_IsNumber(ms) && ms->valuedouble > 1e12)
        s_epoch_off_us = (int64_t)(ms->valuedouble * 1000.0) - esp_timer_get_time();
    cJSON_Delete(j);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t start_handler(httpd_req_t *req)
{
    bool ok = session_start();                 /* borra los datos anteriores (unos segundos) */
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static esp_err_t stop_handler(httpd_req_t *req)
{
    session_stop();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t csv_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"experimentos.csv\"");
    httpd_resp_sendstr_chunk(req, "n,epoch_ms,hora_utc,marca,visual_us,auditivo_us\r\n");
    rec_t r;
    char line[128];
    for (uint32_t i = 0; i < s_stored; i++) {
        if (esp_partition_read(s_part, i * sizeof(r), &r, sizeof(r)) != ESP_OK) break;
        char ep[24] = "", hr[32] = "", v[16] = "", a[16] = "";
        if (r.ts) {
            time_t sec = r.ts / 1000;
            struct tm tm;
            gmtime_r(&sec, &tm);
            size_t k = strftime(hr, sizeof(hr), "%Y-%m-%dT%H:%M:%S", &tm);
            snprintf(hr + k, sizeof(hr) - k, ".%03dZ", (int)(r.ts % 1000));
            snprintf(ep, sizeof(ep), "%lld", (long long)r.ts);
        }
        if (r.v != NONE) snprintf(v, sizeof(v), "%d", (int)r.v);
        if (r.a != NONE) snprintf(a, sizeof(a), "%d", (int)r.a);
        snprintf(line, sizeof(line), "%u,%s,%s,%u,%s,%s\r\n", (unsigned)r.n, ep, hr, (unsigned)r.m, v, a);
        if (httpd_resp_sendstr_chunk(req, line) != ESP_OK) return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
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
        { .uri = "/",       .method = HTTP_GET,  .handler = root_handler },
        { .uri = "/state",  .method = HTTP_GET,  .handler = state_handler },
        { .uri = "/config", .method = HTTP_POST, .handler = config_handler },
        { .uri = "/clear",  .method = HTTP_POST, .handler = clear_handler },
        { .uri = "/time",   .method = HTTP_POST, .handler = time_handler },
        { .uri = "/start",  .method = HTTP_POST, .handler = start_handler },
        { .uri = "/stop",   .method = HTTP_POST, .handler = stop_handler },
        { .uri = "/csv",    .method = HTTP_GET,  .handler = csv_handler },
        { .uri = "/ws",     .method = HTTP_GET,  .handler = ws_handler, .is_websocket = true },
    };
    for (int i = 0; i < 9; i++) httpd_register_uri_handler(srv, &u[i]);
}

void measure_start(void)
{
    esp_err_t e = nvs_flash_init();       /* wifi_manager tambien lo inicializa; repetirlo es inocuo */
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }
    s_dmx = xSemaphoreCreateMutex();
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "datos");
    storage_load();
    cfg_t c = s_cfg;
    cfg_load(&c);
    for (int i = 0; i < 3; i++) s_last[i] = -(1LL << 40);

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_MARCA) | (1ULL << PIN_VIS) | (1ULL << PIN_AUD),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = PULLUP_INTERNAL ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(PIN_MARCA, isr_marca, NULL);
    gpio_isr_handler_add(PIN_VIS, isr_vis, NULL);
    gpio_isr_handler_add(PIN_AUD, isr_aud, NULL);
    apply_cfg(&c);
    xTaskCreate(meas_task, "meas", 4096, NULL, 6, NULL);
}

/* ---- interfaz con session.c y net.c ---- */
bool measure_capture_begin(void)
{
    if (storage_erase() != ESP_OK) return false;
    xSemaphoreTake(s_dmx, portMAX_DELAY);
    s_count = 0;
    memset(s_st, 0, sizeof(s_st));
    xSemaphoreGive(s_dmx);
    portENTER_CRITICAL(&s_mux);
    s_armed = false;
    portEXIT_CRITICAL(&s_mux);
    s_capturing = true;
    return true;
}

void measure_capture_end(void) { s_capturing = false; storage_load(); }
bool measure_wifi_off(void)    { return s_cfg.wifi_off; }
void measure_detach(void)      { s_srv = NULL; }

/* La marca llega por el puerto serie (serial_mark.c): mismo tratamiento que un flanco del pin. */
void measure_mark_from_task(int64_t t_us) { if (s_cfg.src) edge_at(0, t_us); }
