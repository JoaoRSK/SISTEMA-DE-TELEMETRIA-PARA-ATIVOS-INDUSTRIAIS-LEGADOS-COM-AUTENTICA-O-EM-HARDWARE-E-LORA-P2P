#!/usr/bin/env python3
"""Resume RSSI, SNR, PDR e perdas de cada campanha LoRa por ponto/serie."""
import csv
from collections import defaultdict
from pathlib import Path
from statistics import mean, stdev

BASE = Path(__file__).resolve().parent.parent
PASTA_ENTRADA = BASE / "coletas"
PASTA_SAIDA = BASE / "resultados"
TOTAL_PACOTES = 100

COLUNAS = ["ponto", "distancia_m", "obstrucao", "run_id", "n_esperado",
           "n_recebidos_unicos", "n_duplicados", "n_crc_observados",
           "pdr_pct", "plr_pct", "rssi_medio_dbm", "rssi_desvio_db",
           "rssi_min_dbm", "rssi_max_dbm", "snr_medio_db", "snr_desvio_db",
           "snr_min_db", "snr_max_db", "sequencias_ausentes", "arquivo_origem"]

def stats(v):
    if not v:
        return ("", "", "", "")
    return (round(mean(v), 3), round(stdev(v), 3) if len(v) > 1 else 0.0,
            min(v), max(v))

def analisar(arquivos, total):
    resultado = []
    for caminho in arquivos:
        with caminho.open(newline="", encoding="utf-8") as f:
            registros = list(csv.DictReader(f))
        if not registros:
            continue
        info = registros[0]
        runs = defaultdict(list)
        for rec in registros:
            if rec["evento"] == "RX":
                runs[rec["run_id"]].append(rec)
        crc = sum(rec["evento"] == "CRC" for rec in registros)
        # Um arquivo deve corresponder a um ponto e a uma serie de 100 envios.
        for runid, dados in runs.items():
            unicos = {}
            for rec in dados:
                seq = int(rec["seq"])
                if 1 <= seq <= total:
                    unicos.setdefault(seq, rec)
            rssi = [float(r["rssi_dbm"]) for r in unicos.values()]
            snr = [float(r["snr_db"]) for r in unicos.values()]
            dist = float(info["distancia_m"])
            pdr = len(unicos) / total * 100
            faltantes = sorted(set(range(1, total + 1)) - set(unicos))
            resultado.append(dict(zip(COLUNAS, [
                info["ponto"], dist, info["obstrucao"], runid, total,
                len(unicos), len(dados) - len(unicos), crc if len(runs) == 1 else "",
                round(pdr, 2), round(100 - pdr, 2), *stats(rssi), *stats(snr),
                ";".join(map(str, faltantes)), caminho.name])))
        if len(runs) != 1:
            print(f"ATENCAO: {caminho.name} contem {len(runs)} series; "
                  "eventos CRC nao podem ser atribuidos a uma serie especifica.")
        if not runs:
            resultado.append(dict(zip(COLUNAS, [
                info["ponto"], float(info["distancia_m"]), info["obstrucao"],
                "SEM_RX", total, 0, 0, crc, 0, 100, "", "", "", "", "", "", "", "",
                ";".join(map(str, range(1, total + 1))), caminho.name])))
    return resultado

def graficos(rows, pasta):
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib nao instalado; CSV resumo gerado sem graficos.")
        return
    if not rows:
        return
    plots = [("rssi_medio_dbm", "RSSI medio (dBm)", "rssi_distancia.png"),
             ("snr_medio_db", "SNR medio (dB)", "snr_distancia.png"),
             ("pdr_pct", "PDR (%)", "pdr_distancia.png")]
    for field, ylab, filename in plots:
        fig, ax = plt.subplots(figsize=(8.4, 4.8))
        usable = [r for r in rows if r[field] != ""]
        for i, rec in enumerate(sorted(usable, key=lambda r: float(r["distancia_m"]))):
            x = float(rec["distancia_m"]); y = float(rec[field]);
            ax.scatter(x, y)
            ax.annotate(rec["ponto"], (x, y), xytext=(5, 5), textcoords="offset points")
        ax.set_xlabel("Distancia horizontal (m)")
        ax.set_ylabel(ylab)
        ax.set_title(ylab + " x distancia (NLOS urbano)")
        ax.grid(True, alpha=0.25)
        fig.tight_layout()
        fig.savefig(pasta / filename, dpi=180)
        plt.close(fig)

def main():
    arquivos = sorted(PASTA_ENTRADA.glob("eventos_*.csv"))
    if not arquivos:
        print(f"Nao ha arquivos eventos_*.csv em: {PASTA_ENTRADA}")
        return
    PASTA_SAIDA.mkdir(parents=True, exist_ok=True)
    rows = analisar(arquivos, TOTAL_PACOTES)
    resumo = PASTA_SAIDA / "resumo_lora.csv"
    with resumo.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=COLUNAS)
        writer.writeheader()
        writer.writerows(rows)
    for r in rows:
        print(f"{r['ponto']} | {r['distancia_m']:g} m | run={r['run_id']} "
              f"PDR={r['pdr_pct']}% ({r['n_recebidos_unicos']}/{r['n_esperado']}) "
              f"RSSI={r['rssi_medio_dbm']} dBm | SNR={r['snr_medio_db']} dB")
    graficos(rows, PASTA_SAIDA)
    print(f"Resumo salvo: {resumo}")

if __name__ == "__main__":
    main()
