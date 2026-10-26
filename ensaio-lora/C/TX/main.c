#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "protocol.h"

#define LORA_MISO 19
#define LORA_MOSI 27
#define LORA_SCK 5
#define LORA_CS 18
#define LORA_RST 14
#define TEST_BUTTON GPIO_NUM_0
#define TEST_INTERVAL_MS 1000
#define REG_FIFO 0x00
#define REG_OP_MODE 0x01
#define REG_FRF_MSB 0x06
#define REG_PA_CONFIG 0x09
#define REG_FIFO_ADDR_PTR 0x0D
#define REG_FIFO_TX_BASE_ADDR 0x0E
#define REG_IRQ_FLAGS 0x12
#define REG_MODEM_CONFIG_1 0x1D
#define REG_MODEM_CONFIG_2 0x1E
#define REG_PREAMBLE_MSB 0x20
#define REG_PREAMBLE_LSB 0x21
#define REG_PAYLOAD_LENGTH 0x22
#define REG_MODEM_CONFIG_3 0x26
#define REG_SYNC_WORD 0x39
#define REG_VERSION 0x42
#define IRQ_TX_DONE 0x08
static const char *TAG = "LORA_TEST_TX";
static spi_device_handle_t radio;
static uint32_t runs = 0;

static void write_reg(uint8_t reg, uint8_t val) {
    spi_transaction_t t = {.flags = SPI_TRANS_USE_TXDATA, .length = 16,
                           .tx_data = {(uint8_t)(reg | 0x80), val}};
    ESP_ERROR_CHECK(spi_device_transmit(radio, &t));
}
static uint8_t read_reg(uint8_t reg) {
    spi_transaction_t t = {.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
                           .length = 16, .tx_data = {(uint8_t)(reg & 0x7F), 0}};
    ESP_ERROR_CHECK(spi_device_transmit(radio, &t));
    return t.rx_data[1];
}
static void radio_init(void) {
    ESP_ERROR_CHECK(gpio_set_direction(LORA_RST, GPIO_MODE_OUTPUT));
    gpio_set_level(LORA_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LORA_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    spi_bus_config_t bus = {.miso_io_num = LORA_MISO, .mosi_io_num = LORA_MOSI,
                            .sclk_io_num = LORA_SCK, .quadwp_io_num = -1,
                            .quadhd_io_num = -1, .max_transfer_sz = 64};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED));
    spi_device_interface_config_t cfg = {.clock_speed_hz = 8000000, .mode = 0,
                                         .spics_io_num = LORA_CS, .queue_size = 1};
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &cfg, &radio));
    if (read_reg(REG_VERSION) != 0x12) ESP_ERROR_CHECK(ESP_ERR_NOT_FOUND);
    write_reg(REG_OP_MODE, 0x00);
    vTaskDelay(pdMS_TO_TICKS(10));
    write_reg(REG_OP_MODE, 0x80);
    vTaskDelay(pdMS_TO_TICKS(10));
    write_reg(REG_OP_MODE, 0x81);
    write_reg(REG_FRF_MSB, 0xE4);
    write_reg(REG_FRF_MSB + 1, 0xC0);
    write_reg(REG_FRF_MSB + 2, 0x00);
    write_reg(REG_MODEM_CONFIG_1, 0x72);
    write_reg(REG_MODEM_CONFIG_2, 0x74);
    write_reg(REG_MODEM_CONFIG_3, 0x04);
    write_reg(REG_PA_CONFIG, 0x8F);
    write_reg(REG_PREAMBLE_MSB, 0x00);
    write_reg(REG_PREAMBLE_LSB, 0x08);
    write_reg(REG_SYNC_WORD, 0x12);
    write_reg(REG_FIFO_TX_BASE_ADDR, 0x00);
    write_reg(REG_IRQ_FLAGS, 0xFF);
    ESP_LOGI(TAG, "915 MHz | BW125 | SF7 | CR4/5 | CRC ON | 61 bytes");
}
static bool send_frame(const uint8_t frame[TEST_FRAME_SIZE]) {
    write_reg(REG_OP_MODE, 0x81);
    write_reg(REG_IRQ_FLAGS, 0xFF);
    write_reg(REG_FIFO_ADDR_PTR, 0x00);
    for (size_t i = 0; i < TEST_FRAME_SIZE; i++) write_reg(REG_FIFO, frame[i]);
    write_reg(REG_PAYLOAD_LENGTH, TEST_FRAME_SIZE);
    write_reg(REG_OP_MODE, 0x83);
    for (unsigned i = 0; i < 500; i++) {
        if (read_reg(REG_IRQ_FLAGS) & IRQ_TX_DONE) {
            write_reg(REG_IRQ_FLAGS, IRQ_TX_DONE);
            write_reg(REG_OP_MODE, 0x81);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    write_reg(REG_OP_MODE, 0x81);
    ESP_LOGE(TAG, "Timeout TX");
    return false;
}
static void run_test(void) {
    uint32_t run = esp_random() ^ (++runs * 0x9E3779B9u);
    unsigned ok = 0;
    uint8_t frame[TEST_FRAME_SIZE];
    ESP_LOGI(TAG, "INICIO | Run=%08lX | %u pacotes | intervalo=%d ms",
             (unsigned long)run, TEST_PACKETS, TEST_INTERVAL_MS);
    TickType_t next = xTaskGetTickCount();
    for (uint16_t seq = 1; seq <= TEST_PACKETS; seq++) {
        test_make_frame(frame, run, seq);
        bool enviado = send_frame(frame);
        if (enviado) ok++;
        printf("LTX,%08lX,%u,%u\n", (unsigned long)run, (unsigned)seq,
               enviado ? 1u : 0u);
        fflush(stdout);
        if (seq < TEST_PACKETS) vTaskDelayUntil(&next, pdMS_TO_TICKS(TEST_INTERVAL_MS));
    }
    ESP_LOGI(TAG, "FIM | Run=%08lX | TX Done=%u/%u",
             (unsigned long)run, ok, TEST_PACKETS);
    printf("LTX_END,%08lX,%u,%u\n", (unsigned long)run, ok, TEST_PACKETS);
    fflush(stdout);
}
void app_main(void) {
    gpio_config_t button = {.pin_bit_mask = 1ULL << TEST_BUTTON,
                            .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&button));
    radio_init();
    ESP_LOGI(TAG, "Pressione PRG/BOOT (GPIO0) uma vez para iniciar 100 envios");
    while (true) {
        if (gpio_get_level(TEST_BUTTON) == 0) {
            vTaskDelay(pdMS_TO_TICKS(35));
            if (gpio_get_level(TEST_BUTTON) == 0) {
                while (gpio_get_level(TEST_BUTTON) == 0) vTaskDelay(pdMS_TO_TICKS(20));
                run_test();
                ESP_LOGI(TAG, "Pronto para novo ponto; pressione PRG novamente");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
