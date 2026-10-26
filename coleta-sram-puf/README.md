# Coleta e caracterização da SRAM-PUF

Scripts e firmware utilizados na aquisição da inicialização da RTC SLOW SRAM do ESP32 e na análise estatística de duas placas.

## Arquivos

- `C/main.c`: lê um trecho de 8192 bytes da RTC SLOW SRAM e envia o *snapshot* por TCP/Ethernet W5500 com um cabeçalho de transferência.
- `Python/collect_puf.py`: servidor TCP na porta 5000 que recebe amostras e cria arquivos `sample_*.bin`, além de metadados.
- `Python/analyze_puf_duas_placas.py`: análise das duas placas, com estabilidade por bit, Hamming Weight, Hamming Distance intra/interdispositivo, seleção de 128 posições e validação 80/20.

## Procedimento utilizado

A caracterização considera **100 aquisições por placa**, cada uma com **8192 bytes (65.536 bits)**. Entre as leituras, a alimentação era desligada fisicamente por aproximadamente **10 segundos**, permitindo nova inicialização. Reiniciar o software sem remover a energia não é necessariamente equivalente.

A amostra deve ser capturada no início da aplicação, antes de outras rotinas que possam escrever na região de memória avaliada.

Para executar o receptor no computador, a partir de `coleta-sram-puf/Python`:

```bash
python collect_puf.py
```

Para a análise, o script procura precisamente estas pastas ao lado do próprio arquivo:

```text
Python/
  puf_samples_1/
    sample_001.bin ... sample_100.bin
  puf_samples_2/
    sample_001.bin ... sample_100.bin
  analyze_puf_duas_placas.py
```

Após preencher ambas as pastas, execute:

```bash
python analyze_puf_duas_placas.py
```

Saídas são gravadas em `Python/puf_analysis/`, incluindo CSVs, gráficos, bancos SQLite e o cabeçalho `puf_indices.h`. As figuras representam a ordem lógica dos bits lidos, **não um mapa físico do chip**.

## Cuidados

- Ajuste os endereços IP em `C/main.c` à rede utilizada na coleta e configure o W5500.
- O receptor Python usa `Path("puf_samples")` relativo ao **diretório atual do terminal**. Para cada placa, organize/renomeie as coletas conforme os nomes exigidos pelo analisador.
- **Não publique** `sample_*.bin`, respostas extraídas ou `puf_indices.h` de um dispositivo empregado com chave real. O `.gitignore` exclui esses arquivos por padrão.
- A seleção de bits estáveis não garante reconstrução sem erros. A validação independente do TCC não foi 100% exata em ambas as placas; não há *fuzzy extractor* ou ECC implementado neste conjunto de códigos.
- Os dados binários não estão neste repositório: o script de análise requer amostras reais para execução completa.
