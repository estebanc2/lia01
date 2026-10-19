#pragma once
#include "esp_http_server.h"

void measure_start(void);                    /* GPIO + interrupciones + task; no depende de la red */
void measure_register(httpd_handle_t srv);   /* registra "/", "/history" y "/ws" en el servidor */
