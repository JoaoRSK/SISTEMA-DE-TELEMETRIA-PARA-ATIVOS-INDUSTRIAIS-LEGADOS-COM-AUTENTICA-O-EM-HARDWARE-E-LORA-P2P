"""Coleta o ensaio LoRa pelo VS Code (Run Python File)."""
import csv
import sys
from datetime import datetime
from pathlib import Path

# CONFIGURACAO DO ENSAIO: edite aqui antes de clicar em Run Python File.
PORTA = "COM3"
BAUDRATE = 115200
PONTO = "P5"
DISTANCIA_M = 150.0
OBSTRUCAO = "NLOS alto"
PARAR_AO_RECEBER_100 = True
ARQUIVO_LOG_TESTE = None  # Ex.: Path("serial_exemplo.txt"), sem placa conectada.

BASE = Path(__file__).resolve().parent.parent
PASTA_SAIDA = BASE / "coletas"
TOTAL_PACOTES = 100
CAMPOS = ["data_hora", "ponto", "distancia_m", "obstrucao", "evento",
          "run_id", "seq", "rssi_dbm", "snr_db", "bytes", "detalhe"]

def interpretar_linha(linha):
    pos = linha.find("LRX,")
    if pos < 0:
        return None
    col = linha[pos:].strip().split(",")
    if len(col) == 7 and col[1] == "RX":
        try:
            run = col[2].upper()
            int(run, 16)
            seq, rssi = int(col[3]), int(col[4])
            snr, length = float(col[5]), int(col[6])
            if 1 <= seq <= TOTAL_PACOTES and length == 61:
                return {"evento": "RX", "run_id": run, "seq": seq,
                        "rssi_dbm": rssi, "snr_db": snr, "bytes": length, "detalhe": ""}
        except ValueError:
            pass
    if len(col) >= 2 and col[1] == "CRC":
        return {"evento": "CRC", "run_id": "", "seq": "", "rssi_dbm": "",
                "snr_db": "", "bytes": "", "detalhe": "CRC PHY invalido"}
    if len(col) >= 3 and col[1] == "INVALID":
        return {"evento": "INVALID", "run_id": "", "seq": "", "rssi_dbm": "",
                "snr_db": "", "bytes": col[3] if len(col) > 3 else "",
                "detalhe": col[2]}
    return None

def main():
    PASTA_SAIDA.mkdir(parents=True, exist_ok=True)
    momento = datetime.now().strftime("%Y%m%d_%H%M%S")
    arquivo = PASTA_SAIDA / f"eventos_{PONTO}_{momento}.csv"
    runs = {}
    totais = {"RX": 0, "CRC": 0, "INVALID": 0}
    porta = None
    origem = None
    try:
        if ARQUIVO_LOG_TESTE is not None:
            origem = Path(ARQUIVO_LOG_TESTE).open("r", encoding="utf-8", errors="replace")
        else:
            try:
                import serial
            except ImportError:
                print("Instale o pacote 'pyserial' pelo seletor de interpretador do VS Code ou terminal integrado.")
                return
            porta = serial.Serial(PORTA, BAUDRATE, timeout=1)
            origem = porta
        with arquivo.open("w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=CAMPOS)
            writer.writeheader()
            def salvar(evento):
                writer.writerow({"data_hora": datetime.now().astimezone().isoformat(timespec="milliseconds"),
                                 "ponto": PONTO, "distancia_m": DISTANCIA_M,
                                 "obstrucao": OBSTRUCAO, **evento})
                f.flush()
            # Mesmo sem RX, o analisador podera registrar um ponto com PDR=0%.
            salvar({"evento": "START", "run_id": "", "seq": "", "rssi_dbm": "",
                    "snr_db": "", "bytes": "", "detalhe": "Inicio de coleta"})
            print(f"Ponto: {PONTO} | Distancia: {DISTANCIA_M:g} m | Obstrucao: {OBSTRUCAO}")
            print(f"Origem: {ARQUIVO_LOG_TESTE or PORTA} | CSV: {arquivo}")
            print("Com a coleta ativa, pressione PRG no transmissor. Ctrl+C no terminal para terminar.")
            while True:
                raw = origem.readline()
                if not raw:
                    if ARQUIVO_LOG_TESTE is not None:
                        break
                    continue
                linha = raw.decode("utf-8", errors="replace") if isinstance(raw, bytes) else raw
                evento = interpretar_linha(linha)
                if evento is None:
                    continue
                salvar(evento)
                totais[evento["evento"]] += 1
                if evento["evento"] == "RX":
                    seqs = runs.setdefault(evento["run_id"], set())
                    seqs.add(evento["seq"])
                    print(f"RX run={evento['run_id']} seq={evento['seq']:03d} "
                          f"RSSI={evento['rssi_dbm']} dBm SNR={evento['snr_db']:.2f} dB "
                          f"({len(seqs)}/{TOTAL_PACOTES} unicos)")
                    if PARAR_AO_RECEBER_100 and len(seqs) == TOTAL_PACOTES:
                        print("100/100 pacotes unicos recebidos; coleta encerrada automaticamente.")
                        break
                else:
                    print(f"Evento {evento['evento']}: {evento['detalhe']}")
    except KeyboardInterrupt:
        print("\nColeta encerrada pelo usuario.")
    except (OSError, ValueError) as exc:
        print(f"Falha ao abrir/ler a porta ou arquivo: {exc}", file=sys.stderr)
        if "PermissionError" in type(exc).__name__:
            print("Feche o monitor serial do PlatformIO antes de abrir COM3.", file=sys.stderr)
    finally:
        if origem is not None:
            origem.close()
    print(f"Eventos: {totais}. CSV: {arquivo}")
    for run, seqs in sorted(runs.items()):
        ausentes = sorted(set(range(1, TOTAL_PACOTES + 1)) - seqs)
        print(f"Serie {run}: {len(seqs)}/{TOTAL_PACOTES} (PDR nominal={len(seqs)}%); ausentes={ausentes}")
    if not runs:
        print("Nenhum pacote RX valido. Confirme que o transmissor executou 100 TX Done.")

if __name__ == "__main__":
    main()
