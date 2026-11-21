"""Seleção de acesso e inicialização dos servidores de prévia dos painéis."""

from __future__ import annotations

import argparse
import socket
import sys
from http.server import ThreadingHTTPServer


def adicionar_opcoes_acesso(parser: argparse.ArgumentParser, porta_padrao: int) -> None:
    parser.add_argument("--porta", type=int, default=porta_padrao,
                        help="porta HTTP (padrão %(default)s)")
    modo = parser.add_mutually_exclusive_group()
    modo.add_argument("--local", dest="rede", action="store_const", const=False,
                      help="iniciar com acesso somente neste PC")
    modo.add_argument("--rede", action="store_const", const=True,
                      help="iniciar com acesso neste PC e na rede local")


def selecionar_endereco(rede: bool | None) -> str:
    if rede is not None or not sys.stdin.isatty():
        return "0.0.0.0" if rede else "127.0.0.1"

    print("\nOnde você quer abrir a prévia?")
    print("  1) Somente neste PC (padrão)")
    print("  2) Neste PC e em dispositivos na mesma rede")
    while True:
        try:
            escolha = input("Escolha [1/2] (Enter para 1): ").strip()
        except EOFError:
            return "127.0.0.1"
        except KeyboardInterrupt:
            print("\nPrévia cancelada.")
            raise SystemExit from None
        if escolha in ("", "1"):
            return "127.0.0.1"
        if escolha == "2":
            return "0.0.0.0"
        print("Digite 1 ou 2.")


def _enderecos_ipv4() -> list[str]:
    try:
        _, _, enderecos = socket.gethostbyname_ex(socket.gethostname())
    except OSError:
        return []
    return sorted({ip for ip in enderecos if ip != "0.0.0.0" and not ip.startswith("127.")})


def servir_previa(servidor: ThreadingHTTPServer) -> None:
    endereco, porta = servidor.server_address
    print(f"No PC, abra http://127.0.0.1:{porta}/", flush=True)
    if endereco == "0.0.0.0":
        enderecos = _enderecos_ipv4()
        if enderecos:
            print("No celular, conectado à mesma rede, abra o endereço do PC nessa rede.")
            for ip in enderecos:
                print(f"  http://{ip}:{porta}/")
        else:
            print(f"Na mesma rede, abra http://<IPv4-do-PC>:{porta}/. "
                  "Use ipconfig no PC para consultar o IPv4 da conexão.")
    print("Prévia simulada. Ctrl+C encerra.", flush=True)
    try:
        servidor.serve_forever()
    except KeyboardInterrupt:
        print("\nPrévia encerrada.")
