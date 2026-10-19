#include "led.h"

static void led_indicator_task(void *arg)
{
    static uint32_t short_blink_0 = 0x00000000;   // 0000 0000 0000 0000 0000 0000 0000 0000
    static uint32_t short_blink_1 = 0x80000000;   // 1000 0000 0000 0000 0000 0000 0000 0000
    static uint32_t short_blink_2 = 0x88000000;   // 1000 1000 0000 0000 0000 0000 0000 0000
    static uint32_t short_blink_3 = 0x88800000;   // 1000 1000 1000 0000 0000 0000 0000 0000
    //static uint32_t upgrading = 0xFFFFFFFF;
    static uint32_t *roll;
    static uint32_t trans;
    while (1)
    {
        if (0)
        {
            roll = &short_blink_3;
        }
        else if (1)
        {
            roll = &short_blink_1;
        }
        else if (0)
        {
            roll = &short_blink_2;
        }
        else
        {
            roll = &short_blink_0;
        }
        trans = *roll & 0x80000000;
        *roll <<= 1;
        if (trans != 0)
        {
            *roll |= 0x00000001;
            gpio_set_level(GPIO_LED, 1); // se prende
        }
        else
        {
            gpio_set_level(GPIO_LED, 0); // se apaga
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void led_init(){
    gpio_config_t io_conf = {
        .mode         = GPIO_MODE_OUTPUT,
        .intr_type    = GPIO_INTR_DISABLE,
        .pin_bit_mask = (1ULL << GPIO_LED),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(GPIO_LED, 0);
    xTaskCreate(led_indicator_task, "led_indicator_task", 2048, NULL, 4, NULL);
}