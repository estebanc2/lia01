idf_component_register(SRCS "led.c" "main.c" "wifi_manager.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES nvs_flash esp_wifi esp_event esp_netif
                        esp_http_server json esp_driver_gpio driver lwip
                    EMBED_TXTFILES "portal.html")
