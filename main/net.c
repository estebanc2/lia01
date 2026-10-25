#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "net.h"
#include "measure.h"

#define CAPTIVE_PORTAL 0   /* 0 = responder las comprobaciones de internet como si lo hubiera (no se abre la ventanita
                              de inicio de sesion; se entra a http://192.168.4.1); 1 = abrirla y redirigir a la pagina */
#define AP_PASS ""        /* vacio = red abierta; con 8 o mas caracteres, WPA2 */

static httpd_handle_t s_httpd;
static int s_dns_sock = -1;
static bool s_init;

/* DNS: responde 192.168.4.1 a todo, para que el telefono abra la pagina solo (portal cautivo). */
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (sock < 0 || bind(sock, (struct sockaddr *)&a, sizeof(a)) < 0) vTaskDelete(NULL);
    s_dns_sock = sock;

    static const uint8_t ans[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1 };
    uint8_t buf[256];
    for (;;) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - sizeof(ans), 0, (struct sockaddr *)&from, &fl);
        if (n < 0) break;                        /* socket cerrado por net_stop() */
        if (n < 13) continue;
        int p = 12;
        while (p < n && buf[p]) p += buf[p] + 1;
        p += 5;
        if (p > n) continue;
        bool is_a = (buf[p - 4] == 0 && buf[p - 3] == 1);
        buf[2] = 0x81; buf[3] = 0x80;
        buf[6] = 0; buf[7] = is_a ? 1 : 0;
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int len = p;
        if (is_a) { memcpy(buf + p, ans, sizeof(ans)); len += sizeof(ans); }
        sendto(sock, buf, len, 0, (struct sockaddr *)&from, fl);
    }
    vTaskDelete(NULL);
}

/* Comprobaciones de "hay internet" de cada sistema: macOS/iOS (hotspot-detect), Android (generate_204),
 * Windows (connecttest, ncsi) y Firefox (success.txt). Cualquier otra URL desconocida lleva a la pagina. */
static esp_err_t redirect_handler(httpd_req_t *req, httpd_err_code_t err)
{
#if !CAPTIVE_PORTAL
    const char *u = req->uri;
    if (strstr(u, "hotspot-detect") || strstr(u, "library/test/success")) {
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_sendstr(req, "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    }
    if (strstr(u, "generate_204") || strstr(u, "gen_204")) {
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }
    if (strstr(u, "connecttest.txt")) return httpd_resp_sendstr(req, "Microsoft Connect Test");
    if (strstr(u, "ncsi.txt"))        return httpd_resp_sendstr(req, "Microsoft NCSI");
    if (strstr(u, "success.txt"))     return httpd_resp_sendstr(req, "success\n");
#endif
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

void net_start(void)
{
    if (!s_init) {
        esp_netif_init();
        esp_event_loop_create_default();
        esp_netif_create_default_wifi_ap();
        wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&ic);
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        s_init = true;
    }
    wifi_config_t ap = { .ap = { .channel = 1, .max_connection = 2, .authmode = WIFI_AUTH_OPEN } };
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "Medidor-%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    if (strlen(AP_PASS) >= 8) {
        strlcpy((char *)ap.ap.password, AP_PASS, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();
    esp_wifi_set_inactive_time(WIFI_IF_AP, 20);   /* un dispositivo que se fue sin avisar deja de contar a los 20 s (por defecto, 300 s) */

    xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_open_sockets = 4;          /* el socket DNS tambien cuenta en lwip */
    hc.lru_purge_enable = true;
    hc.max_uri_handlers = 12;
    hc.stack_size = 6144;
    if (httpd_start(&s_httpd, &hc) == ESP_OK) {
        measure_register(s_httpd);
        httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect_handler);
    }
}

void net_stop(void)
{
    measure_detach();
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    if (s_dns_sock >= 0) { int s = s_dns_sock; s_dns_sock = -1; close(s); }
    esp_wifi_stop();                  /* radio apagado */
}

int net_clients(void)
{
    wifi_sta_list_t l;
    return esp_wifi_ap_get_sta_list(&l) == ESP_OK ? l.num : 0;
}
