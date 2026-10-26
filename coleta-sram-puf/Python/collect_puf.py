import socket
import struct
import csv
from pathlib import Path
from datetime import datetime

PORT = 5000
TOTAL = 100
SIZE = 8192
OUT = Path("puf_samples")
CSV = OUT / "metadata.csv"

def recv_exact(conn, n):
    data = bytearray()
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            raise ConnectionError("Conexao encerrada antes do fim.")
        data.extend(chunk)
    return bytes(data)

def next_sample():
    nums = []
    for f in OUT.glob("sample_*.bin"):
        try:
            nums.append(int(f.stem.split("_")[1]))
        except ValueError:
            pass
    return max(nums, default=0) + 1

def save_meta(num, ip):
    new = not CSV.exists()
    with CSV.open("a", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        if new:
            w.writerow(["sample", "timestamp", "client_ip", "size_bytes"])
        w.writerow([num, datetime.now().isoformat(), ip, SIZE])

def main():
    OUT.mkdir(exist_ok=True)
    num = next_sample()

    if num > TOTAL:
        print("Ja existem 100 amostras.")
        return

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(("0.0.0.0", PORT))
        server.listen(1)
        print(f"Servidor TCP porta {PORT}. Proxima amostra: {num:03d}")

        while num <= TOTAL:
            try:
                conn, addr = server.accept()

                with conn:
                    header = recv_exact(conn, 8)

                    if header[:4] != b"PUF1":
                        raise ValueError("Cabecalho invalido.")

                    size = struct.unpack("!I", header[4:])[0]

                    if size != SIZE:
                        raise ValueError(f"Tamanho invalido: {size}")

                    data = recv_exact(conn, SIZE)

                    file = OUT / f"sample_{num:03d}.bin"
                    file.write_bytes(data)
                    save_meta(num, addr[0])

                    print(f"Amostra {num:03d}/100 salva: {file}")

                    if num < TOTAL:
                        print("Desligue completamente por pelo menos 5 s e ligue novamente.")

                    num += 1

            except Exception as e:
                print(f"Erro: {e}. Amostra nao contabilizada.")

    print("100 amostras concluidas.")

if __name__ == "__main__":
    main()
