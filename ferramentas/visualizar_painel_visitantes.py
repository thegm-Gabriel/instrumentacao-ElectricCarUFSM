"""Prévia local do painel público com telemetria simulada."""

import argparse
import json
import math
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

WEB_DIR = Path(__file__).resolve().parents[1] / "firmware" / "visitantes-s3" / "web"
sys.path.insert(0, str(WEB_DIR))
from gerar_painel import montar_html


def telemetria_simulada(inicio: float) -> dict:
    segundos = time.monotonic() - inicio
    velocidade = max(0.0, 17 + 13 * math.sin(segundos / 9) + 3 * math.sin(segundos / 2.8))
    aceleracao = (13 / 9 * math.cos(segundos / 9) + 3 / 2.8 * math.cos(segundos / 2.8)) / 3.6
    potencia = round(550 + velocidade * 75 + aceleracao * 900)
    carga = max(0, 82 - segundos / 900)
    return {
        "valido": True,
        "idade_ms": 0,
        "sequencia": int(segundos * 2) + 1,
        "simulado": True,
        "velocidade_centesimos_kmh": round(velocidade * 100),
        "acelerador_decimos_percentual": round(min(100, max(0, 20 + aceleracao * 100)) * 10) if aceleracao >= -0.1 else 0,
        "freio_decimos_percentual": round(min(100, max(0, -aceleracao * 130)) * 10) if aceleracao < -0.1 else 0,
        "aceleracao_disponivel": True,
        "aceleracao_milesimos_ms2": round(aceleracao * 1000),
        "tensao_pacote_mv": 48500,
        "corrente_centesimos_a": round(potencia / 48.5 * 100),
        "carga_decimos_percentual": round(carga * 10),
        "autonomia_decimos_km": round(carga * 0.62 * 10),
        "potencia_w": potencia,
        "percurso_metros": round(1200 + segundos * 4.7),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--porta", type=int, default=8765, help="porta local (padrão: 8765)")
    parser.add_argument("--rede", action="store_true", help="permitir acesso de outros dispositivos na rede local")
    args = parser.parse_args()
    inicio = time.monotonic()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:
            if self.path in ("/", "/index.html"):
                body = montar_html().encode("utf-8")
                content_type = "text/html; charset=utf-8"
            elif self.path == "/api/telemetria":
                body = json.dumps(telemetria_simulada(inicio)).encode("utf-8")
                content_type = "application/json; charset=utf-8"
            elif self.path == "/favicon.ico":
                self.send_response(204)
                self.end_headers()
                return
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

    endereco = "0.0.0.0" if args.rede else "127.0.0.1"
    with ThreadingHTTPServer((endereco, args.porta), Handler) as servidor:
        print(f"No PC: http://127.0.0.1:{args.porta}/", flush=True)
        if args.rede:
            print(f"No celular: http://<IP_IPV4_DO_PC>:{args.porta}/ (mesma rede Wi-Fi)", flush=True)
        print("Ctrl+C encerra.", flush=True)
        try:
            servidor.serve_forever()
        except KeyboardInterrupt:
            print("\nPrévia encerrada.")


if __name__ == "__main__":
    main()
