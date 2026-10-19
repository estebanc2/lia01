#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "cJSON.h"
#include "wifi_manager.h"

#include "led.h"

static const char *TAG = "app";
static volatile uint32_t s_counter = 0;

/* Un entero que sube una vez por segundo. */
static void counter_task(void *arg)
{
    for (;;) {
        s_counter++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static esp_err_t data_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "contador", s_counter);
    char *txt = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, txt);
    free(txt);
    cJSON_Delete(root);
    return r;
}

/* wifi_manager la llama una vez, cuando ya hay IP y el puerto 80 esta libre. */
static void on_wifi_ready(const char *ip)
{
    ESP_LOGI(TAG, "Conectado, IP %s", ip);

    mdns_init();
    mdns_hostname_set(MDNS_HOST);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    httpd_handle_t srv = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&srv, &cfg) == ESP_OK) {
        httpd_uri_t data = { .uri = "/data", .method = HTTP_GET, .handler = data_get_handler };
        httpd_register_uri_handler(srv, &data);
    }
}

void app_main(void)
{
    //led_init();
    xTaskCreate(counter_task, "counter", 2048, NULL, 5, NULL);
    wifi_manager_start(on_wifi_ready);
}
