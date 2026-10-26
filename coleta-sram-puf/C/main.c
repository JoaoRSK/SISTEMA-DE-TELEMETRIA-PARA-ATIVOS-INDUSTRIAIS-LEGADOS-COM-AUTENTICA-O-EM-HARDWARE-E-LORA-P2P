#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "esp_system.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "PUF_COLLECTOR";

#define RTC_SLOW_BASE 0x50000000UL
#define RTC_SLOW_SIZE 8192

#define W5500_MISO 12
#define W5500_MOSI 13
#define W5500_SCK  23
#define W5500_CS   15
#define W5500_INT  4
#define W5500_RST  2

#define ESP_IP   "172.16.0.10"
#define ESP_GW   "172.16.0.1"
#define ESP_MASK "255.255.255.0"
#define PC_IP    "172.16.0.1"
#define PC_PORT  5000

static uint8_t sram_snapshot[RTC_SLOW_SIZE] __attribute__((aligned(4)));

static void capture_rtc_slow(void) {
    volatile const uint32_t *src = (volatile const uint32_t *)RTC_SLOW_BASE;
    uint32_t *dst = (uint32_t *)sram_snapshot;
    for (size_t i = 0; i < RTC_SLOW_SIZE / 4; i++) dst[i] = src[i];
}

static void ethernet_init(void) {
    esp_err_t isr = gpio_install_isr_service(0);
    if (isr != ESP_OK && isr != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(isr);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&cfg);

    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(netif));
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = esp_ip4addr_aton(ESP_IP);
    ip.gw.addr = esp_ip4addr_aton(ESP_GW);
    ip.netmask.addr = esp_ip4addr_aton(ESP_MASK);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip));

    spi_bus_config_t bus = {
        .miso_io_num = W5500_MISO, .mosi_io_num = W5500_MOSI,
        .sclk_io_num = W5500_SCK, .quadwp_io_num = -1, .quadhd_io_num = -1
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .command_bits = 16, .address_bits = 8, .mode = 0,
        .clock_speed_hz = 10 * 1000 * 1000,
        .spics_io_num = W5500_CS, .queue_size = 20
    };

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = 1;
    phy_cfg.reset_gpio_num = W5500_RST;

    eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(SPI3_HOST, &dev);
    w5500.int_gpio_num = W5500_INT;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;

    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth));
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_eth_start(eth));
}

static bool send_all(int sock, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len) {
        int n = send(sock, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

static bool send_snapshot(void) {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) return false;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PC_PORT),
        .sin_addr.s_addr = inet_addr(PC_IP)
    };

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGW(TAG, "Falha TCP: errno=%d", errno);
        close(sock);
        return false;
    }

    uint8_t header[8] = {'P','U','F','1'};
    uint32_t size = htonl(RTC_SLOW_SIZE);
    memcpy(header + 4, &size, 4);

    bool ok = send_all(sock, header, sizeof(header)) &&
              send_all(sock, sram_snapshot, RTC_SLOW_SIZE);

    shutdown(sock, SHUT_RDWR);
    close(sock);
    return ok;
}

void app_main(void) {
    esp_reset_reason_t reason = esp_reset_reason();

    if (reason != ESP_RST_POWERON) {
        ESP_LOGW(TAG, "Reset nao foi power-on (%d). Amostra descartada.", reason);
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    capture_rtc_slow();
    ethernet_init();
    vTaskDelay(pdMS_TO_TICKS(4000));

    while (!send_snapshot()) {
        ESP_LOGW(TAG, "Nova tentativa em 2 s...");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    ESP_LOGI(TAG, "Amostra enviada. Desligue o ESP32.");
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}