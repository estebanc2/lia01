#include <string.h>
#include <stdlib.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "wifi_manager.h"

#define RESET_GPIO GPIO_NUM_9      /* BOOT del C3: mantener 2 s en los primeros 10 s tras arrancar */
#define PORTAL_LINGER_MS 20000     /* el AP sigue 20 s tras conectar para que el telefono lea el resultado */

typedef enum { ST_IDLE, ST_CONNECTING, ST_OK, ST_FAIL } prov_state_t;

extern const char portal_html_start[] asm("_binary_portal_html_start");

static volatile prov_state_t s_state = ST_IDLE;
static char s_ip[16] = "";
static char s_err[48] = "";
static int s_retries = 0;
static bool s_portal = false, s_has_creds = false, s_cb_done = false;
static wifi_got_ip_cb_t s_cb;
static httpd_handle_t s_httpd;
static int s_dns_sock = -1;
static char *s_scan_json = NULL;

/* ------------------------------------------------------------------ DNS --- */
/* Responde a TODAS las consultas A con 192.168.4.1 para disparar el portal cautivo. */
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (sock < 0 || bind(sock, (struct sockaddr *)&a, sizeof(a)) < 0) { vTaskDelete(NULL); }
    s_dns_sock = sock;

    static const uint8_t ans[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1 };
    uint8_t buf[256];
    for (;;) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - sizeof(ans), 0, (struct sockaddr *)&from, &fl);
        if (n < 0) break;                       /* socket cerrado por stop_portal() */
        if (n < 13) continue;
        int p = 12;
        while (p < n && buf[p]) p += buf[p] + 1;
        p += 5;                                  /* byte 0 + QTYPE + QCLASS */
        if (p > n) continue;
        bool is_a = (buf[p - 4] == 0 && buf[p - 3] == 1);
        buf[2] = 0x81; buf[3] = 0x80;            /* respuesta estandar */
        buf[6] = 0; buf[7] = is_a ? 1 : 0;       /* ANCOUNT */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int len = p;
        if (is_a) { memcpy(buf + p, ans, sizeof(ans)); len += sizeof(ans); }
        sendto(sock, buf, len, 0, (struct sockaddr *)&from, fl);
    }
    vTaskDelete(NULL);
}

/* ----------------------------------------------------------------- HTTP --- */
static void do_scan(void)
{
    if (s_state == ST_CONNECTING) return;
    static wifi_ap_record_t recs[20];
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return;
    uint16_t n = 20;
    esp_wifi_scan_get_ap_records(&n, recs);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        if (!recs[i].ssid[0]) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (char *)recs[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
        cJSON_AddBoolToObject(o, "open", recs[i].authmode == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, o);
    }
    free(s_scan_json);
    s_scan_json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, portal_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    char q[16];
    bool rescan = httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK;
    if (rescan || !s_scan_json) do_scan();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, s_scan_json ? s_scan_json : "[]");
}

static esp_err_t connect_handler(httpd_req_t *req)
{
    char buf[200];
    int len = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (len <= 0) return ESP_FAIL;
    buf[len] = 0;
    cJSON *j = cJSON_Parse(buf);
    const cJSON *ssid = cJSON_GetObjectItem(j, "ssid");
    const cJSON *pass = cJSON_GetObjectItem(j, "pass");
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "falta ssid");
    }
    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, ssid->valuestring, sizeof(sta.sta.ssid));
    if (cJSON_IsString(pass)) strlcpy((char *)sta.sta.password, pass->valuestring, sizeof(sta.sta.password));
    sta.sta.pmf_cfg.capable = true;
    cJSON_Delete(j);

    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &sta);   /* queda guardado en NVS */
    s_has_creds = true; s_retries = 0; s_err[0] = 0; s_state = ST_CONNECTING;
    esp_wifi_connect();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t status_handler(httpd_req_t *req)
{
    static const char *names[] = { "idle", "connecting", "ok", "fail" };
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", names[s_state]);
    cJSON_AddStringToObject(o, "ip", s_ip);
    cJSON_AddStringToObject(o, "err", s_err);
    cJSON_AddStringToObject(o, "host", MDNS_HOST ".local");
    char *txt = cJSON_PrintUnformatted(o);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, txt);
    free(txt); cJSON_Delete(o);
    return r;
}

/* Cualquier otra URL (generate_204, hotspot-detect.html, ncsi.txt...) -> portal. */
static esp_err_t redirect_handler(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static void stop_portal(void)
{
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    if (s_dns_sock >= 0) { int s = s_dns_sock; s_dns_sock = -1; close(s); }
}

static void start_portal(void)
{
    s_portal = true; s_state = ST_IDLE;

    wifi_config_t ap = { .ap = { .channel = 1, .max_connection = 2, .authmode = WIFI_AUTH_OPEN } };
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "Medidor-%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();

    xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_open_sockets = 4;          /* el socket DNS tambien cuenta en lwip */
    hc.lru_purge_enable = true;
    hc.stack_size = 6144;
    if (httpd_start(&s_httpd, &hc) != ESP_OK) return;
    const httpd_uri_t uris[] = {
        { .uri = "/",        .method = HTTP_GET,  .handler = root_handler },
        { .uri = "/scan",    .method = HTTP_GET,  .handler = scan_handler },
        { .uri = "/status",  .method = HTTP_GET,  .handler = status_handler },
        { .uri = "/connect", .method = HTTP_POST, .handler = connect_handler },
    };
    for (int i = 0; i < 4; i++) httpd_register_uri_handler(s_httpd, &uris[i]);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect_handler);
}

/* --------------------------------------------------------------- eventos --- */
static const char *reason_text(int r)
{
    switch (r) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "Clave incorrecta";
    case WIFI_REASON_NO_AP_FOUND:       return "No se encontró la red (¿es de 5 GHz?)";
    default:                            return "No se pudo conectar";
    }
}

/* Corre en una task propia: el handler de eventos tiene poco stack. */
static void finish_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS((uint32_t)(uintptr_t)arg));
    if (s_portal) {
        stop_portal();
        esp_wifi_set_mode(WIFI_MODE_STA);
        s_portal = false;
    }
    if (!s_cb_done) { s_cb_done = true; if (s_cb) s_cb(s_ip); }
    vTaskDelete(NULL);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_has_creds) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        if (d->reason == WIFI_REASON_ASSOC_LEAVE || !s_has_creds) return;
        if (s_state == ST_OK) { esp_wifi_connect(); return; }      /* la red cayo: reintentar siempre */
        if (++s_retries < (s_portal ? 3 : 5)) { esp_wifi_connect(); return; }
        if (s_portal) {
            s_state = ST_FAIL;
            strlcpy(s_err, reason_text(d->reason), sizeof(s_err));
            s_has_creds = false;
        } else {
            s_has_creds = false;                                    /* credenciales viejas: abrir portal */
            start_portal();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_state = ST_OK; s_retries = 0;
        if (!s_cb_done)
            xTaskCreate(finish_task, "wifi_done", 4096,
                        (void *)(uintptr_t)(s_portal ? PORTAL_LINGER_MS : 0), 5, NULL);
    }
}

/* Mantener BOOT 2 s dentro de los 10 s posteriores al arranque borra la WiFi guardada. */
static void reset_watch_task(void *arg)
{
    gpio_set_direction(RESET_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(RESET_GPIO, GPIO_PULLUP_ONLY);
    int low = 0;
    for (int i = 0; i < 100; i++) {
        low = gpio_get_level(RESET_GPIO) ? 0 : low + 1;
        if (low >= 20) { esp_wifi_restore(); esp_restart(); }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}

void wifi_manager_start(wifi_got_ip_cb_t cb)
{
    s_cb = cb;
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&ic);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    xTaskCreate(reset_watch_task, "rst_watch", 2048, NULL, 3, NULL);

    wifi_config_t c;
    esp_wifi_get_config(WIFI_IF_STA, &c);
    s_has_creds = c.sta.ssid[0] != 0;
    if (s_has_creds) { esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_start(); }
    else start_portal();
}
