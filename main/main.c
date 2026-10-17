#include <string.h>
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "cJSON.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"

static httpd_handle_t server = NULL;
static volatile uint32_t s_counter = 0;

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
    //cJSON_AddNumberToObject(root, "temperatura", 23.5);   // tu dato real
    cJSON_AddNumberToObject(root, "contador", s_counter);
    char *txt = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, txt);
    free(txt);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char buf[256];
    int len = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (len <= 0) return ESP_FAIL;
    buf[len] = 0;
    cJSON *j = cJSON_Parse(buf);
    // ... aplicar y guardar en NVS ...
    cJSON_Delete(j);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static void start_services(void)
{
    mdns_init();
    mdns_hostname_set("miesp");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    httpd_start(&server, &cfg);
    httpd_uri_t data = { .uri = "/data",   .method = HTTP_GET,  .handler = data_get_handler };
    httpd_uri_t conf = { .uri = "/config", .method = HTTP_POST, .handler = config_post_handler };
    httpd_register_uri_handler(server, &data);
    httpd_register_uri_handler(server, &conf);
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_PROV_EVENT && id == WIFI_PROV_END) {
        wifi_prov_mgr_deinit();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (!server) start_services();
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL);
    esp_event_handler_register(WIFI_EVENT,      ESP_EVENT_ANY_ID, event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,     event_handler, NULL);

    xTaskCreate(counter_task, "counter", 2048, NULL, 5, NULL);

    wifi_prov_mgr_config_t pcfg = {
        .scheme = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };
    ESP_ERROR_CHECK(wifi_prov_mgr_init(pcfg));

    bool provisioned = false;
    wifi_prov_mgr_is_provisioned(&provisioned);

    if (!provisioned) {
        wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, "abcd1234" /* PoP */,
                                         "PROV_MIESP", NULL);
    } else {
        wifi_prov_mgr_deinit();
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
    }
}