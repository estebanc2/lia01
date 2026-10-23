#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"

void measure_start(void);                    /* GPIO + interrupciones + task; no depende de la red */
void measure_register(httpd_handle_t srv);   /* registra "/", "/history" y "/ws" en el servidor */

bool measure_capture_begin(void);   /* borra la flash y empieza a guardar */
void measure_capture_end(void);     /* deja de guardar y recarga lo guardado */
bool measure_wifi_off(void);        /* configuracion: apagar el WiFi durante la captura */
void measure_detach(void);          /* el servidor se apaga: no enviar mas por WebSocket */

void measure_mark_from_task(int64_t t_us);   /* marca recibida por el puerto serie (instante en us) */
void serial_mark_start(void);                /* escucha el puerto serie de la placa */
