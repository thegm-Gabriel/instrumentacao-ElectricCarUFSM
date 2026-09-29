#!/usr/bin/env python3
"""Cria o manifesto OTA único para mestre, equipe e visitantes."""

import argparse
import hashlib
import json
import re
from pathlib import Path


def obter_sha256_imagem(conteudo: bytes) -> str:
    if len(conteudo) < 56 or conteudo[0] != 0xE9:
        raise ValueError("o arquivo não parece ser uma imagem ESP32 válida")

    possui_hash_anexado = conteudo[23] == 1
    if not possui_hash_anexado:
        return hashlib.sha256(conteudo).hexdigest()

    dados, hash_anexado = conteudo[:-32], conteudo[-32:]
    hash_calculado = hashlib.sha256(dados).digest()
    if hash_calculado != hash_anexado:
        raise ValueError("o SHA-256 anexado à imagem não confere")
    return hash_anexado.hex()


def main() -> None:
    parser = argparse.ArgumentParser(description="Cria o manifesto OTA dos três firmwares")
    parser.add_argument("--firmware", "--firmware-mestre", dest="firmware_mestre",
                        required=True, type=Path)
    parser.add_argument("--firmware-equipe", required=True, type=Path)
    parser.add_argument("--firmware-visitantes", required=True, type=Path)
    parser.add_argument("--versao", "--versao-mestre", dest="versao_mestre",
                        help="Versão do mestre")
    parser.add_argument("--versao-equipe", required=True)
    parser.add_argument("--versao-visitantes", required=True)
    parser.add_argument("--etiqueta", required=True, help="Etiqueta do GitHub Release, por exemplo v1.2.0")
    parser.add_argument("--repositorio", required=True, help="Proprietário/repositório")
    parser.add_argument("--saida", required=True, type=Path)
    argumentos = parser.parse_args()

    versao_mestre = argumentos.versao_mestre
    if not versao_mestre:
        raise ValueError("informe --versao-mestre")
    versoes = {
        "mestre-s3": versao_mestre.removeprefix("v"),
        "equipe-s3": argumentos.versao_equipe.removeprefix("v"),
        "visitantes-s3": argumentos.versao_visitantes.removeprefix("v"),
    }
    for alvo, versao in versoes.items():
        if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?", versao):
            raise ValueError(f"a versão de {alvo} deve seguir o formato 1.2.3")
    if not re.fullmatch(r"[0-9A-Za-z_.-]+/[0-9A-Za-z_.-]+", argumentos.repositorio):
        raise ValueError("repositório inválido; use proprietário/repositório")
    if not re.fullmatch(r"[0-9A-Za-z_.-]+", argumentos.etiqueta):
        raise ValueError("etiqueta de release inválida")

    imagens = {
        "mestre-s3": (argumentos.firmware_mestre, "mestre-s3.bin"),
        "equipe-s3": (argumentos.firmware_equipe, "equipe-s3.bin"),
        "visitantes-s3": (argumentos.firmware_visitantes, "visitantes-s3.bin"),
    }
    firmwares = {}
    for alvo, (caminho, nome_arquivo) in imagens.items():
        conteudo = caminho.read_bytes()
        firmwares[alvo] = {
            "versao": versoes[alvo],
            "url_firmware": (
                f"https://github.com/{argumentos.repositorio}/releases/download/"
                f"{argumentos.etiqueta}/{nome_arquivo}"
            ),
            "sha256": obter_sha256_imagem(conteudo),
            "tamanho_bytes": len(conteudo),
        }
    manifesto = {
        "versao_formato": 2,
        "release": argumentos.etiqueta,
        "firmwares": firmwares,
    }
    argumentos.saida.parent.mkdir(parents=True, exist_ok=True)
    argumentos.saida.write_text(
        json.dumps(manifesto, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(f"Manifesto criado em {argumentos.saida}")


if __name__ == "__main__":
    main()
