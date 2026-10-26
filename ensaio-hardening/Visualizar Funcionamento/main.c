#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_chip_info.h"
#include "esp_log.h"

static const char *TAG = "HARDENING";

void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "================================");
    ESP_LOGI(TAG, "TESTE DE HARDENING");
    ESP_LOGI(TAG, "Firmware executando normalmente");
    ESP_LOGI(TAG, "Revisao do ESP32: %d", chip_info.revision);
    ESP_LOGI(TAG, "================================");

    uint32_t contador = 0;

    while (1)
    {
        ESP_LOGI(TAG, "Firmware ativo - contador: %lu",
                 (unsigned long)contador++);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}