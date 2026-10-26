# Ensaios de autenticação e antirrepetição

Conjunto específico de testes dos mecanismos de integridade/autenticidade AES-128-GCM, identificação de sessão e número de sequência usados no enlace LoRa.

## Arquivos

- `C/TX/tx.c`: transmissor de ensaios com cenários de quadros válidos, alterações deliberadas dos campos autenticados e repetição de mensagens.
- `C/RX/rx.c`: receptor que valida os quadros e registra a decisão de autenticação/antirrepetição.
- `Python/coletar_e_validar.py`: coleta simultânea de logs seriais TX/RX e gera uma tabela de comparação entre os resultados esperados e observados.

## Cenários estudados

São avaliados quadros válidos, alterações no texto cifrado, no cabeçalho autenticado (AAD) ou na tag, reprodução da mesma sequência, tentativa de sequência inferior, utilização de sessão anterior e comportamento após reinicialização do receptor.

O script registra decisões como `ACCEPT`, `GCM_FAIL`, `REPLAY_SEQ` e `REPLAY_SESSION`. O resultado dos testes depende dos logs efetivamente recebidos; não se deve tratar a mera transmissão do quadro como comprovação de aceitação no RX.

## Execução do coletor

No início de `Python/coletar_e_validar.py`, ajuste `PORTA_TX`, `PORTA_RX`, `BAUDRATE` e `DURACAO_SEGUNDOS` conforme as portas dos ESP32. Os valores presentes no código refletem a configuração local do ensaio.

```bash
python Python/coletar_e_validar.py
```

O script usa **PySerial** e salva os registros na pasta `Python/resultados/`. Os resultados gerados não acompanham esta distribuição.

## Observações de segurança

Os firmwares exigem cabeçalhos locais não incluídos nesta versão: `puf_indices.h` (TX) e `gateway_key.h` (RX). Não publique material criptográfico real. Os cenários de falha de autenticação não representam uma análise exaustiva de segurança; são ensaios controlados para os casos descritos no TCC.
