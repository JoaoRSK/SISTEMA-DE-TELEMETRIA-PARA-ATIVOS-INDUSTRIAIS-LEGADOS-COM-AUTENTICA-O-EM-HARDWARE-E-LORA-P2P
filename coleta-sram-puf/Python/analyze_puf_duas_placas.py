"""Caracterizacao SRAM-PUF de dois ESP32: intra-dispositivo e inter-dispositivo.

Execute pelo Run Python File do VS Code. Necessita numpy e matplotlib.
Mantem saidas individuais do analyze_puf_extended.py e acrescenta comparacoes.
"""
from pathlib import Path
import csv
import sqlite3
import re

import numpy as np
import matplotlib.pyplot as plt

# CONFIGURACAO: diretorios ao lado deste arquivo, independentes do cwd do VS Code.
BASE = Path(__file__).resolve().parent
DIRS = {
    "PUF_1": BASE / "puf_samples_1",
    "PUF_2": BASE / "puf_samples_2",
}
OUT = BASE / "puf_analysis"
N_SAMPLES = 100
SRAM_SIZE = 8192
PUF_BITS = 128
ENROLLMENT = 80
MOSTRAR_RESPOSTA_HEX = False  # Evite exportar o segredo bruto usado para derivar chave.

TOTAL_BITS = SRAM_SIZE * 8
POPCOUNT = np.array([int(i).bit_count() for i in range(256)], dtype=np.uint8)


def write_csv(path, columns, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(columns)
        w.writerows(data)


def metrics(values):
    a = np.asarray(values, dtype=np.float64)
    return {
        "media": float(np.mean(a)),
        "desvio": float(np.std(a, ddof=1)) if a.size > 1 else 0.0,
        "mediana": float(np.median(a)),
        "minimo": float(np.min(a)),
        "maximo": float(np.max(a)),
    }


def choose_indices(candidates):
    if candidates.size < PUF_BITS:
        return None

    return candidates[np.linspace(0, len(candidates) - 1, PUF_BITS).round().astype(int)]


def load_device(name, folder):
    if not folder.is_dir():
        raise FileNotFoundError(f"{name}: pasta nao encontrada: {folder}")
    files = list(folder.glob("sample_*.bin"))
    def sample_number(p):
        m = re.fullmatch(r"sample_(\d+)\.bin", p.name)
        if not m:
            raise ValueError(f"Nome de amostra invalido: {p.name}")
        return int(m.group(1))
    files.sort(key=sample_number)
    if len(files) != N_SAMPLES:
        raise RuntimeError(f"{name}: esperadas {N_SAMPLES} amostras, encontradas {len(files)} em {folder}")
    numbers = [sample_number(p) for p in files]
    if len(set(numbers)) != len(numbers):
        raise RuntimeError(f"{name}: indices de amostra repetidos nos nomes de arquivo")
    invalid = [(p.name, p.stat().st_size) for p in files if p.stat().st_size != SRAM_SIZE]
    if invalid:
        raise RuntimeError(f"{name}: arquivos com tamanho diferente de {SRAM_SIZE} bytes: {invalid[:5]}")
    raw = np.stack([np.frombuffer(p.read_bytes(), dtype=np.uint8) for p in files])
    bits = np.unpackbits(raw, axis=1, bitorder="little")
    return {"nome": name, "pasta": folder, "raw": raw, "bits": bits, "files": files, "numbers": numbers}


def compute_intra(d):
    name, bits, raw = d["nome"], d["bits"], d["raw"]
    n = len(bits)
    out = OUT / name
    out.mkdir(parents=True, exist_ok=True)
    ones = bits.sum(axis=0, dtype=np.uint32)
    zeros = n - ones
    dominant = (ones >= zeros).astype(np.uint8)
    stability = np.maximum(ones, zeros) / n * 100.0
    stable = np.flatnonzero(stability == 100)
    transitions = np.count_nonzero(bits[1:] != bits[:-1], axis=0)
    selected = choose_indices(stable)
    if selected is None:
        raise RuntimeError(f"{name}: somente {len(stable)} bits estaveis; insuficientes para {PUF_BITS} bits")
    selected_values = bits[0, selected]
    final_hex = np.packbits(selected_values, bitorder="little").tobytes().hex().upper()
    hw = bits.sum(axis=1) / TOTAL_BITS * 100.0
    # O primeiro valor de HD de referencia e 0 por definicao; nao incluir na media.
    hd_ref = POPCOUNT[np.bitwise_xor(raw, raw[0])].sum(axis=1).astype(np.uint32)
    hd_prev = np.zeros(n, dtype=np.uint32)
    hd_prev[1:] = POPCOUNT[np.bitwise_xor(raw[1:], raw[:-1])].sum(axis=1)
    pairwise = []
    pairwise_values = []
    for i in range(n):
        hd = POPCOUNT[np.bitwise_xor(raw[i+1:], raw[i])].sum(axis=1)
        for j_offset, h in enumerate(hd):
            h = int(h)
            pairwise.append((i+1, i+j_offset+2, h, h / TOTAL_BITS * 100))
            pairwise_values.append(h / TOTAL_BITS * 100)
    pairwise_values = np.asarray(pairwise_values)

    train = bits[:ENROLLMENT]
    val = bits[ENROLLMENT:]
    train_stable = np.flatnonzero(np.all(train == train[0], axis=0))
    selected_train = choose_indices(train_stable)
    if selected_train is None:
        raise RuntimeError(f"{name}: enrollment com menos de {PUF_BITS} candidatos estaveis")
    expected = train[0, selected_train]
    errors_by_bit = np.count_nonzero(val[:, selected_train] != expected, axis=0)
    val_errors = np.count_nonzero(val[:, selected_train] != expected, axis=1)
    val_matches = val_errors == 0
    val_ber = int(val_errors.sum()) / (len(val) * PUF_BITS) * 100

    write_csv(out / "all_bits.csv",
              ["bit_index", "byte_index", "bit_in_byte", "ones", "zeros", "dominant_value", "stability_pct", "transition_count", "stable_100"],
              ((i, i//8, i%8, int(ones[i]), int(zeros[i]), int(dominant[i]), float(stability[i]), int(transitions[i]), int(stability[i] == 100)) for i in range(TOTAL_BITS)))
    write_csv(out / "stable_bits.csv", ["bit_index", "byte_index", "bit_in_byte", "value", "ones", "zeros", "stability_pct"],
              ((int(i), int(i//8), int(i%8), int(bits[0,i]), int(ones[i]), int(zeros[i]), float(stability[i])) for i in stable))
    write_csv(out / "selected_bits.csv", ["puf_bit", "sram_bit_index", "byte_index", "bit_in_byte", "value"],
              ((k, int(i), int(i//8), int(i%8), int(bits[0,i])) for k, i in enumerate(selected)))
    write_csv(out / "sample_metrics.csv",
              ["sample", "hamming_weight_bits", "hamming_weight_pct", "hd_vs_reference_bits", "hd_vs_reference_pct", "hd_vs_previous_bits", "hd_vs_previous_pct"],
              ((i+1, int(round(hw[i] * TOTAL_BITS / 100)), float(hw[i]), int(hd_ref[i]), float(hd_ref[i] / TOTAL_BITS * 100), int(hd_prev[i]), float(hd_prev[i] / TOTAL_BITS * 100)) for i in range(n)))
    write_csv(out / "pairwise_hamming.csv", ["sample_a", "sample_b", "hamming_distance_bits", "hamming_distance_pct"], pairwise)
    thresholds = [100, 99, 98, 95, 90, 80, 70, 60, 50]
    write_csv(out / "stability_thresholds.csv", ["minimum_stability_pct", "bit_count", "bit_percent"],
              ((t, int(np.count_nonzero(stability >= t)), float(np.mean(stability >= t) * 100)) for t in thresholds))
    write_csv(out / "validation_80_20.csv", ["sample", "bit_errors_128", "hamming_distance_pct", "exact_match"],
              ((ENROLLMENT+i+1, int(err), float(err / PUF_BITS * 100), int(err == 0)) for i, err in enumerate(val_errors)))
    write_csv(out / "validation_selected_bits.csv", ["puf_bit", "sram_bit_index", "expected_value", "validation_flip_count", "validation_flip_pct"],
              ((k, int(idx), int(expected[k]), int(errors_by_bit[k]), float(errors_by_bit[k] / len(val) * 100)) for k, idx in enumerate(selected_train)))

    header = ["#ifndef PUF_INDICES_H", "#define PUF_INDICES_H", "", "#include <stdint.h>", f"#define PUF_NUM_BITS {PUF_BITS}",
              "", "static const uint16_t puf_bit_index[PUF_NUM_BITS] = {"]
    for i in range(0, PUF_BITS, 8):
        header.append("    " + ", ".join(str(int(v)) for v in selected[i:i+8]) + ("," if i + 8 < PUF_BITS else ""))
    header += ["};", "", "#endif", ""]
    (out / "puf_indices.h").write_text("\n".join(header), encoding="utf-8")

    hwd = metrics(hw)
    hd_intra = metrics(pairwise_values)
    hd_reference = metrics(hd_ref[1:] / TOTAL_BITS * 100)
    hd_consecutive = metrics(hd_prev[1:] / TOTAL_BITS * 100)
    summary = (
        f"{name} - CARACTERIZACAO SRAM-PUF\n"
        f"Amostras: {n} | SRAM: {SRAM_SIZE} bytes | Bits: {TOTAL_BITS}\n"
        f"Bits estaveis {n}/{n}: {len(stable)} ({len(stable)/TOTAL_BITS*100:.4f}%)\n"
        f"Hamming Weight SRAM: media={hwd['media']:.4f}% | desvio={hwd['desvio']:.4f}%\n"
        f"HD intra (todas as {len(pairwise_values)} duplas): media={hd_intra['media']:.4f}% | desvio={hd_intra['desvio']:.4f}%"
        f" | min={hd_intra['minimo']:.4f}% | max={hd_intra['maximo']:.4f}%\n"
        f"HD para amostra 1 (sem o zero da propria referencia): media={hd_reference['media']:.4f}%\n"
        f"HD consecutiva: media={hd_consecutive['media']:.4f}%\n"
        f"PUF selecionada: {PUF_BITS} bits | HW={int(selected_values.sum())}/{PUF_BITS} ({selected_values.mean()*100:.4f}%)\n"
        f"Validacao 80/20: {int(val_matches.sum())}/{len(val)} exatas ({val_matches.mean()*100:.2f}%)"
        f" | BER={val_ber:.6f}% | erros={int(val_errors.sum())}/{len(val)*PUF_BITS}\n"
        "NOTA: a selecao final (100 amostras) NAO e a mesma selecao testada no 80/20.\n"
    )
    if MOSTRAR_RESPOSTA_HEX:
        summary += f"AVISO - resposta PUF sensivel: {final_hex}\n"
    (out / "summary.txt").write_text(summary, encoding="utf-8")

    conn = sqlite3.connect(out / "puf_analysis.sqlite")
    try:
        cur = conn.cursor()
        cur.executescript("""
            DROP TABLE IF EXISTS bit_metrics; DROP TABLE IF EXISTS stable_bits;
            DROP TABLE IF EXISTS selected_bits; DROP TABLE IF EXISTS sample_metrics;
            DROP TABLE IF EXISTS pairwise_hamming; DROP TABLE IF EXISTS validation_samples;
            DROP TABLE IF EXISTS validation_bits; DROP TABLE IF EXISTS stability_thresholds;
            CREATE TABLE bit_metrics(bit_index INTEGER PRIMARY KEY, byte_index INTEGER, bit_in_byte INTEGER, ones INTEGER, zeros INTEGER, dominant_value INTEGER, stability_pct REAL, transition_count INTEGER, stable_100 INTEGER);
            CREATE TABLE stable_bits(bit_index INTEGER PRIMARY KEY, byte_index INTEGER, bit_in_byte INTEGER, value INTEGER, stability_pct REAL);
            CREATE TABLE selected_bits(puf_bit INTEGER PRIMARY KEY, sram_bit_index INTEGER, byte_index INTEGER, bit_in_byte INTEGER, value INTEGER);
            CREATE TABLE sample_metrics(sample INTEGER PRIMARY KEY, hamming_weight_bits INTEGER, hamming_weight_pct REAL, hd_vs_reference_bits INTEGER, hd_vs_reference_pct REAL, hd_vs_previous_bits INTEGER, hd_vs_previous_pct REAL);
            CREATE TABLE pairwise_hamming(sample_a INTEGER, sample_b INTEGER, hamming_distance_bits INTEGER, hamming_distance_pct REAL);
            CREATE TABLE validation_samples(sample INTEGER PRIMARY KEY, bit_errors_128 INTEGER, hamming_distance_pct REAL, exact_match INTEGER);
            CREATE TABLE validation_bits(puf_bit INTEGER PRIMARY KEY, sram_bit_index INTEGER, expected_value INTEGER, validation_flip_count INTEGER, validation_flip_pct REAL);
            CREATE TABLE stability_thresholds(minimum_stability_pct REAL PRIMARY KEY, bit_count INTEGER, bit_percent REAL);
        """)
        cur.executemany('INSERT INTO bit_metrics VALUES (?,?,?,?,?,?,?,?,?)',
            ((i,i//8,i%8,int(ones[i]),int(zeros[i]),int(dominant[i]),float(stability[i]),int(transitions[i]),int(stability[i] == 100)) for i in range(TOTAL_BITS)))
        cur.executemany('INSERT INTO stable_bits VALUES (?,?,?,?,?)',
            ((int(i),int(i//8),int(i%8),int(bits[0,i]),float(stability[i])) for i in stable))
        cur.executemany('INSERT INTO selected_bits VALUES (?,?,?,?,?)',
            ((k,int(i),int(i//8),int(i%8),int(bits[0,i])) for k, i in enumerate(selected)))
        cur.executemany('INSERT INTO sample_metrics VALUES (?,?,?,?,?,?,?)',
            ((i+1,int(round(hw[i]*TOTAL_BITS/100)),float(hw[i]),int(hd_ref[i]),float(hd_ref[i]/TOTAL_BITS*100),int(hd_prev[i]),float(hd_prev[i]/TOTAL_BITS*100)) for i in range(n)))
        cur.executemany('INSERT INTO pairwise_hamming VALUES (?,?,?,?)', pairwise)
        cur.executemany('INSERT INTO validation_samples VALUES (?,?,?,?)',
            ((ENROLLMENT+i+1,int(err),float(err/PUF_BITS*100),int(err == 0)) for i,err in enumerate(val_errors)))
        cur.executemany('INSERT INTO validation_bits VALUES (?,?,?,?,?)',
            ((k,int(idx),int(expected[k]),int(errors_by_bit[k]),float(errors_by_bit[k]/len(val)*100)) for k,idx in enumerate(selected_train)))
        cur.executemany('INSERT INTO stability_thresholds VALUES (?,?,?)',
            ((t,int(np.count_nonzero(stability >= t)),float(np.mean(stability >= t)*100)) for t in thresholds))
        conn.commit()
    finally:
        conn.close()

    x = np.arange(1, n+1)
    def line_plot(y, ylabel, title, filename, x_values=None):
        fig, ax = plt.subplots(figsize=(9, 4.8))
        ax.plot(x if x_values is None else x_values, y, marker="o", markersize=2.8)
        ax.set(xlabel="Amostra", ylabel=ylabel, title=f"{name} - {title}")
        ax.grid(alpha=.3)
        fig.tight_layout(); fig.savefig(out / filename, dpi=180); plt.close(fig)

    line_plot(hw, "Hamming Weight (%)", "Hamming Weight da RTC SLOW", "hamming_weight_samples.png")
    line_plot(hd_ref / TOTAL_BITS * 100, "Hamming Distance (%)", "HD para a primeira amostra", "hamming_distance_reference.png")
    line_plot(hd_prev[1:] / TOTAL_BITS * 100, "Hamming Distance (%)", "HD entre amostras consecutivas",
              "hamming_distance_consecutive.png", x_values=x[1:])
    fig, ax = plt.subplots(figsize=(8, 4.8))
    ax.hist(pairwise_values, bins=30)
    ax.set(xlabel="Hamming Distance (%)", ylabel="Numero de pares", title=f"{name} - distribuicao HD intra")
    fig.tight_layout(); fig.savefig(out / "pairwise_hamming_histogram.png", dpi=180); plt.close(fig)
    fig, ax = plt.subplots(figsize=(8, 4.8))
    ax.hist(stability, bins=np.arange(49.5, 101.5, 1))
    ax.set(xlabel="Estabilidade do bit (%)", ylabel="Quantidade de bits", title=f"{name} - estabilidade por bit")
    fig.tight_layout(); fig.savefig(out / "bit_stability_histogram.png", dpi=180); plt.close(fig)
    fig, ax = plt.subplots(figsize=(7, 7))
    ax.imshow(bits[0].reshape(256, 256), interpolation="nearest", cmap="gray", vmin=0, vmax=1)
    ax.set(title=f"{name} - RTC SLOW - amostra 001"); ax.axis("off")
    fig.tight_layout(); fig.savefig(out / "sram_sample_001.png", dpi=180); plt.close(fig)
    print(summary)
    d.update({"ones": ones, "dominant": dominant, "stability": stability, "stable": stable,
              "selected": selected, "hw": hw, "hd_ref_pct": hd_ref/TOTAL_BITS*100,
              "hd_intra": pairwise_values, "summary": summary,
              "results": {"dispositivo": name, "amostras": n, "bits_estaveis": len(stable),
                          "estabilidade_pct": len(stable)/TOTAL_BITS*100, "hw_medio_pct": hwd["media"],
                          "hd_intra_medio_pct": hd_intra["media"], "hd_intra_desvio_pct": hd_intra["desvio"],
                          "reconstrucao_exata_pct": val_matches.mean()*100, "ber_80_20_pct": val_ber}})
    return d


def compute_inter(a, b):
    out = OUT / "inter_dispositivo"
    out.mkdir(parents=True, exist_ok=True)
    r1, r2 = a["raw"], b["raw"]
    n1, n2 = len(r1), len(r2)
    # HD entre TODAS as 100 x 100 amostras (10.000 comparacoes com os MESMOS 65.536 indices).
    hd = np.empty((n1, n2), dtype=np.uint32)
    for first in range(0, n1, 5):
        last = min(first + 5, n1)
        xor = np.bitwise_xor(r1[first:last, None, :], r2[None, :, :])
        hd[first:last] = POPCOUNT[xor].sum(axis=2)
    hd_pct = hd.astype(np.float64) / TOTAL_BITS * 100
    write_csv(out / "hd_inter_65536.csv",
        ["sample_puf1", "sample_puf2", "hamming_distance_bits", "hamming_distance_pct"],
        ((i+1, j+1, int(hd[i,j]), float(hd_pct[i,j])) for i in range(n1) for j in range(n2)))
    stats = metrics(hd_pct)

    dominant_hamming_bits = int(np.count_nonzero(a["dominant"] != b["dominant"]))
    common_stable = np.intersect1d(a["stable"], b["stable"], assume_unique=True)
    common_indices = choose_indices(common_stable)
    common_128_hd = None
    if common_indices is not None:
        bits1 = a["bits"][0, common_indices]
        bits2 = b["bits"][0, common_indices]
        common_128_hd = int(np.count_nonzero(bits1 != bits2))
        write_csv(out / "indices_comuns_128.csv",
            ["puf_bit", "sram_bit_index", "valor_puf1", "valor_puf2", "diferente"],
            ((k, int(idx), int(bits1[k]), int(bits2[k]), int(bits1[k]!=bits2[k])) for k,idx in enumerate(common_indices)))
    else:
        # Falta de candidatos nao pode ser disfarçada com comparacao de indices diferentes.
        write_csv(out / "indices_comuns_128.csv",
                  ["puf_bit", "sram_bit_index", "valor_puf1", "valor_puf2", "diferente"], [])

    cross_rows = []
    cross_metrics = {}
    for reference, target in ((a,b), (b,a)):
        selected = reference["selected"]
        reference_puf = reference["bits"][0, selected]
        opposite_data = target["bits"][:, selected]
        errors = np.count_nonzero(opposite_data != reference_puf, axis=1)
        cross_metrics[reference["nome"]] = metrics(errors / PUF_BITS * 100)
        for i, diff in enumerate(errors):
            cross_rows.append((reference["nome"], target["nome"], i+1,
                               int(diff), float(diff / PUF_BITS * 100)))
    write_csv(out / "hd_indices_puf1_puf2.csv",
        ["referencia", "avaliado", "amostra_avaliada", "erros_128", "hd_128_pct"], cross_rows)

    summary = (
        "COMPARACAO INTER-DISPOSITIVO - SRAM-PUF\n"
        f"HD inter em 65.536 posicoes, {n1} x {n2} = {n1*n2} pares\n"
        f"Media={stats['media']:.4f}% | DP={stats['desvio']:.4f}% | mediana={stats['mediana']:.4f}%\n"
        f"Minimo={stats['minimo']:.4f}% | Maximo={stats['maximo']:.4f}%\n"
        f"HD entre respostas majoritarias: {dominant_hamming_bits}/{TOTAL_BITS} "
        f"({dominant_hamming_bits/TOTAL_BITS*100:.4f}%)\n"
        f"Bits observados 100% estaveis nas duas placas (mesmos indices): {len(common_stable)}\n"
    )
    if common_128_hd is not None:
        summary += f"HD em 128 indices COMUNS estaveis: {common_128_hd}/{PUF_BITS} ({common_128_hd/PUF_BITS*100:.4f}%)\n"
    else:
        summary += "HD em 128 indices COMUNS estaveis: indisponivel (menos de 128 indices comuns)\n"
    for ref in (a,b):
        v = cross_metrics[ref["nome"]]
        summary += (f"Indices selecionados na {ref['nome']} aplicados na OUTRA placa: "
                    f"HD medio={v['media']:.4f}% | DP={v['desvio']:.4f}%\n")
    summary += (
        "NOTAS: 1) comparar duas respostas de 128 bits feitas com indices DIFERENTES nao mede unicidade.\n"
        "2) HD inter nos 65.536 indices e HD nos indices comuns representam experimentos distintos.\n"
        "3) 10.000 pares sao combinacoes de apenas duas placas (nao 10.000 dispositivos independentes).\n"
        "4) valores estatisticos proximos de 50% nao comprovam impossibilidade de clonagem nem entropia criptografica.\n"
    )
    (out / "summary.txt").write_text(summary, encoding="utf-8")

    with sqlite3.connect(out / "puf_inter_analysis.sqlite") as conn:
        conn.executescript("""
            DROP TABLE IF EXISTS hd_inter; DROP TABLE IF EXISTS hd_128_indices;
            DROP TABLE IF EXISTS hd_indices_comuns;
            CREATE TABLE hd_inter(sample_puf1 INTEGER, sample_puf2 INTEGER, hd_bits INTEGER, hd_pct REAL);
            CREATE TABLE hd_128_indices(referencia TEXT, avaliado TEXT, amostra INTEGER, erros_128 INTEGER, hd_pct REAL);
            CREATE TABLE hd_indices_comuns(puf_bit INTEGER, sram_bit_index INTEGER, valor_puf1 INTEGER, valor_puf2 INTEGER, diferente INTEGER);
        """)
        conn.executemany("INSERT INTO hd_inter VALUES (?,?,?,?)",
                         ((i+1,j+1,int(hd[i,j]),float(hd_pct[i,j])) for i in range(n1) for j in range(n2)))
        conn.executemany("INSERT INTO hd_128_indices VALUES (?,?,?,?,?)", cross_rows)
        if common_indices is not None:
            conn.executemany("INSERT INTO hd_indices_comuns VALUES (?,?,?,?,?)",
                ((k,int(idx),int(a['bits'][0,idx]),int(b['bits'][0,idx]),
                  int(a['bits'][0,idx]!=b['bits'][0,idx])) for k,idx in enumerate(common_indices)))

    fig, ax = plt.subplots(figsize=(9,5))
    ax.hist(a["hd_intra"], bins=32, alpha=.65, label="Intra PUF 1", density=True)
    ax.hist(b["hd_intra"], bins=32, alpha=.65, label="Intra PUF 2", density=True)
    ax.hist(hd_pct.ravel(), bins=32, alpha=.65, label="Inter PUF 1 x PUF 2", density=True)
    ax.set(xlabel="Hamming Distance (%)", ylabel="Densidade", title="HD intra e inter-dispositivo (65.536 bits)")
    ax.legend(); fig.tight_layout(); fig.savefig(out / "hamming_intra_vs_inter.png", dpi=180); plt.close(fig)

    fig, ax = plt.subplots(figsize=(8,5))
    ax.boxplot([a["hd_intra"], b["hd_intra"], hd_pct.ravel()])
    ax.set_xticks([1, 2, 3], ["Intra 1", "Intra 2", "Inter"])
    ax.set(ylabel="Hamming Distance (%)", title="Comparacao intra e inter-dispositivo")
    ax.grid(axis="y", alpha=.3)
    fig.tight_layout(); fig.savefig(out / "boxplot_hd_intra_inter.png", dpi=180); plt.close(fig)

    fig, ax = plt.subplots(figsize=(7, 5.5))
    hm = ax.imshow(hd_pct, aspect="auto", interpolation="nearest")
    ax.set(xlabel="Amostras PUF 2", ylabel="Amostras PUF 1", title="Mapa de HD inter-dispositivo (%)")
    fig.colorbar(hm, ax=ax, label="HD (%)")
    fig.tight_layout(); fig.savefig(out / "heatmap_hd_inter_65536.png", dpi=180); plt.close(fig)

    fig, ax = plt.subplots(figsize=(8, 4.8))
    ax.plot(np.arange(1, n1+1), a["hw"], label="PUF 1")
    ax.plot(np.arange(1, n2+1), b["hw"], label="PUF 2")
    ax.set(xlabel="Amostra", ylabel="Hamming Weight (%)", title="Hamming Weight das duas placas")
    ax.grid(alpha=.3); ax.legend()
    fig.tight_layout(); fig.savefig(out / "hamming_weight_comparacao.png", dpi=180); plt.close(fig)

    fig, ax = plt.subplots(figsize=(8, 4.8))
    ax.hist(a["stability"], bins=np.arange(49.5,101.5,1), alpha=.55, label="PUF 1")
    ax.hist(b["stability"], bins=np.arange(49.5,101.5,1), alpha=.55, label="PUF 2")
    ax.set(xlabel="Estabilidade por posicao (%)", ylabel="Quantidade de bits", title="Estabilidade dos bits: PUF 1 e PUF 2")
    ax.legend(); fig.tight_layout(); fig.savefig(out / "estabilidade_comparacao.png", dpi=180); plt.close(fig)

    if common_indices is not None:
        diff = (a["bits"][0, common_indices] != b["bits"][0, common_indices]).astype(int)
        fig, ax = plt.subplots(figsize=(8, 2.8))
        ax.step(np.arange(1, PUF_BITS+1), diff, where="mid")
        ax.set(xlabel="Posicao comum de 128 bits", ylabel="Diferenca (0 ou 1)",
               title=f"Comparacao em indices comuns - HD={common_128_hd}/{PUF_BITS}", ylim=(-.1, 1.1))
        fig.tight_layout(); fig.savefig(out / "hd_128_indices_comuns.png", dpi=180); plt.close(fig)

    write_csv(OUT / "resumo_comparativo.csv",
        ["dispositivo", "amostras", "bits_estaveis", "estabilidade_pct", "hw_medio_pct", "hd_intra_medio_pct",
         "hd_intra_desvio_pct", "reconstrucao_exata_pct", "ber_80_20_pct"],
        ([d["results"][k] for k in ("dispositivo", "amostras", "bits_estaveis", "estabilidade_pct", "hw_medio_pct",
                                "hd_intra_medio_pct", "hd_intra_desvio_pct", "reconstrucao_exata_pct", "ber_80_20_pct")]
         for d in (a,b)))
    (OUT / "resumo_geral.txt").write_text(a["summary"]+"\n"+b["summary"]+"\n"+summary, encoding="utf-8")
    print(summary)


def main():
    if ENROLLMENT <= 0 or ENROLLMENT >= N_SAMPLES:
        raise ValueError("ENROLLMENT precisa estar entre 1 e N_SAMPLES-1")
    OUT.mkdir(parents=True, exist_ok=True)
    devices = [load_device(name, folder) for name, folder in DIRS.items()]
    devices = [compute_intra(d) for d in devices]
    compute_inter(*devices)
    print(f"\nAnalise finalizada. Resultados em: {OUT}")
    print("ATENCAO: selected_bits.csv e os BIN podem conter material criptografico sensivel.")


if __name__ == "__main__":
    main()
