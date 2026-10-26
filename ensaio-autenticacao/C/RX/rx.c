#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/gcm.h"
#include "gateway_key.h"

static const char *TAG = "TCC_RECEPTOR_LORA";

// Somente em bancada: 1 = nao envia valores fixos de teste ao Zabbix.
#define TCC_ISOLAR_ZABBIX 1
#define PROTOCOL_VERSION  0x03
#define DEV_TYPE_PM5100   0x51
#define NODE_SRC_ID       0x01
#define NODE_DST_ID       0x02

#define AES_KEY_SIZE           16
#define HEADER_SIZE             8
#define GCM_NONCE_SIZE         12
#define PM5100_PAYLOAD_SIZE    25
#define GCM_TAG_SIZE           16
#define FRAME_SIZE (HEADER_SIZE + GCM_NONCE_SIZE + PM5100_PAYLOAD_SIZE + GCM_TAG_SIZE)
#define VAL_TENSAO   0x01u
#define VAL_CORRENTE 0x02u
#define VAL_FP       0x04u
#define VAL_POTENCIA 0x08u
#define VAL_ENERGIA  0x10u
#define VAL_TODOS    0x1Fu

_Static_assert(FRAME_SIZE == 61, "Frame LoRa deve possuir 61 bytes");

typedef struct {
    float tensao;
    float corrente;
    float fp;
    float potencia;
    uint64_t energia;
    uint8_t valid_mask;
} pm5100_data_t;

#define LORA_MISO  19
#define LORA_MOSI  27
#define LORA_SCK    5
#define LORA_CS    18
#define LORA_RST   14

#define REG_FIFO                 0x00
#define REG_OP_MODE              0x01
#define REG_FRF_MSB              0x06
#define REG_FRF_MID              0x07
#define REG_FRF_LSB              0x08
#define REG_PA_CONFIG            0x09
#define REG_FIFO_ADDR_PTR        0x0D
#define REG_FIFO_TX_BASE_ADDR    0x0E
#define REG_FIFO_RX_BASE_ADDR    0x0F
#define REG_FIFO_RX_CURRENT_ADDR 0x10
#define REG_IRQ_FLAGS            0x12
#define REG_RX_NB_BYTES          0x13
#define REG_PKT_SNR_VALUE        0x19
#define REG_PKT_RSSI_VALUE       0x1A
#define REG_MODEM_CONFIG_1       0x1D
#define REG_MODEM_CONFIG_2       0x1E
#define REG_PREAMBLE_MSB         0x20
#define REG_PREAMBLE_LSB         0x21
#define REG_PAYLOAD_LENGTH       0x22
#define REG_MODEM_CONFIG_3       0x26
#define REG_SYNC_WORD            0x39
#define REG_VERSION              0x42

#define IRQ_RX_DONE              0x40
#define IRQ_PAYLOAD_CRC_ERROR    0x20

static spi_device_handle_t spi;

#define NVS_NAMESPACE_RX       "rx_security"
#define NVS_KEY_STATE          "replay_state"
#define REPLAY_STATE_MAGIC     0x52504C59u
#define RX_SEQ_RESERVE         32u
#define MAX_VALID_SEQUENCE     (UINT32_MAX - 1u)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t session;
    uint32_t seq_limit;
    uint32_t reserved;
} replay_store_t;

typedef enum {
    REPLAY_OK,
    REPLAY_NOVA_SESSAO,
    REPLAY_SESSAO_ANTIGA,
    REPLAY_SEQ_ANTIGA,
    REPLAY_ESTADO_INVALIDO
} replay_result_t;

static nvs_handle_t replay_nvs;
static bool replay_inicializado = false;
static uint64_t ultima_sessao = 0;
static uint32_t ultima_seq = 0;
static uint32_t seq_limite_persistido = 0;

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) ESP_LOGE("SEC", "NVS indisponivel: %s", esp_err_to_name(err));
    ESP_ERROR_CHECK(err);
}

static uint32_t calcular_limite_seq(uint32_t seq)
{
    uint64_t limite = (uint64_t)seq + RX_SEQ_RESERVE - 1u;
    return limite > MAX_VALID_SEQUENCE ? MAX_VALID_SEQUENCE : (uint32_t)limite;
}

static bool persistir_estado_replay(uint64_t session, uint32_t seq_limit)
{
    replay_store_t estado = {
        .magic = REPLAY_STATE_MAGIC,
        .version = PROTOCOL_VERSION,
        .session = session,
        .seq_limit = seq_limit,
        .reserved = 0
    };
    esp_err_t err = nvs_set_blob(replay_nvs, NVS_KEY_STATE, &estado, sizeof(estado));
    if (err == ESP_OK) err = nvs_commit(replay_nvs);
    if (err != ESP_OK) {
        ESP_LOGE("SEC", "Falha ao persistir anti-replay: %s", esp_err_to_name(err));
        return false;
    }
    seq_limite_persistido = seq_limit;
    return true;
}

static void init_anti_replay(void)
{
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE_RX, NVS_READWRITE, &replay_nvs));
    replay_store_t estado = {0};
    size_t tamanho = sizeof(estado);
    esp_err_t err = nvs_get_blob(replay_nvs, NVS_KEY_STATE, &estado, &tamanho);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI("SEC", "Nenhum estado anti-replay persistido; aguardando primeiro frame valido");
        return;
    }
    ESP_ERROR_CHECK(err);
    if (tamanho != sizeof(estado) || estado.magic != REPLAY_STATE_MAGIC ||
        (estado.version != PROTOCOL_VERSION && estado.version != 0x02) || estado.session == 0 ||
        estado.seq_limit == 0 || estado.seq_limit > MAX_VALID_SEQUENCE) {
        ESP_LOGE("SEC", "Estado anti-replay persistido invalido");
        ESP_ERROR_CHECK(ESP_ERR_INVALID_STATE);
    }
    replay_inicializado = true;
    ultima_sessao = estado.session;
    seq_limite_persistido = estado.seq_limit;
    ultima_seq = estado.seq_limit;
    ESP_LOGI("SEC", "Estado restaurado | Session=%llu | Seq protegida ate=%lu",
             (unsigned long long)ultima_sessao, (unsigned long)ultima_seq);
    printf("TEST_RX,RESTORE,%llu,%lu\n", (unsigned long long)ultima_sessao,
           (unsigned long)ultima_seq);
    fflush(stdout);
}

static uint32_t read_u32_be(const uint8_t *src)
{
    return ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) |
           ((uint32_t)src[2] << 8) | (uint32_t)src[3];
}

static uint64_t read_u64_be(const uint8_t *src)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) value = (value << 8) | src[i];
    return value;
}

static float read_float_be(const uint8_t *src)
{
    uint32_t raw = read_u32_be(src);
    float value;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

static void desserializar_payload(const uint8_t payload[PM5100_PAYLOAD_SIZE], pm5100_data_t *dados)
{
    dados->tensao = read_float_be(&payload[0]);
    dados->corrente = read_float_be(&payload[4]);
    dados->fp = read_float_be(&payload[8]);
    dados->potencia = read_float_be(&payload[12]);
    dados->energia = read_u64_be(&payload[16]);
    dados->valid_mask = payload[24];
}

static void lora_write_reg(uint8_t reg, uint8_t val)
{
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA,
        .length = 16,
        .tx_data = {(uint8_t)(reg | 0x80), val}
    };
    ESP_ERROR_CHECK(spi_device_transmit(spi, &t));
}

static uint8_t lora_read_reg(uint8_t reg)
{
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .length = 16,
        .tx_data = {(uint8_t)(reg & 0x7F), 0x00}
    };
    ESP_ERROR_CHECK(spi_device_transmit(spi, &t));
    return t.rx_data[1];
}

static void lora_reset(void)
{
    ESP_ERROR_CHECK(gpio_set_direction(LORA_RST, GPIO_MODE_OUTPUT));
    gpio_set_level(LORA_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LORA_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static void lora_start_rx(void)
{
    lora_write_reg(REG_OP_MODE, 0x81);
    lora_write_reg(REG_FIFO_RX_BASE_ADDR, 0x00);
    lora_write_reg(REG_FIFO_ADDR_PTR, 0x00);
    lora_write_reg(REG_IRQ_FLAGS, 0xFF);
    lora_write_reg(REG_OP_MODE, 0x85);
}

static void lora_init(void)
{
    lora_reset();
    spi_bus_config_t buscfg = {
        .miso_io_num = LORA_MISO,
        .mosi_io_num = LORA_MOSI,
        .sclk_io_num = LORA_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_DISABLED));
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 8000000,
        .mode = 0,
        .spics_io_num = LORA_CS,
        .queue_size = 1
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &spi));
    uint8_t version = lora_read_reg(REG_VERSION);
    ESP_LOGI("LORA", "SX127x REG_VERSION = 0x%02X", version);
    if (version != 0x12) {
        ESP_LOGE("LORA", "SX127x nao detectado. Verifique SPI e pinagem.");
        ESP_ERROR_CHECK(ESP_FAIL);
    }
    lora_write_reg(REG_OP_MODE, 0x00);
    vTaskDelay(pdMS_TO_TICKS(10));
    lora_write_reg(REG_OP_MODE, 0x80);
    vTaskDelay(pdMS_TO_TICKS(10));
    lora_write_reg(REG_OP_MODE, 0x81);
    lora_write_reg(REG_FRF_MSB, 0xE4);
    lora_write_reg(REG_FRF_MID, 0xC0);
    lora_write_reg(REG_FRF_LSB, 0x00);
    lora_write_reg(REG_MODEM_CONFIG_1, 0x72);
    lora_write_reg(REG_MODEM_CONFIG_2, 0x74);
    lora_write_reg(REG_MODEM_CONFIG_3, 0x04);
    lora_write_reg(REG_PA_CONFIG, 0x8F);
    lora_write_reg(REG_PREAMBLE_MSB, 0x00);
    lora_write_reg(REG_PREAMBLE_LSB, 0x08);
    lora_write_reg(REG_SYNC_WORD, 0x12);
    lora_write_reg(REG_FIFO_TX_BASE_ADDR, 0x00);
    lora_write_reg(REG_FIFO_RX_BASE_ADDR, 0x00);
    lora_write_reg(REG_IRQ_FLAGS, 0xFF);
    lora_start_rx();
    ESP_LOGI("LORA", "Receptor ativo: 915 MHz | BW125 | SF7 | CR4/5 | CRC ON");
}

static int lora_receive_bytes(uint8_t *buffer, size_t capacidade, int *rssi, float *snr)
{
    uint8_t irq = lora_read_reg(REG_IRQ_FLAGS);
    if (!(irq & IRQ_RX_DONE)) return 0;
    lora_write_reg(REG_IRQ_FLAGS, irq);
    if (irq & IRQ_PAYLOAD_CRC_ERROR) {
        ESP_LOGW("LORA_RX", "Pacote descartado: CRC PHY invalido");
        return -1;
    }
    uint8_t len = lora_read_reg(REG_RX_NB_BYTES);
    uint8_t addr = lora_read_reg(REG_FIFO_RX_CURRENT_ADDR);
    if (len > capacidade) {
        ESP_LOGW("LORA_RX", "Pacote excede buffer: %u bytes", len);
        return -1;
    }
    lora_write_reg(REG_FIFO_ADDR_PTR, addr);
    for (uint8_t i = 0; i < len; i++) buffer[i] = lora_read_reg(REG_FIFO);
    int8_t snr_raw = (int8_t)lora_read_reg(REG_PKT_SNR_VALUE);
    *snr = snr_raw / 4.0f;
    *rssi = -157 + lora_read_reg(REG_PKT_RSSI_VALUE);
    return len;
}

static bool aes_gcm_decrypt(const uint8_t *ciphertext, size_t ciphertext_len,
                            const uint8_t *key, const uint8_t *nonce,
                            const uint8_t *aad, size_t aad_len,
                            const uint8_t tag[GCM_TAG_SIZE], uint8_t *plaintext)
{
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, AES_KEY_SIZE * 8);
    if (ret == 0) {
        ret = mbedtls_gcm_auth_decrypt(&gcm, ciphertext_len, nonce, GCM_NONCE_SIZE,
                                       aad, aad_len, tag, GCM_TAG_SIZE,
                                       ciphertext, plaintext);
    }
    mbedtls_gcm_free(&gcm);
    if (ret != 0) {
        ESP_LOGW("SEC", "Frame rejeitado: autenticacao AES-GCM falhou");
        return false;
    }
    return true;
}

static replay_result_t anti_replay_validar(uint64_t session, uint32_t seq)
{
    if (session == 0 || seq == 0 || seq > MAX_VALID_SEQUENCE)
        return REPLAY_ESTADO_INVALIDO;
    if (!replay_inicializado || session > ultima_sessao) {
        uint32_t limite = calcular_limite_seq(seq);
        if (!persistir_estado_replay(session, limite)) return REPLAY_ESTADO_INVALIDO;
        replay_inicializado = true;
        ultima_sessao = session;
        ultima_seq = seq;
        return REPLAY_NOVA_SESSAO;
    }
    if (session < ultima_sessao) return REPLAY_SESSAO_ANTIGA;
    if (seq <= ultima_seq) return REPLAY_SEQ_ANTIGA;
    if (seq > seq_limite_persistido) {
        uint32_t limite = calcular_limite_seq(seq);
        if (!persistir_estado_replay(session, limite)) return REPLAY_ESTADO_INVALIDO;
    }
    ultima_seq = seq;
    return REPLAY_OK;
}

static bool validar_header(const uint8_t header[HEADER_SIZE])
{
    if (header[0] != PROTOCOL_VERSION) {
        ESP_LOGW("PROTO", "Versao invalida: %u", header[0]);
        return false;
    }
    if (header[1] != DEV_TYPE_PM5100) {
        ESP_LOGW("PROTO", "Tipo de dispositivo invalido: 0x%02X", header[1]);
        return false;
    }
    if (header[2] != NODE_SRC_ID || header[3] != NODE_DST_ID) {
        ESP_LOGW("PROTO", "Endereco invalido: src=0x%02X dst=0x%02X", header[2], header[3]);
        return false;
    }
    return true;
}

static void imprimir_hex(const char *nome, const uint8_t *data, size_t len)
{
    printf("%s: ", nome);
    for (size_t i = 0; i < len; i++) printf("%02X", data[i]);
    printf("\n");
}

#define W5500_MISO 12
#define W5500_MOSI 13
#define W5500_SCK  23
#define W5500_CS   15
#define W5500_INT   4
#define W5500_RST   2
#define ESP_IP "172.16.0.10"
#define ESP_GW "172.16.0.1"
#define ESP_MASK "255.255.255.0"
#define ZBX_IP "172.16.0.1"
#define ZBX_PORT 10051
#define ZBX_HOST "Medidor_PM5100_1_1"

typedef struct {
    pm5100_data_t dados;
    uint64_t session;
    uint32_t seq;
    int rssi;
    float snr;
    uint8_t protocol;
    uint8_t src_id;
    uint8_t dst_id;
    uint8_t dev_type;
} zbx_msg_t;

static QueueHandle_t zbx_queue;
static volatile bool ethernet_pronta = false;

static void ethernet_evento(void *ctx, esp_event_base_t base, int32_t id, void *data)
{
    (void)ctx; (void)data;
    if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        ethernet_pronta = true;
        ESP_LOGI("ETH", "W5500 pronto: %s", ESP_IP);
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) {
        ethernet_pronta = false;
        ESP_LOGW("ETH", "Link Ethernet desconectado");
    }
}

static void ethernet_init(void)
{
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, ethernet_evento, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, ethernet_evento, NULL));
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&cfg);
    if (!netif) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
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
        .clock_speed_hz = 10000000, .spics_io_num = W5500_CS, .queue_size = 20
    };
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = 1;
    phy_cfg.reset_gpio_num = W5500_RST;
    eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(SPI3_HOST, &dev);
    w5500.int_gpio_num = W5500_INT;
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
    if (!mac || !phy) ESP_ERROR_CHECK(ESP_FAIL);
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth));
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_eth_start(eth));
}

static bool zbx_add(char *json, size_t cap, size_t *pos, bool *first,
                    const char *key, const char *value)
{
    if (*pos >= cap) return false;
    int n = snprintf(json + *pos, cap - *pos,
                     "%s{\"host\":\"%s\",\"key\":\"%s\",\"value\":\"%s\"}",
                     *first ? "" : ",", ZBX_HOST, key, value);
    if (n < 0 || (size_t)n >= cap - *pos) return false;
    *pos += (size_t)n;
    *first = false;
    return true;
}

static bool tcp_send_all(int sock, const uint8_t *data, size_t len)
{
    while (len) {
        int sent = send(sock, data, len, 0);
        if (sent <= 0) return false;
        data += sent;
        len -= sent;
    }
    return true;
}

static bool tcp_read_all(int sock, uint8_t *data, size_t len)
{
    while (len) {
        int n = recv(sock, data, len, 0);
        if (n <= 0) return false;
        data += n;
        len -= n;
    }
    return true;
}

static size_t montar_json_zabbix(const zbx_msg_t *msg, char *json, size_t cap)
{
    char value[48];
    size_t pos = 0;
    bool first = true;
    int n = snprintf(json, cap, "{\"request\":\"sender data\",\"data\":[");
    if (n < 0 || (size_t)n >= cap) return 0;
    pos = (size_t)n;
    const pm5100_data_t *d = &msg->dados;
    if (d->valid_mask & VAL_TENSAO) {
        snprintf(value, sizeof(value), "%.2f", d->tensao);
        if (!zbx_add(json, cap, &pos, &first, "pm5100.tensao", value)) return 0;
    }
    if (d->valid_mask & VAL_CORRENTE) {
        snprintf(value, sizeof(value), "%.2f", d->corrente);
        if (!zbx_add(json, cap, &pos, &first, "pm5100.corrente", value)) return 0;
    }
    if (d->valid_mask & VAL_POTENCIA) {
        snprintf(value, sizeof(value), "%.2f", d->potencia);
        if (!zbx_add(json, cap, &pos, &first, "pm5100.potencia", value)) return 0;
    }
    if (d->valid_mask & VAL_ENERGIA) {
        snprintf(value, sizeof(value), "%llu", (unsigned long long)d->energia);
        if (!zbx_add(json, cap, &pos, &first, "pm5100.energia", value)) return 0;
    }
    if (d->valid_mask & VAL_FP) {
        snprintf(value, sizeof(value), "%.3f", d->fp);
        if (!zbx_add(json, cap, &pos, &first, "pm5100.fp", value)) return 0;
    }
    snprintf(value, sizeof(value), "%d", msg->rssi);
    if (!zbx_add(json, cap, &pos, &first, "lora.rssi", value)) return 0;
    if (!zbx_add(json, cap, &pos, &first, "medidor.status", d->valid_mask ? "1" : "0")) return 0;
    snprintf(value, sizeof(value), "%.2f", msg->snr);
    if (!zbx_add(json, cap, &pos, &first, "lora.snr", value)) return 0;
    snprintf(value, sizeof(value), "%u", (unsigned)d->valid_mask);
    if (!zbx_add(json, cap, &pos, &first, "medidor.valid_mask", value)) return 0;
    snprintf(value, sizeof(value), "%lu", (unsigned long)msg->seq);
    if (!zbx_add(json, cap, &pos, &first, "lora.seq", value)) return 0;
    snprintf(value, sizeof(value), "%u", (unsigned)msg->protocol);
    if (!zbx_add(json, cap, &pos, &first, "lora.protocol", value)) return 0;
    snprintf(value, sizeof(value), "%llu", (unsigned long long)msg->session);
    if (!zbx_add(json, cap, &pos, &first, "lora.session", value)) return 0;
    snprintf(value, sizeof(value), "%u", (unsigned)msg->src_id);
    if (!zbx_add(json, cap, &pos, &first, "lora.src_id", value)) return 0;
    snprintf(value, sizeof(value), "%u", (unsigned)msg->dst_id);
    if (!zbx_add(json, cap, &pos, &first, "lora.dst_id", value)) return 0;
    snprintf(value, sizeof(value), "%u", (unsigned)msg->dev_type);
    if (!zbx_add(json, cap, &pos, &first, "lora.dev_type", value)) return 0;
    n = snprintf(json + pos, cap - pos, "]}");
    if (n < 0 || (size_t)n >= cap - pos) return 0;
    pos += (size_t)n;
    return pos;
}

static bool enviar_para_zabbix(const zbx_msg_t *msg)
{
    char json[2048];
    size_t pos = montar_json_zabbix(msg, json, sizeof(json));
    if (!pos) return false;
    if (!ethernet_pronta) {
        ESP_LOGW("ZABBIX", "Ethernet indisponivel; frame nao encaminhado");
        return false;
    }
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) return false;
    struct timeval timeout = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in dest = {
        .sin_family = AF_INET, .sin_port = htons(ZBX_PORT),
        .sin_addr.s_addr = inet_addr(ZBX_IP)
    };
    bool ok = false;
    do {
        if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) break;
        uint8_t hdr[13] = {'Z','B','X','D',0x01};
        uint64_t len = pos;
        for (int i = 0; i < 8; i++) hdr[5 + i] = (uint8_t)(len >> (8 * i));
        if (!tcp_send_all(sock, hdr, sizeof(hdr)) ||
            !tcp_send_all(sock, (const uint8_t *)json, pos)) break;
        uint8_t resp_hdr[13];
        if (!tcp_read_all(sock, resp_hdr, sizeof(resp_hdr)) ||
            memcmp(resp_hdr, hdr, 5) != 0) break;
        uint64_t resp_len = 0;
        for (int i = 0; i < 8; i++) resp_len |= (uint64_t)resp_hdr[5 + i] << (8 * i);
        if (resp_len == 0 || resp_len > 511) break;
        char response[512];
        if (!tcp_read_all(sock, (uint8_t *)response, (size_t)resp_len)) break;
        response[resp_len] = 0;
        ok = strstr(response, "\"response\":\"success\"") != NULL &&
             strstr(response, "failed: 0") != NULL;
        ESP_LOGI("ZABBIX", "Resposta: %s", response);
    } while (0);
    close(sock);
    if (!ok) ESP_LOGW("ZABBIX", "Falha ao enviar ou confirmar telemetria");
    return ok;
}

static void tarefa_zabbix(void *arg)
{
    zbx_msg_t msg;
    while (1) {
        if (xQueueReceive(zbx_queue, &msg, portMAX_DELAY) == pdTRUE)
            enviar_para_zabbix(&msg);
    }
}

// Log estruturado; valores de sessao/seq antes do AES sao NAO CONFIAVEIS.
static void log_teste_rx(const char *resultado, const uint8_t *nonce)
{
    uint64_t sessao = read_u64_be(nonce);
    uint32_t seq = read_u32_be(nonce + 8);
    printf("TEST_RX,%s,%llu,%lu\n", resultado,
           (unsigned long long)sessao, (unsigned long)seq);
    fflush(stdout);
}

static void processar_frame(const uint8_t frame[FRAME_SIZE], int rssi, float snr)
{
    const uint8_t *header = &frame[0];
    const uint8_t *nonce = &frame[HEADER_SIZE];
    const uint8_t *ciphertext = &frame[HEADER_SIZE + GCM_NONCE_SIZE];
    const uint8_t *tag = &frame[HEADER_SIZE + GCM_NONCE_SIZE + PM5100_PAYLOAD_SIZE];
    uint8_t plaintext[PM5100_PAYLOAD_SIZE];
    if (!validar_header(header)) { log_teste_rx("HEADER_FAIL", nonce); return; }
    if (!aes_gcm_decrypt(ciphertext, PM5100_PAYLOAD_SIZE, aes_key, nonce,
                         header, HEADER_SIZE, tag, plaintext)) {
        log_teste_rx("GCM_FAIL", nonce);
        return;
    }
    uint32_t header_seq = read_u32_be(&header[4]);
    uint64_t session = read_u64_be(&nonce[0]);
    uint32_t nonce_seq = read_u32_be(&nonce[8]);
    if (header_seq != nonce_seq) {
        ESP_LOGW("SEC", "Frame rejeitado: Sequence do header e nonce divergem");
        log_teste_rx("SEQ_MISMATCH", nonce);
        return;
    }
    if (plaintext[24] & (uint8_t)~VAL_TODOS) {
        ESP_LOGW("PROTO", "Mascara de validade invalida: 0x%02X", plaintext[24]);
        log_teste_rx("MASK_FAIL", nonce);
        return;
    }
    replay_result_t replay = anti_replay_validar(session, nonce_seq);
    if (replay == REPLAY_SESSAO_ANTIGA) {
        ESP_LOGW("SEC", "REPLAY bloqueado: Session=%llu < atual=%llu",
                 (unsigned long long)session, (unsigned long long)ultima_sessao);
        log_teste_rx("REPLAY_SESSION", nonce);
        return;
    }
    if (replay == REPLAY_SEQ_ANTIGA) {
        ESP_LOGW("SEC", "REPLAY bloqueado: Session=%llu | Seq=%lu <= ultima=%lu",
                 (unsigned long long)session, (unsigned long)nonce_seq,
                 (unsigned long)ultima_seq);
        log_teste_rx("REPLAY_SEQ", nonce);
        return;
    }
    if (replay == REPLAY_ESTADO_INVALIDO) {
        ESP_LOGE("SEC", "Frame rejeitado: estado anti-replay invalido ou NVS indisponivel");
        log_teste_rx("STATE_FAIL", nonce);
        return;
    }
    if (replay == REPLAY_NOVA_SESSAO) {
        ESP_LOGI("SEC", "Nova sessao autenticada: %llu | Seq inicial=%lu",
                 (unsigned long long)session, (unsigned long)nonce_seq);
    }
    log_teste_rx("ACCEPT", nonce);
    pm5100_data_t dados;
    desserializar_payload(plaintext, &dados);
    ESP_LOGI("RX", "Autenticado | Session=%llu | Seq=%lu | RSSI=%d dBm | SNR=%.2f dB | Validade=0x%02X",
             (unsigned long long)session, (unsigned long)nonce_seq, rssi, snr, dados.valid_mask);
    if (dados.valid_mask & VAL_TENSAO) ESP_LOGI("MEDIDOR", "Tensao: %.2f V", dados.tensao);
    else ESP_LOGW("MEDIDOR", "Tensao: indisponivel");
    if (dados.valid_mask & VAL_CORRENTE) ESP_LOGI("MEDIDOR", "Corrente: %.2f A", dados.corrente);
    else ESP_LOGW("MEDIDOR", "Corrente: indisponivel");
    if (dados.valid_mask & VAL_FP) ESP_LOGI("MEDIDOR", "FP: %.3f", dados.fp);
    else ESP_LOGW("MEDIDOR", "FP: indisponivel");
    if (dados.valid_mask & VAL_POTENCIA) ESP_LOGI("MEDIDOR", "Potencia: %.2f W", dados.potencia);
    else ESP_LOGW("MEDIDOR", "Potencia: indisponivel");
    if (dados.valid_mask & VAL_ENERGIA) ESP_LOGI("MEDIDOR", "Energia: %llu Wh", (unsigned long long)dados.energia);
    else ESP_LOGW("MEDIDOR", "Energia: indisponivel");
    imprimir_hex("Nonce", nonce, GCM_NONCE_SIZE);
#if !TCC_ISOLAR_ZABBIX
    zbx_msg_t msg = {
        .dados = dados, .session = session, .seq = nonce_seq, .rssi = rssi, .snr = snr,
        .protocol = header[0], .dev_type = header[1], .src_id = header[2], .dst_id = header[3]
    };
#endif
#if TCC_ISOLAR_ZABBIX
    ESP_LOGI("TESTE", "Frame autenticado em bancada: envio Zabbix suprimido");
#else
    if (xQueueSend(zbx_queue, &msg, 0) != pdTRUE)
        ESP_LOGW("ZABBIX", "Fila cheia; envio Zabbix desse frame perdido");
#endif
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "TCC - Receptor de Telemetria Industrial Segura | Heltec WiFi LoRa 32 V2");
    ESP_LOGI(TAG, "AES-128-GCM + LoRa P2P | Protocolo v%d | Frame=%d bytes",
             PROTOCOL_VERSION, FRAME_SIZE);
    ESP_LOGI(TAG, "Anti-replay persistente: Session64 + Sequence32 | reserva=%u", RX_SEQ_RESERVE);
    init_nvs();
    init_anti_replay();
    zbx_queue = xQueueCreate(16, sizeof(zbx_msg_t));
    if (!zbx_queue) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    ethernet_init();
    if (xTaskCreate(tarefa_zabbix, "zabbix", 6144, NULL, 5, NULL) != pdPASS)
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    lora_init();
    uint8_t frame[255];
    while (1) {
        int rssi = 0;
        float snr = 0.0f;
        int len = lora_receive_bytes(frame, sizeof(frame), &rssi, &snr);
        if (len == FRAME_SIZE) {
            ESP_LOGI("LORA_RX", "Pacote recebido: %d bytes", len);
            ESP_LOG_BUFFER_HEX_LEVEL("FRAME_RX", frame, len, ESP_LOG_INFO);
            processar_frame(frame, rssi, snr);
        } else if (len > 0) {
            ESP_LOGW("LORA_RX", "Tamanho inesperado: %d bytes; esperado=%d", len, FRAME_SIZE);
            ESP_LOG_BUFFER_HEX_LEVEL("LORA_RX", frame, len, ESP_LOG_INFO);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
