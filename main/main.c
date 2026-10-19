#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "wifi_manager.h"
#include "measure.h"

static const char *TAG = "app";

/* wifi_manager la llama una vez: ya hay IP y el puerto 80 esta libre. */
static void on_wifi_ready(const char *ip)
{
    ESP_LOGI(TAG, "Conectado, IP %s", ip);

    mdns_init();
    mdns_hostname_set(MDNS_HOST);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    httpd_handle_t srv = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    if (httpd_start(&srv, &cfg) == ESP_OK) measure_register(srv);
}

void app_main(void)
{
    measure_start();                      /* la medicion no depende de la red */
    wifi_manager_start(on_wifi_ready);
}
