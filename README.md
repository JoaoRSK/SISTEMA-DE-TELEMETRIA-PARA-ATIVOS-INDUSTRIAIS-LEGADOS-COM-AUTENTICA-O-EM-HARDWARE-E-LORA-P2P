# Sistema de Telemetria para Ativos Industriais Legados com Autenticação em Hardware e LoRa P2P

Repositório de códigos-fonte e roteiros de ensaio do Trabalho de Conclusão de Curso (TCC) em Engenharia Eletrônica da Universidade Tecnológica Federal do Paraná (UTFPR), desenvolvido em 2026 por **João Roberto dos Santos Klassen** e **Luiz Eduardo Prado de Oliveira**.

**Repositório:** https://github.com/JoaoRSK/SISTEMA-DE-TELEMETRIA-PARA-ATIVOS-INDUSTRIAIS-LEGADOS-COM-AUTENTICA-O-EM-HARDWARE-E-LORA-P2P

## Objetivo e arquitetura

O protótipo foi projetado para adquirir grandezas elétricas de um medidor industrial legado, transmiti-las por LoRa ponto a ponto e encaminhá-las a uma plataforma de monitoramento após validação criptográfica.

```text
Medidor Schneider PM5100
        │ RS-485 / Modbus RTU
        ▼
ESP32 Heltec V2 (transmissor)
        │ LoRa P2P a 915 MHz
        ▼
ESP32 Heltec V2 (receptor)
        │ Ethernet W5500 / TCP
        ▼
Servidor Zabbix (itens trapper / protocolo Zabbix Sender)
```

A solução utiliza AES-128-GCM para confidencialidade e autenticação das mensagens, identificação de sessão e número de sequência para controle de repetição, e estudo da SRAM-PUF do ESP32 como fonte de material para derivação de chave. Há também ensaios isolados dos mecanismos *Secure Boot*, *Flash Encryption* e *eFuses*.

## Organização dos diretórios

| Diretório | Conteúdo |
| --- | --- |
| [`firmware-principal/`](firmware-principal/) | Código C dos nós transmissor (TX) e receptor (RX), incluindo telemetria, LoRa, AES-GCM, NVS e envio ao Zabbix. |
| [`coleta-sram-puf/`](coleta-sram-puf/) | Coletor embarcado, recepção de amostras e análise estatística da SRAM-PUF em Python. |
| [`ensaio-autenticacao/`](ensaio-autenticacao/) | Firmwares e script para ensaios de integridade, autenticação e proteção contra repetição. |
| [`ensaio-hardening/`](ensaio-hardening/) | Aplicação de diagnóstico e roteiros de avaliação de Secure Boot V2, Flash Encryption e eFuses. |
| [`ensaio-lora/`](ensaio-lora/) | Firmwares e scripts para ensaios de enlace LoRa, incluindo PDR, RSSI e SNR. |

Cada diretório contém um `README.md` com descrição dos arquivos, entradas, saídas e limitações.

## Plataformas utilizadas

- **Hardware:** placas Heltec WiFi LoRa 32 V2 (ESP32/SX127x), módulo Ethernet W5500 e medidor Schneider PM5100.
- **Firmware:** linguagem C e ESP-IDF; os projetos foram desenvolvidos no ecossistema PlatformIO.
- **Análise:** Python 3, NumPy, Matplotlib e PySerial conforme o ensaio.
- **Monitoramento:** servidor Zabbix acessado pelo receptor via Ethernet.

Instale as dependências dos scripts Python com:

```bash
python -m pip install -r requirements.txt
```

Os arquivos disponibilizados contêm os códigos `.c`, `.h`, `.py` e os roteiros `.txt` presentes no conjunto entregue para publicação. **Não incluem os projetos completos de compilação** (`platformio.ini`, `CMakeLists.txt`, `sdkconfig` e configurações específicas de cada placa). Portanto, para recompilar, é necessário configurar localmente um projeto ESP-IDF/PlatformIO compatível e consultar as orientações de cada diretório. Esta publicação não representa uma compilação ou execução novamente verificada.

## Segurança e dados não publicados



- Chaves AES utilizadas no protótipo, chaves privadas de assinatura e segredos de dispositivos.
- `gateway_key.h`, respostas brutas da SRAM-PUF (`sample_*.bin`), saídas que contenham respostas secretas e arquivos particulares de configuração.
- `puf_indices.h` produzido para uma placa real, quando esses índices fizerem parte do mecanismo de derivação e provisionamento da chave.
- Credenciais, tokens de acesso e arquivos locais com senhas.



As amostras brutas, os CSVs gerados e os arquivos de compilação também não foram incluídos neste pacote. Assim, o repositório documenta a implementação e o procedimento, mas não contém todos os dados necessários para reproduzir integralmente os resultados estatísticos do TCC.

## Relação com o TCC

Os diretórios correspondem aos ensaios descritos na **Metodologia (Capítulo 3)** e aos resultados discutidos no **Capítulo 4**.

## Licença

Os arquivos de autoria dos participantes do TCC estão disponibilizados sob a **Licença MIT**, conforme [`LICENSE`](LICENSE). Marcas, bibliotecas externas e documentação de terceiros permanecem sujeitos às suas respectivas condições de uso. A adoção da licença deve ter concordância de ambos os autores.
