#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "measure.h"

#define MARK_UART UART_NUM_0     /* puerto del conversor USB-serie de la placa (el mismo con que se carga el firmware) */
#define MARK_BAUD 115200

/* Cada llegada de bytes cuenta como un flanco de la marca. El instante es el de llegada del
 * primer byte completo (a 115200 baudios dura unos 87 us) mas la latencia de la tarea, un
 * desfase pequeno y casi constante frente al retardo del USB y del sistema operativo. Si la
 * PC envia varios bytes seguidos, solo el primero define la marca del experimento. */
static void serial_task(void *arg)
{
    uint8_t buf[64];
    for (;;) {
        int n = uart_read_bytes(MARK_UART, buf, sizeof(buf), portMAX_DELAY);
        int64_t t = esp_timer_get_time();
        if (n > 0) measure_mark_from_task(t);
    }
}

void serial_mark_start(void)
{
    uart_config_t cfg = {
        .baud_rate = MARK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(MARK_UART, 256, 0, 0, NULL, 0);
    uart_param_config(MARK_UART, &cfg);
    uart_set_rx_full_threshold(MARK_UART, 1);     /* interrumpir ya con el primer byte */
    uart_set_rx_timeout(MARK_UART, 1);
    xTaskCreate(serial_task, "mark", 3072, NULL, 20, NULL);
}
