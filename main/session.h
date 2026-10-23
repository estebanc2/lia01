#pragma once
#include <stdbool.h>

typedef enum { S_IDLE = 0, S_WAIT = 1, S_CAPTURE = 2 } session_state_t;

void session_init(void);              /* arranca el AP y la tarea de control (LED, boton, desconexion) */
session_state_t session_state(void);
bool session_start(void);             /* borra los datos y pasa a espera (WiFi off) o captura */
void session_stop(void);              /* termina la captura y vuelve a la configuracion */
