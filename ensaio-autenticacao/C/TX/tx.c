#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "puf_indices.h"

static const char *TAG = "TCC_TRANSMISSOR_LORA";

#if PUF_NUM_BITS != 128
#error "puf_indices.h deve conter 128 posicoes da RTC SLOW"
#endif

// 1 = bateria de ensaios com valores fixos; 0 = telemetria Modbus normal.
#define TCC_MODO_ENSAIO 1
#define TEMPO_ENTRE_TESTES_MS 2200

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

#define RTC_SLOW_BASE 0x50000000UL
#define RTC_SLOW_BITS 65536U
#define HKDF_SALT "TCC-PUF-LoRa-v1"
#define HKDF_INFO "AES-128-GCM-TX01-RX02"

static uint8_t aes_key[AES_KEY_SIZE];

static void limpar_segredo(void *buffer, size_t tamanho)
{
    volatile uint8_t *p = buffer;
    while (tamanho--) *p++ = 0;
}

static bool inicializar_chave_puf(void)
{
    volatile const uint32_t *rtc = (volatile const uint32_t *)RTC_SLOW_BASE;
    uint8_t resposta[16] = {0}, prk[32] = {0}, t1[32] = {0};
    static const uint8_t info[] = HKDF_INFO;
    uint8_t info_bloco[sizeof(info)];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    bool ok = false;
    if (!md) goto fim;
    for (unsigned i = 0; i < PUF_NUM_BITS; i++) {
        uint16_t pos = puf_bit_index[i];
        uint8_t bit = (uint8_t)((rtc[pos / 32U] >> (pos % 32U)) & 1U);
        resposta[i / 8U] |= (uint8_t)(bit << (i % 8U));
    }
    if (mbedtls_md_hmac(md, (const uint8_t *)HKDF_SALT, sizeof(HKDF_SALT) - 1,
                        resposta, sizeof(resposta), prk) != 0) goto fim;
    memcpy(info_bloco, info, sizeof(info) - 1);
    info_bloco[sizeof(info_bloco) - 1] = 0x01;
    if (mbedtls_md_hmac(md, prk, sizeof(prk), info_bloco,
                        sizeof(info_bloco), t1) != 0) goto fim;
    memcpy(aes_key, t1, sizeof(aes_key));
    ok = true;
fim:
    limpar_segredo(resposta, sizeof(resposta));
    limpar_segredo(prk, sizeof(prk));
    limpar_segredo(t1, sizeof(t1));
    limpar_segredo(info_bloco, sizeof(info_bloco));
    if (!ok) limpar_segredo(aes_key, sizeof(aes_key));
    return ok;
}

#define NVS_NAMESPACE_TX  "tx_security"
#define NVS_KEY_SESSION   "session"

static uint64_t g_session_counter = 0;
static uint32_t g_seq_counter = 1;

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

#define REG_FIFO               0x00
#define REG_OP_MODE            0x01
#define REG_FRF_MSB            0x06
#define REG_FRF_MID            0x07
#define REG_FRF_LSB            0x08
#define REG_PA_CONFIG          0x09
#define REG_FIFO_ADDR_PTR      0x0D
#define REG_FIFO_TX_BASE_ADDR  0x0E
#define REG_IRQ_FLAGS          0x12
#define REG_MODEM_CONFIG_1     0x1D
#define REG_MODEM_CONFIG_2     0x1E
#define REG_PREAMBLE_MSB       0x20
#define REG_PREAMBLE_LSB       0x21
#define REG_PAYLOAD_LENGTH     0x22
#define REG_MODEM_CONFIG_3     0x26
#define REG_SYNC_WORD          0x39
#define REG_VERSION            0x42
#define IRQ_TX_DONE            0x08

static spi_device_handle_t spi;

#define TXD_PIN 23
#define RXD_PIN 22
#define UART_PORT UART_NUM_2
#define MODBUS_SLAVE_ID 0x01
#define MODBUS_FUNCTION 0x03

static void write_u32_be(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

static void write_u64_be(uint8_t *dst, uint64_t value)
{
    dst[0] = (uint8_t)(value >> 56);
    dst[1] = (uint8_t)(value >> 48);
    dst[2] = (uint8_t)(value >> 40);
    dst[3] = (uint8_t)(value >> 32);
    dst[4] = (uint8_t)(value >> 24);
    dst[5] = (uint8_t)(value >> 16);
    dst[6] = (uint8_t)(value >> 8);
    dst[7] = (uint8_t)value;
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) ESP_LOGE("SEC", "NVS indisponivel: %s", esp_err_to_name(err));
    ESP_ERROR_CHECK(err);
}

static bool iniciar_nova_sessao(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE_TX, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE("SEC", "Falha ao abrir NVS: %s", esp_err_to_name(err));
        return false;
    }
    uint64_t anterior = 0;
    err = nvs_get_u64(nvs, NVS_KEY_SESSION, &anterior);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE("SEC", "Falha ao ler sessao: %s", esp_err_to_name(err));
        nvs_close(nvs);
        return false;
    }
    if (anterior == UINT64_MAX) {
        ESP_LOGE("SEC", "Contador de sessao esgotado");
        nvs_close(nvs);
        return false;
    }
    uint64_t nova = anterior + 1;
    err = nvs_set_u64(nvs, NVS_KEY_SESSION, nova);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE("SEC", "Falha ao persistir sessao: %s", esp_err_to_name(err));
        return false;
    }
    g_session_counter = nova;
    g_seq_counter = 1;
    ESP_LOGI("SEC", "Nova sessao persistente: %llu", (unsigned long long)nova);
    return true;
}

static bool preparar_nonce(uint8_t nonce[GCM_NONCE_SIZE], uint64_t *session, uint32_t *seq)
{
    if (g_seq_counter == UINT32_MAX && !iniciar_nova_sessao()) return false;
    *session = g_session_counter;
    *seq = g_seq_counter++;
    write_u64_be(&nonce[0], *session);
    write_u32_be(&nonce[8], *seq);
    return true;
}

static void write_float_be(uint8_t *dst, float value)
{
    uint32_t raw;
    memcpy(&raw, &value, sizeof(raw));
    write_u32_be(dst, raw);
}

static void construir_header(uint8_t header[HEADER_SIZE], uint32_t seq_num)
{
    header[0] = PROTOCOL_VERSION;
    header[1] = DEV_TYPE_PM5100;
    header[2] = NODE_SRC_ID;
    header[3] = NODE_DST_ID;
    write_u32_be(&header[4], seq_num);
}

static void serializar_payload(const pm5100_data_t *dados, uint8_t payload[PM5100_PAYLOAD_SIZE])
{
    write_float_be(&payload[0], dados->tensao);
    write_float_be(&payload[4], dados->corrente);
    write_float_be(&payload[8], dados->fp);
    write_float_be(&payload[12], dados->potencia);
    write_u64_be(&payload[16], dados->energia);
    payload[24] = dados->valid_mask;
}

static uint16_t calcula_crc16(const uint8_t *buffer, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t pos = 0; pos < length; pos++) {
        crc ^= buffer[pos];
        for (int i = 0; i < 8; i++) {
            if (crc & 0x0001) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static void inserir_crc(uint8_t *pacote)
{
    uint16_t crc = calcula_crc16(pacote, 6);
    pacote[6] = (uint8_t)(crc & 0xFF);
    pacote[7] = (uint8_t)((crc >> 8) & 0xFF);
}

static float converter_modbus_para_float(const uint8_t *payload)
{
    uint32_t raw = ((uint32_t)payload[0] << 24) |
                   ((uint32_t)payload[1] << 16) |
                   ((uint32_t)payload[2] << 8) |
                   (uint32_t)payload[3];
    float valor;
    memcpy(&valor, &raw, sizeof(valor));
    return valor;
}

static uint64_t converter_modbus_para_uint64(const uint8_t *payload)
{
    uint64_t valor = 0;
    for (int i = 0; i < 8; i++) valor = (valor << 8) | payload[i];
    return valor;
}

static int uart_read_exact(uint8_t *buffer, size_t quantidade, TickType_t timeout_total)
{
    size_t total = 0;
    TickType_t inicio = xTaskGetTickCount();
    while (total < quantidade) {
        TickType_t decorrido = xTaskGetTickCount() - inicio;
        if (decorrido >= timeout_total) break;
        int recebidos = uart_read_bytes(UART_PORT, buffer + total, quantidade - total,
                                        timeout_total - decorrido);
        if (recebidos <= 0) break;
        total += recebidos;
    }
    return (int)total;
}

static int receber_resposta_modbus(uint8_t *resposta, size_t capacidade)
{
    if (capacidade < 5) return -1;
    int recebidos = uart_read_exact(resposta, 3, pdMS_TO_TICKS(500));
    if (recebidos != 3) {
        ESP_LOGW("MODBUS", "Timeout recebendo cabecalho Modbus");
        return recebidos;
    }
    if (resposta[1] & 0x80) {
        int restante = uart_read_exact(&resposta[3], 2, pdMS_TO_TICKS(200));
        return 3 + restante;
    }
    size_t restante = (size_t)resposta[2] + 2;
    if (3 + restante > capacidade) {
        ESP_LOGE("MODBUS", "Resposta Modbus excede buffer");
        return -1;
    }
    return 3 + uart_read_exact(&resposta[3], restante, pdMS_TO_TICKS(500));
}

static bool validar_resposta_modbus(const uint8_t *resposta, size_t tamanho,
                                    uint8_t bytes_dados_esperados)
{
    if (tamanho < 5) {
        ESP_LOGW("MODBUS", "Resposta Modbus muito curta: %d bytes", (int)tamanho);
        return false;
    }
    if (resposta[0] != MODBUS_SLAVE_ID) {
        ESP_LOGW("MODBUS", "Slave ID incorreto: 0x%02X", resposta[0]);
        return false;
    }
    uint16_t crc_recebido = resposta[tamanho - 2] | ((uint16_t)resposta[tamanho - 1] << 8);
    uint16_t crc_calculado = calcula_crc16(resposta, tamanho - 2);
    if (crc_recebido != crc_calculado) {
        ESP_LOGW("MODBUS", "CRC invalido: recebido=0x%04X calculado=0x%04X",
                 crc_recebido, crc_calculado);
        return false;
    }
    if (resposta[1] & 0x80) {
        ESP_LOGE("MODBUS", "Excecao Modbus | Function=0x%02X | Code=0x%02X",
                 resposta[1], resposta[2]);
        return false;
    }
    if (resposta[1] != MODBUS_FUNCTION) {
        ESP_LOGW("MODBUS", "Function Code inesperado: 0x%02X", resposta[1]);
        return false;
    }
    if (resposta[2] != bytes_dados_esperados) {
        ESP_LOGW("MODBUS", "Byte Count inesperado: recebido=%u esperado=%u",
                 resposta[2], bytes_dados_esperados);
        return false;
    }
    size_t tamanho_esperado = 3 + bytes_dados_esperados + 2;
    if (tamanho != tamanho_esperado) {
        ESP_LOGW("MODBUS", "Tamanho inesperado: recebido=%d esperado=%d",
                 (int)tamanho, (int)tamanho_esperado);
        return false;
    }
    return true;
}

static bool enviar_requisicao_modbus(uint8_t requisicao[8])
{
    inserir_crc(requisicao);
    uart_flush_input(UART_PORT);
    if (uart_write_bytes(UART_PORT, (const char *)requisicao, 8) != 8) {
        ESP_LOGE("MODBUS", "Falha ao enviar requisicao");
        return false;
    }
    if (uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(200)) != ESP_OK) {
        ESP_LOGE("MODBUS", "Timeout na transmissao UART");
        return false;
    }
    return true;
}

static bool modbus_read_float(uint8_t requisicao[8], float *valor)
{
    uint8_t resposta[16];
    if (!enviar_requisicao_modbus(requisicao)) return false;
    int recebidos = receber_resposta_modbus(resposta, sizeof(resposta));
    if (recebidos <= 0 || !validar_resposta_modbus(resposta, recebidos, 4)) return false;
    *valor = converter_modbus_para_float(&resposta[3]);
    return true;
}

static bool modbus_read_uint64(uint8_t requisicao[8], uint64_t *valor)
{
    uint8_t resposta[16];
    if (!enviar_requisicao_modbus(requisicao)) return false;
    int recebidos = receber_resposta_modbus(resposta, sizeof(resposta));
    if (recebidos <= 0 || !validar_resposta_modbus(resposta, recebidos, 8)) return false;
    *valor = converter_modbus_para_uint64(&resposta[3]);
    return true;
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
    lora_write_reg(REG_IRQ_FLAGS, 0xFF);
    ESP_LOGI("LORA", "LoRa configurado: 915 MHz | BW125 | SF7 | CR4/5 | CRC ON");
}

static bool lora_send_bytes(const uint8_t *data, size_t len)
{
    if (len > 255) {
        ESP_LOGE("LORA_TX", "Payload excede limite de 255 bytes");
        return false;
    }
    lora_write_reg(REG_OP_MODE, 0x81);
    lora_write_reg(REG_IRQ_FLAGS, 0xFF);
    lora_write_reg(REG_FIFO_ADDR_PTR, 0x00);
    for (size_t i = 0; i < len; i++) lora_write_reg(REG_FIFO, data[i]);
    lora_write_reg(REG_PAYLOAD_LENGTH, (uint8_t)len);
    lora_write_reg(REG_OP_MODE, 0x83);
    for (int tentativas = 0; ; tentativas++) {
        if (lora_read_reg(REG_IRQ_FLAGS) & IRQ_TX_DONE) break;
        if (tentativas >= 500) {
            ESP_LOGE("LORA_TX", "Timeout aguardando TX Done");
            lora_write_reg(REG_OP_MODE, 0x81);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    lora_write_reg(REG_IRQ_FLAGS, IRQ_TX_DONE);
    lora_write_reg(REG_OP_MODE, 0x81);
    ESP_LOGI("LORA_TX", "Pacote transmitido: %d bytes", (int)len);
    return true;
}

static void init_uart_modbus(void)
{
    const uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, TXD_PIN, RXD_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_mode(UART_PORT, UART_MODE_UART));
    uart_flush_input(UART_PORT);
    ESP_LOGI("MODBUS", "UART2 inicializada: 9600 8N1");
}

static bool aes_gcm_encrypt(const uint8_t *plaintext, size_t plaintext_len,
                            const uint8_t *key, const uint8_t *nonce,
                            const uint8_t *aad, size_t aad_len,
                            uint8_t *ciphertext, uint8_t tag[GCM_TAG_SIZE])
{
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, AES_KEY_SIZE * 8);
    if (ret != 0) {
        ESP_LOGE(TAG, "Falha mbedtls_gcm_setkey: %d", ret);
        mbedtls_gcm_free(&gcm);
        return false;
    }
    ret = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plaintext_len,
                                    nonce, GCM_NONCE_SIZE, aad, aad_len,
                                    plaintext, ciphertext, GCM_TAG_SIZE, tag);
    mbedtls_gcm_free(&gcm);
    if (ret != 0) {
        ESP_LOGE(TAG, "Falha AES-GCM: %d", ret);
        return false;
    }
    return true;
}

static bool processar_e_enviar_pacote(const pm5100_data_t *dados)
{
    uint8_t header[HEADER_SIZE], plaintext[PM5100_PAYLOAD_SIZE];
    uint8_t nonce[GCM_NONCE_SIZE], ciphertext[PM5100_PAYLOAD_SIZE];
    uint8_t auth_tag[GCM_TAG_SIZE], frame[FRAME_SIZE];
    uint64_t session;
    uint32_t seq;
    if (!preparar_nonce(nonce, &session, &seq)) return false;
    construir_header(header, seq);
    serializar_payload(dados, plaintext);
    if (!aes_gcm_encrypt(plaintext, sizeof(plaintext), aes_key, nonce,
                         header, sizeof(header), ciphertext, auth_tag)) {
        ESP_LOGE(TAG, "Falha ao criptografar pacote");
        return false;
    }
    size_t offset = 0;
    memcpy(&frame[offset], header, HEADER_SIZE); offset += HEADER_SIZE;
    memcpy(&frame[offset], nonce, GCM_NONCE_SIZE); offset += GCM_NONCE_SIZE;
    memcpy(&frame[offset], ciphertext, PM5100_PAYLOAD_SIZE); offset += PM5100_PAYLOAD_SIZE;
    memcpy(&frame[offset], auth_tag, GCM_TAG_SIZE);
    if (!lora_send_bytes(frame, FRAME_SIZE)) {
        ESP_LOGE(TAG, "Falha na transmissao LoRa");
        return false;
    }
    ESP_LOGI(TAG, "Frame enviado | Session=%llu | Seq=%lu | Total=%d",
             (unsigned long long)session, (unsigned long)seq, FRAME_SIZE);
    return true;
}


#if TCC_MODO_ENSAIO
#define TEST_NVS_NAMESPACE "tcc_laboratorio"
#define TEST_NVS_OLD_FRAME "oldframe"

static const pm5100_data_t dados_fixos = {
    .tensao = 230.50f, .corrente = 5.20f, .fp = 0.970f,
    .potencia = 1100.0f, .energia = 123456u, .valid_mask = VAL_TODOS
};

static bool montar_frame_teste(uint8_t frame[FRAME_SIZE], uint64_t *session, uint32_t *seq)
{
    uint8_t header[HEADER_SIZE], plaintext[PM5100_PAYLOAD_SIZE];
    uint8_t nonce[GCM_NONCE_SIZE], ciphertext[PM5100_PAYLOAD_SIZE], tag[GCM_TAG_SIZE];
    if (!preparar_nonce(nonce, session, seq)) return false;
    construir_header(header, *seq);
    serializar_payload(&dados_fixos, plaintext);
    if (!aes_gcm_encrypt(plaintext, sizeof(plaintext), aes_key, nonce,
                         header, sizeof(header), ciphertext, tag)) return false;
    memcpy(frame, header, HEADER_SIZE);
    memcpy(frame + HEADER_SIZE, nonce, GCM_NONCE_SIZE);
    memcpy(frame + HEADER_SIZE + GCM_NONCE_SIZE, ciphertext, PM5100_PAYLOAD_SIZE);
    memcpy(frame + HEADER_SIZE + GCM_NONCE_SIZE + PM5100_PAYLOAD_SIZE, tag, GCM_TAG_SIZE);
    limpar_segredo(plaintext, sizeof(plaintext));
    return true;
}

// Todas as alteracoes ocorrem APOS a cifragem. Nunca cifrar duas mensagens
// distintas com a mesma combinacao de chave e nonce AES-GCM.
static bool enviar_etapa(const char *teste, const uint8_t frame[FRAME_SIZE],
                         uint64_t session, uint32_t seq)
{
    printf("TEST_TX,START,%s,%llu,%lu\n", teste,
           (unsigned long long)session, (unsigned long)seq);
    fflush(stdout);
    bool ok = lora_send_bytes(frame, FRAME_SIZE);
    printf("TEST_TX,END,%s,%s,%llu,%lu\n", teste, ok ? "TX_DONE" : "TX_FAIL",
           (unsigned long long)session, (unsigned long)seq);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(TEMPO_ENTRE_TESTES_MS));
    return ok;
}

static bool gravar_frame_antigo(const uint8_t frame[FRAME_SIZE])
{
    nvs_handle_t nvs;
    if (nvs_open(TEST_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(nvs, TEST_NVS_OLD_FRAME, frame, FRAME_SIZE);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}

static bool ler_frame_antigo(uint8_t frame[FRAME_SIZE])
{
    nvs_handle_t nvs;
    if (nvs_open(TEST_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t tam = FRAME_SIZE;
    esp_err_t err = nvs_get_blob(nvs, TEST_NVS_OLD_FRAME, frame, &tam);
    nvs_close(nvs);
    return err == ESP_OK && tam == FRAME_SIZE;
}

static bool apagar_frame_antigo(void)
{
    nvs_handle_t nvs;
    if (nvs_open(TEST_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_erase_key(nvs, TEST_NVS_OLD_FRAME);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}

static uint32_t seq_do_frame(const uint8_t frame[FRAME_SIZE])
{
    return ((uint32_t)frame[16] << 24) | ((uint32_t)frame[17] << 16) |
           ((uint32_t)frame[18] << 8) | frame[19];
}

static uint64_t session_do_frame(const uint8_t frame[FRAME_SIZE])
{
    uint64_t v = 0;
    for (int i = 8; i < 16; i++) v = (v << 8) | frame[i];
    return v;
}

static void executar_ensaios_seguranca(void)
{
    uint8_t original[FRAME_SIZE], f[FRAME_SIZE], baixo[FRAME_SIZE], alto[FRAME_SIZE];
    uint64_t s, s2, s3;
    uint32_t q, q2, q3;
    // Aguardar o coletor abrir as portas antes de iniciar a bateria.
    printf("TEST_TX,READY,%llu\n", (unsigned long long)g_session_counter);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(6000));

    if (ler_frame_antigo(original)) {
        uint64_t antiga_s = session_do_frame(original);
        uint32_t antiga_q = seq_do_frame(original);
        printf("TEST_TX,PHASE,2,%llu,%lu\n", (unsigned long long)g_session_counter, g_seq_counter);
        fflush(stdout);
        // E10: apos o operador reiniciar o RX, deve recusar esse frame
        // por causa do limite de sequencia restaurado da NVS.
        if (!enviar_etapa("E10_REPLAY_APOS_REBOOT_RX", original, antiga_s, antiga_q)) return;
        if (!montar_frame_teste(f, &s, &q) ||
            !enviar_etapa("E09_NOVA_SESSAO_TX", f, s, q)) return;
        if (!enviar_etapa("E08_SESSAO_ANTIGA", original, antiga_s, antiga_q)) return;
        if (!apagar_frame_antigo()) ESP_LOGW("TESTE", "Falha ao limpar etapa persistida");
        printf("TEST_TX,FINISH,2,%llu\n", (unsigned long long)g_session_counter);
        fflush(stdout);
        return;
    }

    printf("TEST_TX,PHASE,1,%llu,%lu\n", (unsigned long long)g_session_counter, g_seq_counter);
    fflush(stdout);
    if (!montar_frame_teste(original, &s, &q) ||
        !enviar_etapa("E04_VALIDO", original, s, q)) return;

    if (!montar_frame_teste(f, &s2, &q2)) return;
    f[HEADER_SIZE + GCM_NONCE_SIZE + 3] ^= 0x01;
    if (!enviar_etapa("E05_CIPHERTEXT", f, s2, q2)) return;

    if (!montar_frame_teste(f, &s2, &q2)) return;
    f[7] ^= 0x01; // Altera Sequence do AAD; mantem versao e IDs validos.
    if (!enviar_etapa("E05_AAD", f, s2, q2)) return;

    if (!montar_frame_teste(f, &s2, &q2)) return;
    f[FRAME_SIZE - 1] ^= 0x01;
    if (!enviar_etapa("E05_TAG", f, s2, q2)) return;

    if (!enviar_etapa("E06_REPLAY_IDENTICO", original, s, q)) return;

    if (!montar_frame_teste(baixo, &s2, &q2)) return;
    if (!montar_frame_teste(alto, &s3, &q3)) return;
    if (!enviar_etapa("E07_SEQ_SUPERIOR", alto, s3, q3)) return;
    if (!enviar_etapa("E07_SEQ_INFERIOR", baixo, s2, q2)) return;

    // Persistir o frame ORIGINAL apenas para retransmissao entre reboots;
    // nunca usá-lo para uma nova cifragem com o mesmo nonce.
    if (!gravar_frame_antigo(original)) {
        ESP_LOGE("TESTE", "Nao foi possivel armazenar frame para etapa 2");
        return;
    }
    printf("TEST_TX,FINISH,1,%llu\n", (unsigned long long)g_session_counter);
    printf("TEST_TX,INSTRUCTION,REINICIE_RX_DEPOIS_TX_SEM_APAGAR_NVS\n");
    fflush(stdout);
}
#endif

void app_main(void)
{
    if (!inicializar_chave_puf()) {
        ESP_LOGE("PUF", "Falha ao reconstruir chave AES pela SRAM-PUF");
        return;
    }
    ESP_LOGI("PUF", "Chave AES derivada pela RTC SLOW (HKDF-SHA256)");
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "TCC - Telemetria Industrial Segura | Heltec WiFi LoRa 32 V2");
    ESP_LOGI(TAG, "Modbus RTU + AES-128-GCM + LoRa P2P | Protocolo v%d | Frame=%d bytes",
             PROTOCOL_VERSION, FRAME_SIZE);
    init_nvs();
    if (!iniciar_nova_sessao()) {
        ESP_LOGE(TAG, "Nao foi possivel iniciar sessao segura");
        return;
    }
    ESP_LOGI(TAG, "Inicializando LoRa...");
    lora_init();
#if TCC_MODO_ENSAIO
    ESP_LOGW(TAG, "MODO ENSAIO: nao consultara o medidor Modbus");
    executar_ensaios_seguranca();
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
#else
    ESP_LOGI(TAG, "Inicializando UART Modbus...");
    init_uart_modbus();
    uint8_t req_tensao[8]   = {0x01, 0x03, 0x0B, 0xD3, 0x00, 0x02, 0x00, 0x00};
    uint8_t req_corrente[8] = {0x01, 0x03, 0x0B, 0xB7, 0x00, 0x02, 0x00, 0x00};
    uint8_t req_fp[8]       = {0x01, 0x03, 0x0C, 0x05, 0x00, 0x02, 0x00, 0x00};
    uint8_t req_potencia[8] = {0x01, 0x03, 0x0B, 0xED, 0x00, 0x02, 0x00, 0x00};
    uint8_t req_energia[8]  = {0x01, 0x03, 0x0C, 0x83, 0x00, 0x04, 0x00, 0x00};
    while (1) {
        pm5100_data_t dados = {0};
        ESP_LOGI("MODBUS", "Iniciando ciclo de leitura...");
        if (modbus_read_float(req_tensao, &dados.tensao) && isfinite(dados.tensao)) dados.valid_mask |= VAL_TENSAO;
        vTaskDelay(pdMS_TO_TICKS(200));
        if (modbus_read_float(req_corrente, &dados.corrente) && isfinite(dados.corrente)) dados.valid_mask |= VAL_CORRENTE;
        vTaskDelay(pdMS_TO_TICKS(200));
        if (modbus_read_float(req_fp, &dados.fp) && isfinite(dados.fp)) dados.valid_mask |= VAL_FP;
        vTaskDelay(pdMS_TO_TICKS(200));
        if (modbus_read_float(req_potencia, &dados.potencia) && isfinite(dados.potencia)) dados.valid_mask |= VAL_POTENCIA;
        vTaskDelay(pdMS_TO_TICKS(200));
        if (modbus_read_uint64(req_energia, &dados.energia)) dados.valid_mask |= VAL_ENERGIA;
        if (dados.valid_mask & VAL_TENSAO) ESP_LOGI("MEDIDOR", "Tensao: %.2f V", dados.tensao);
        else ESP_LOGW("MEDIDOR", "Tensao: leitura indisponivel");
        if (dados.valid_mask & VAL_CORRENTE) ESP_LOGI("MEDIDOR", "Corrente: %.2f A", dados.corrente);
        else ESP_LOGW("MEDIDOR", "Corrente: leitura indisponivel");
        if (dados.valid_mask & VAL_FP) ESP_LOGI("MEDIDOR", "FP: %.3f", dados.fp);
        else ESP_LOGW("MEDIDOR", "FP: leitura indisponivel");
        if (dados.valid_mask & VAL_POTENCIA) ESP_LOGI("MEDIDOR", "Potencia: %.2f W", dados.potencia);
        else ESP_LOGW("MEDIDOR", "Potencia: leitura indisponivel");
        if (dados.valid_mask & VAL_ENERGIA) ESP_LOGI("MEDIDOR", "Energia: %llu Wh", (unsigned long long)dados.energia);
        else ESP_LOGW("MEDIDOR", "Energia: leitura indisponivel");
        ESP_LOGI("MODBUS", "Validade=0x%02X | %s", dados.valid_mask,
                 dados.valid_mask == VAL_TODOS ? "ciclo completo" : "ciclo parcial");
        processar_e_enviar_pacote(&dados);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
#endif
}

