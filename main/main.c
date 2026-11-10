#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ch9344.h"
#include "esp_system.h"
#include "esp_log.h"

#define TAG "main"
void usb_host_init(void);

void app_main(void)
{
    usb_host_init();
    ESP_LOGI(TAG, "waiting for ch9344 to be initialized...\n");

    do {
        vTaskDelay(300 / portTICK_PERIOD_MS);
    }while(!ch9344_is_initialized());
    ESP_LOGI(TAG, "ch9344 initialized done.\n");

    for(int i = 0; i < 8; i++) {
        ch9344_enable_uart(i);
    }
    //vTaskDelay(1000 / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "ch9344 port enable done.\n");
    char buf[256];
    while(1) {
        int i;
        for(i = 0; i < 8; i++) {
            int len = 0;
            len = ch9344_uart_recv(i, (char *)buf, sizeof(buf));
            if(len > 0) {
                //ESP_LOG_BUFFER_HEXDUMP(TAG, buf, len, ESP_LOG_INFO);
                ch9344_uart_send(i, buf, len);
            }
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}