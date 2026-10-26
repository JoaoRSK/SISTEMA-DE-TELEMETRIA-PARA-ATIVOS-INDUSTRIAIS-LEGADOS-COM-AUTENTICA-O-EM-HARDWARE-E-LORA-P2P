#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "protocol.h"

#define LORA_MISO 19
#define LORA_MOSI 27
#define LORA_SCK 5
#define LORA_CS 18
#define LORA_RST 14
#define REG_FIFO 0x00
#define REG_OP_MODE 0x01
#define REG_FRF_MSB 0x06
#define REG_FIFO_ADDR_PTR 0x0D
#define REG_FIFO_RX_BASE_ADDR 0x0F
#define REG_FIFO_RX_CURRENT_ADDR 0x10
#define REG_IRQ_FLAGS 0x12
#define REG_RX_NB_BYTES 0x13
#define REG_PKT_SNR_VALUE 0x19
#define REG_PKT_RSSI_VALUE 0x1A
#define REG_MODEM_CONFIG_1 0x1D
#define REG_MODEM_CONFIG_2 0x1E
#define REG_PREAMBLE_MSB 0x20
#define REG_PREAMBLE_LSB 0x21
#define REG_MODEM_CONFIG_3 0x26
#define REG_SYNC_WORD 0x39
#define REG_VERSION 0x42
#define IRQ_RX_DONE 0x40
#define IRQ_CRC_ERROR 0x20
static const char *TAG = "LORA_TEST_RX";
static spi_device_handle_t radio;

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
    write_reg(REG_PREAMBLE_MSB, 0x00);
    write_reg(REG_PREAMBLE_LSB, 0x08);
    write_reg(REG_SYNC_WORD, 0x12);
    write_reg(REG_FIFO_RX_BASE_ADDR, 0x00);
    write_reg(REG_FIFO_ADDR_PTR, 0x00);
    write_reg(REG_IRQ_FLAGS, 0xFF);
    write_reg(REG_OP_MODE, 0x85);
    ESP_LOGI(TAG, "Escutando: 915 MHz | BW125 | SF7 | CR4/5 | CRC ON");
}
static void receive_once(void) {
    uint8_t flags = read_reg(REG_IRQ_FLAGS);
    if (!(flags & IRQ_RX_DONE)) return;
    write_reg(REG_IRQ_FLAGS, flags);
    if (flags & IRQ_CRC_ERROR) {
        printf("LRX,CRC\n");
        fflush(stdout);
        return;
    }
    uint8_t len = read_reg(REG_RX_NB_BYTES);
    uint8_t addr = read_reg(REG_FIFO_RX_CURRENT_ADDR);
    if (len != TEST_FRAME_SIZE) {
        printf("LRX,INVALID,LEN,%u\n", (unsigned)len);
        fflush(stdout);
        return;
    }
    uint8_t frame[TEST_FRAME_SIZE];
    write_reg(REG_FIFO_ADDR_PTR, addr);
    for (size_t i = 0; i < TEST_FRAME_SIZE; i++) frame[i] = read_reg(REG_FIFO);
    uint32_t run;
    uint16_t seq;
    if (!test_parse_frame(frame, len, &run, &seq)) {
        printf("LRX,INVALID,DATA,%u\n", (unsigned)len);
        fflush(stdout);
        return;
    }
    int8_t snr_raw = (int8_t)read_reg(REG_PKT_SNR_VALUE);
    float snr = snr_raw / 4.0f;
    int rssi = (int)read_reg(REG_PKT_RSSI_VALUE) - 157;
    printf("LRX,RX,%08lX,%u,%d,%.2f,%u\n", (unsigned long)run,
           (unsigned)seq, rssi, (double)snr, (unsigned)len);
    fflush(stdout);
}
void app_main(void) {
    radio_init();
    ESP_LOGI(TAG, "Modo medicao RF; aguarde o transmissor iniciar o ensaio");
    while (true) {
        receive_once();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
