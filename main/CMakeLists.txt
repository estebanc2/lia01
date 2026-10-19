idf_component_register(SRCS "main.c" "wifi_manager.c" "measure.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES nvs_flash esp_wifi esp_event esp_netif
                        esp_http_server json esp_driver_gpio driver lwip esp_timer
                    EMBED_TXTFILES "portal.html" "app.html")
