# Ensaios do enlace LoRa P2P

Códigos dedicados à avaliação do enlace de rádio em pontos de medição externos. Utilizam quadros de teste com tamanho equivalente ao do protocolo principal, mas **não são equivalentes ao sistema completo de telemetria criptografada**.

## Arquivos

- `C/TX/main.c` e `C/TX/protocol.h`: emissor de séries de teste; o botão `PRG` inicia uma campanha de 100 quadros.
- `C/RX/main.c` e `C/RX/protocol.h`: receptor que verifica o formato do quadro e registra as métricas RSSI, SNR e sequência no terminal serial.
- `Python/coletar_lora.py`: coleta os eventos registrados pelo receptor e gera CSVs.
- `Python/analisar_lora.py`: calcula a taxa de entrega de pacotes (PDR), a perda de pacotes (PLR) e estatísticas de RSSI e SNR, além de gerar gráficos quando Matplotlib está instalado.

## Configuração do ensaio

Os códigos utilizam 915 MHz, largura de banda de 125 kHz, SF7, CR 4/5 e quadros de **61 bytes**. Cada campanha contempla **100 transmissões**, com intervalo de aproximadamente 1 segundo.

Em `Python/coletar_lora.py`, edite antes de cada ponto:

- `PORTA`: porta serial do receptor (por padrão `COM3` no arquivo fornecido).
- `PONTO`: identificador do ponto da campanha.
- `DISTANCIA_M`: distância adotada para o ponto.
- `OBSTRUCAO`: descrição das condições de propagação.

Os valores iniciais desses campos são exemplos de uma configuração local e **não representam necessariamente todos os pontos do estudo**.

Com o firmware TX/RX apropriado em execução, inicie o coletor e acione o transmissor conforme as mensagens do programa:

```bash
python Python/coletar_lora.py
```

O CSV ficará em `coletas/eventos_<PONTO>_<data>.csv`. Depois execute:

```bash
python Python/analisar_lora.py
```

Os arquivos `resultados/resumo_lora.csv`, `rssi_distancia.png`, `snr_distancia.png` e `pdr_distancia.png` são gerados a partir das coletas.

## Interpretação e limitações

As métricas descrevem as condições específicas de cada local, incluindo obstáculos e propagação sem linha de visada (NLOS), e não permitem extrapolar o desempenho para qualquer ambiente. O repositório contém os programas, mas não inclui os CSVs originais de campo. Cada ponto deve corresponder a uma série identificável para a interpretação adequada das perdas.
