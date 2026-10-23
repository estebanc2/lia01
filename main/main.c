#include "measure.h"
#include "session.h"

void app_main(void)
{
    measure_start();        /* GPIO, interrupciones, almacenamiento */
    serial_mark_start();    /* marca por el puerto serie de la placa */
    session_init();         /* WiFi AP y control de la sesion */
}
