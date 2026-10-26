# Firmware principal: transmissor e receptor

Implementação central do protótipo de telemetria apresentado no TCC.

## Arquivos

| Arquivo | Finalidade |
| --- | --- |
| `TX/main.c` | Consulta Modbus RTU do medidor PM5100, derivação da chave a partir da resposta SRAM-PUF, criação do quadro autenticado e envio LoRa P2P. |
| `RX/main.c` | Recepção LoRa, validação AES-GCM, controle de sessão/sequência e encaminhamento de dados ao Zabbix por Ethernet W5500. |

## Arquitetura dos dados

- **Coleta:** RS-485 / Modbus RTU, função `0x03`, 9600 bit/s, 8N1, identificação do medidor `0x01`.
- **Rádio:** SX127x / LoRa P2P, 915 MHz, BW de 125 kHz, SF7 e CR 4/5 conforme a configuração presente nos arquivos.
- **Quadro de aplicação:** 61 bytes = cabeçalho autenticado (8 B) + nonce (12 B) + payload cifrado (25 B) + tag GCM (16 B).
- **Payload:** tensão, corrente, fator de potência, potência, energia e máscara de validade.
- **Proteção:** AES-128-GCM e mecanismo de rejeição de repetição com estado mantido em NVS.
- **Gateway:** Ethernet W5500 e protocolo Zabbix Sender para envio de métricas ao Zabbix.

## Requisitos e observações

1. Preparar projetos locais ESP-IDF/PlatformIO apropriados para a Heltec WiFi LoRa 32 V2, incorporando os arquivos `main.c` aos componentes correspondentes.
2. Ajustar os pinos e parâmetros de rede conforme a montagem real. O código RX contém endereços IPv4 fixos para a rede de laboratório.
3. Fornecer ao TX o cabeçalho local `puf_indices.h` para a placa caracterizada e, ao RX, `gateway_key.h` com o material previamente provisionado. Esses arquivos não foram incluídos para evitar publicação de dados específicos ou sensíveis.
4. Configurar o medidor, o servidor Zabbix e os itens *trapper* conforme a metodologia do TCC.

**Atenção:** a existência dos códigos-fonte não implica que a reconstrução SRAM-PUF seja exata em toda inicialização. Os dados experimentais do TCC incluem falhas ocasionais de reconstrução. Não reutilize chaves reais em projetos públicos e não apague o estado persistente de sessão/antirrepetição sem considerar o ciclo de vida das chaves.

**Limitação de distribuição:** este diretório não inclui `platformio.ini`, `CMakeLists.txt`, bibliotecas incorporadas nem arquivos de provisionamento. Nenhuma compilação foi executada durante a organização do repositório.
