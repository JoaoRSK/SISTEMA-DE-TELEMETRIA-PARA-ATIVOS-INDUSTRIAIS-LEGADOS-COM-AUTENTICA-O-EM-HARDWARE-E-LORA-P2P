"""Coleta simultanea TX/RX e elabora tabela de verificacao sem inferir eventos ausentes.
Execute pelo botao Run Python File no VS Code; requer pip install pyserial.
"""
import csv
import queue
import re
import threading
import time
from datetime import datetime
from pathlib import Path

# === CONFIGURE AQUI ===
PORTA_TX = "COM8"
PORTA_RX = "COM3"
BAUDRATE = 115200
DURACAO_SEGUNDOS = 180  # Ajuste para ter tempo de reiniciar RX e TX entre as fases
PASTA_RESULTADOS = Path(__file__).resolve().parent / "resultados"
# ======================

ESPERADOS = {
    "E04_VALIDO": "ACCEPT",
    "E05_CIPHERTEXT": "GCM_FAIL",
    "E05_AAD": "GCM_FAIL",
    "E05_TAG": "GCM_FAIL",
    "E06_REPLAY_IDENTICO": "REPLAY_SEQ",
    "E07_SEQ_SUPERIOR": "ACCEPT",
    "E07_SEQ_INFERIOR": "REPLAY_SEQ",
    "E10_REPLAY_APOS_REBOOT_RX": "REPLAY_SEQ",
    "E09_NOVA_SESSAO_TX": "ACCEPT",
    "E08_SESSAO_ANTIGA": "REPLAY_SESSION",
}

COLUNAS_EVENTOS = ["horario", "timestamp", "dispositivo", "tipo", "teste", "sessao", "seq", "decisao", "linha_serial"]
COLUNAS_RESUMO = ["teste", "sessao", "seq", "esperado_rx", "resultado_rx", "tx_done", "situacao", "observacao"]


def ler_serial(nome, porta, fila, parar):
    try:
        import serial
    except ImportError:
        fila.put((time.time(), nome, f"ERRO: pyserial nao instalado no interpretador Python"))
        return
    while not parar.is_set():
        try:
            # Configure DTR/RTS antes de abrir para reduzir resets no ESP32.
            ser = serial.Serial(port=None, baudrate=BAUDRATE, timeout=0.2)
            ser.dtr = False
            ser.rts = False
            ser.port = porta
            ser.open()
            fila.put((time.time(), nome, f"[SERIAL_CONECTADO] {porta}"))
            with ser:
                while not parar.is_set():
                    data = ser.readline()
                    if data:
                        fila.put((time.time(), nome, data.decode("utf-8", errors="replace").strip()))
        except Exception as exc:
            fila.put((time.time(), nome, f"[SERIAL_DESCONECTADO] {porta}: {exc}"))
            parar.wait(2)


def classificar(nome, linha):
    linha = re.sub(r"\x1b\[[0-9;]*m", "", linha).strip()
    p = linha.split(",")
    info = {"tipo": "LOG", "teste": "", "sessao": "", "seq": "", "decisao": ""}
    if nome == "TX" and len(p) >= 3 and p[0] == "TEST_TX":
        info["tipo"] = p[1]
        if p[1] == "START" and len(p) == 5:
            info.update(teste=p[2], sessao=p[3], seq=p[4])
        elif p[1] == "END" and len(p) == 6:
            info.update(teste=p[2], decisao=p[3], sessao=p[4], seq=p[5])
        elif p[1] == "PHASE" and len(p) >= 4:
            info.update(teste="FASE_" + p[2], sessao=p[3])
        elif p[1] == "FINISH" and len(p) >= 4:
            info.update(teste="FIM_FASE_" + p[2], sessao=p[3])
        elif p[1] == "READY" and len(p) >= 3:
            info["sessao"] = p[2]
    elif nome == "RX" and len(p) == 4 and p[0] == "TEST_RX":
        info.update(tipo="RX_DECISAO", decisao=p[1], sessao=p[2], seq=p[3])
    elif "[SERIAL_DESCONECTADO]" in linha:
        info["tipo"] = "DESCONECTADO"
    elif "[SERIAL_CONECTADO]" in linha:
        info["tipo"] = "CONECTADO"
    return info


def resumir(eventos):
    starts = []
    fases = []
    restores = []
    for e in eventos:
        if e["dispositivo"] == "TX" and e["tipo"] == "PHASE":
            fases.append(e)
        if e["dispositivo"] == "RX" and e["decisao"] == "RESTORE":
            restores.append(e)
        if e["dispositivo"] == "TX" and e["tipo"] == "START":
            starts.append(e)

    # Associacao pela identificacao do frame e pela janela temporal; nao toma
    # as mensagens do transmissor como prova de recepcao no gateway.
    saida = []
    for i, ini in enumerate(starts):
        fim = starts[i + 1]["timestamp"] if i + 1 < len(starts) else float("inf")
        teste, sessao, seq = ini["teste"], ini["sessao"], ini["seq"]
        esperado = ESPERADOS.get(teste, "")
        candidatos = [e for e in eventos if e["dispositivo"] == "RX"
                      and e["tipo"] == "RX_DECISAO" and e["decisao"] != "RESTORE"
                      and e["sessao"] == sessao and e["seq"] == seq
                      and ini["timestamp"] - 0.05 <= e["timestamp"] < fim]
        decisoes = sorted(set(e["decisao"] for e in candidatos))
        end = [e for e in eventos if e["dispositivo"] == "TX" and e["tipo"] == "END"
               and e["teste"] == teste and e["sessao"] == sessao and e["seq"] == seq
               and ini["timestamp"] <= e["timestamp"] < fim]
        tx_done = "SIM" if any(e["decisao"] == "TX_DONE" for e in end) else "NAO"
        evidencia = "+".join(decisoes) if decisoes else "SEM_EVIDENCIA_RX"
        obs = ""
        status = "VALIDADO" if tx_done == "SIM" and decisoes == [esperado] else "NAO_COMPROVADO"
        if tx_done != "SIM":
            obs = "Sem confirmacao TxDone"
        elif len(decisoes) > 1:
            obs = "Decisoes RX divergentes na mesma janela"
        elif not decisoes:
            obs = "Nenhum log TEST_RX para essa etapa (ou pacote perdido)"
        elif decisoes != [esperado]:
            obs = "Decisao recebida diferente da esperada"
        if teste == "E10_REPLAY_APOS_REBOOT_RX":
            prev = [r for r in restores if r["timestamp"] < ini["timestamp"]
                    and r["sessao"] == sessao and r["seq"].isdigit() and seq.isdigit()
                    and int(r["seq"]) >= int(seq)]
            if not prev:
                status = "NAO_COMPROVADO"
                obs = "Falta registro RX RESTORE compatível antes do replay"
        if teste == "E09_NOVA_SESSAO_TX":
            anteriores = [e for e in fases if e["teste"] == "FASE_1" and e["timestamp"] < ini["timestamp"]]
            if not anteriores or not sessao.isdigit() or not anteriores[-1]["sessao"].isdigit() or int(sessao) <= int(anteriores[-1]["sessao"]):
                status = "NAO_COMPROVADO"
                obs = "Nao comprovado incremento de sessao entre as fases"
        saida.append(dict(zip(COLUNAS_RESUMO, [teste, sessao, seq, esperado, evidencia, tx_done, status, obs])))
    for nome in ESPERADOS:
        if not any(r["teste"] == nome for r in saida):
            saida.append(dict(zip(COLUNAS_RESUMO, [nome, "", "", ESPERADOS[nome], "NAO_EXECUTADO", "NAO", "NAO_COMPROVADO", "Etapa nao observada no TX"])))
    return saida


def salvar_resumo(eventos, pasta):
    resultado = resumir(sorted(eventos, key=lambda x: x["timestamp"]))
    with (pasta / "validacao_seguranca.csv").open("w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=COLUNAS_RESUMO, delimiter=";")
        w.writeheader()
        w.writerows(resultado)
    print("\n--- RESULTADOS EXPERIMENTAIS ---")
    for row in resultado:
        print(f"{row['teste']:30} esperado={row['esperado_rx']:17} recebido={row['resultado_rx']:18} {row['situacao']}")
    return resultado


def identificar_firmware(linha):
    """Identifica o firmware pela saida, independentemente da COM configurada."""
    if linha.startswith("TEST_RX,") or "TCC_RECEPTOR_LORA:" in linha:
        return "RX"
    if linha.startswith("TEST_TX,") or "TCC_TRANSMISSOR_LORA:" in linha:
        return "TX"
    return None


def main():
    pasta = PASTA_RESULTADOS / datetime.now().strftime("%Y%m%d_%H%M%S")
    pasta.mkdir(parents=True, exist_ok=True)
    print("Coleta TX + RX pelo VS Code. Inicie o script primeiro; reinicie o TX para iniciar a FASE 1.")
    print("Apos TEST_TX,FINISH,1: reinicie RX, espere o boot; reinicie TX para FASE 2.")
    print(f"Tempo de coleta: {DURACAO_SEGUNDOS}s | Pasta: {pasta}\n")
    fila, parar, eventos = queue.Queue(), threading.Event(), []
    # As COMs podem ter sido invertidas. O firmware prevalece sobre o rotulo inicial.
    papeis = {"TX": None, "RX": None}
    threads = [threading.Thread(target=ler_serial, args=(nome, porta, fila, parar), daemon=True)
               for nome, porta in (("TX", PORTA_TX), ("RX", PORTA_RX))]
    for th in threads:
        th.start()
    inicio = time.monotonic()
    with (pasta / "logs_completos.txt").open("w", encoding="utf-8") as raw, \
         (pasta / "eventos_seriais.csv").open("w", newline="", encoding="utf-8-sig") as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=COLUNAS_EVENTOS, delimiter=";")
        writer.writeheader()
        try:
            while time.monotonic() - inicio < DURACAO_SEGUNDOS:
                try:
                    instante, nome, linha = fila.get(timeout=0.25)
                except queue.Empty:
                    continue
                if not linha:
                    continue
                identificado = identificar_firmware(linha)
                if identificado and papeis[nome] != identificado:
                    anterior = papeis[nome]
                    papeis[nome] = identificado
                    outra = "RX" if nome == "TX" else "TX"
                    if papeis[outra] == identificado or papeis[outra] is None:
                        papeis[outra] = "TX" if identificado == "RX" else "RX"
                    aviso = (f"[IDENTIFICACAO] Porta inicialmente chamada {nome} "
                             f"contem firmware {identificado}" +
                             (f" (antes: {anterior})" if anterior else ""))
                    print(aviso)
                    raw.write(f"{aviso}\n")
                real = papeis[nome] or nome
                info = classificar(real, linha)
                data = datetime.fromtimestamp(instante).astimezone().isoformat(timespec="milliseconds")
                reg = {"horario": data, "timestamp": instante, "dispositivo": real,
                       "linha_serial": linha, **info}
                eventos.append(reg)
                raw.write(f"{data} [{real}] {linha}\n")
                writer.writerow(reg)
                # Flush garante evidencia, mesmo se o programa for encerrado a força.
                raw.flush()
                csvfile.flush()
                print(f"[{real}] {linha}")
        except KeyboardInterrupt:
            print("\nColeta interrompida pelo usuario; gerando resumo...")
        finally:
            parar.set()
    salvar_resumo(eventos, pasta)
    print(f"\nArquivos gravados em {pasta}")


if __name__ == "__main__":
    main()
