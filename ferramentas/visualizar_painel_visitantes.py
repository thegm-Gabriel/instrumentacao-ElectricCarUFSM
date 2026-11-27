"""Prévia local do painel público com telemetria simulada."""

import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

if __package__:
    from .previas.servidor import (
        adicionar_opcoes_acesso, carregar_gerador, selecionar_endereco, servir_previa,
    )
    from .previas.telemetria import telemetria_simulada
else:
    from previas.servidor import (
        adicionar_opcoes_acesso, carregar_gerador, selecionar_endereco, servir_previa,
    )
    from previas.telemetria import telemetria_simulada

montar_html = carregar_gerador("visitantes-s3").montar_html


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    adicionar_opcoes_acesso(parser, 8765)
    args = parser.parse_args()
    endereco = selecionar_endereco(args.rede)
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

    with ThreadingHTTPServer((endereco, args.porta), Handler) as servidor:
        servir_previa(servidor)


if __name__ == "__main__":
    main()
