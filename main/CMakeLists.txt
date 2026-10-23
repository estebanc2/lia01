  idf_component_register(SRCS "serial_mark.c" "main.c" "net.c" "session.c" "measure.c"
                         INCLUDE_DIRS "."
                         PRIV_REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_http_server
                                       json driver lwip esp_timer esp_partition
                         EMBED_TXTFILES "app.html")