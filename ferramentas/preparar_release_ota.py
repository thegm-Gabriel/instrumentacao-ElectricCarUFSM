#!/usr/bin/env python3
"""Versiona, compila incrementalmente e prepara o release OTA dos três ESPs."""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path


REPOSITORIO_PADRAO = "thegm-Gabriel/instrumentacao-ElectricCarUFSM"
MAGIA_DESCRICAO_APLICATIVO = (0xABCD5432).to_bytes(4, "little")
VERSAO_INICIAL = "0.1.0"
ARQUIVOS_IGNORADOS = {"sdkconfig.old"}
PASTAS_IGNORADAS = {".pio", ".git", "__pycache__"}
EXTENSOES_FIRMWARE = {".c", ".cc", ".cpp", ".h", ".hpp", ".ini", ".csv", ".cmake"}
EXTENSOES_WEB = {".html", ".css", ".js", ".py"}

PROJETOS = {
    "mestre-s3": {
        "pasta": "mestre-s3",
        "ambiente": "mestre-s3-n16r8",
        "projeto": "esp32s3-MESTRE",
        "binario": "mestre-s3.bin",
    },
    "equipe-s3": {
        "pasta": "equipe-s3",
        "ambiente": "esp32-s3-devkitm-1",
        "projeto": "esp32s3_equipe",
        "binario": "equipe-s3.bin",
    },
    "visitantes-s3": {
        "pasta": "visitantes-s3",
        "ambiente": "esp32-s3-devkitm-1",
        "projeto": "esp32s3_visitantes",
        "binario": "visitantes-s3.bin",
    },
}


def validar_versao(texto: str) -> str:
    versao = texto.removeprefix("v")
    if not re.fullmatch(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", versao):
        raise ValueError(f"versão inválida: {texto}")
    return versao


def incrementar_versao(versao: str, incremento: str) -> str:
    maior, menor, correcao = map(int, validar_versao(versao).split("."))
    if incremento == "major":
        return f"{maior + 1}.0.0"
    if incremento == "minor":
        return f"{maior}.{menor + 1}.0"
    if incremento == "patch":
        return f"{maior}.{menor}.{correcao + 1}"
    raise ValueError(f"incremento inválido: {incremento}")


def escolher_incremento(ler=input, escrever=print) -> str | None:
    """Pede a escolha no terminal; None cancela antes de alterar arquivos."""
    escrever("\nQual tipo de release deseja preparar?")
    escrever("  1) Patch — correções e refinamentos compatíveis")
    escrever("  2) Minor — nova funcionalidade compatível")
    escrever("  3) Major — mudança incompatível")
    escrever("  0) Cancelar")
    opcoes = {"1": "patch", "2": "minor", "3": "major",
              "patch": "patch", "minor": "minor", "major": "major"}
    while True:
        try:
            resposta = ler("Escolha [1/2/3/0]: ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            escrever("\nPreparação cancelada.")
            return None
        if resposta in opcoes:
            incremento = opcoes[resposta]
            escrever(f"Incremento selecionado: {incremento}.")
            return incremento
        if resposta in ("0", "sair", "cancelar"):
            escrever("Preparação cancelada.")
            return None
        escrever("Opção inválida. Digite 1, 2, 3 ou 0.")


def calcular_novas_versoes(estado: dict, alterados: set[str],
                          incremento: str) -> tuple[str, dict[str, str]]:
    # Valida o tipo mesmo quando não houve alteração, antes de preparar arquivos.
    if incremento not in ("major", "minor", "patch"):
        raise ValueError(f"incremento inválido: {incremento}")
    release = (incrementar_versao(estado["versao_release"], incremento)
               if alterados else estado["versao_release"])
    firmwares = {
        alvo: (incrementar_versao(estado["firmwares"][alvo]["versao"], incremento)
               if alvo in alterados else estado["firmwares"][alvo]["versao"])
        for alvo in PROJETOS
    }
    return release, firmwares


def localizar_platformio() -> Path:
    executavel = shutil.which("pio") or shutil.which("platformio")
    if executavel:
        return Path(executavel)
    nomes = ("pio.exe", "platformio.exe") if os.name == "nt" else ("pio", "platformio")
    pasta = Path.home() / ".platformio" / "penv" / (
        "Scripts" if os.name == "nt" else "bin"
    )
    for nome in nomes:
        candidato = pasta / nome
        if candidato.is_file():
            return candidato
    raise FileNotFoundError("PlatformIO não encontrado no PATH nem na instalação do usuário")


def ler_texto_campo(dados: bytes) -> str:
    return dados.split(b"\0", 1)[0].decode("utf-8", errors="strict")


def obter_versao_interna(imagem: Path, nome_projeto: str) -> str:
    conteudo = imagem.read_bytes()
    inicio = 0
    while True:
        posicao = conteudo.find(MAGIA_DESCRICAO_APLICATIVO, inicio)
        if posicao < 0:
            break
        if posicao + 80 <= len(conteudo):
            try:
                versao = ler_texto_campo(conteudo[posicao + 16 : posicao + 48])
                projeto = ler_texto_campo(conteudo[posicao + 48 : posicao + 80])
            except UnicodeDecodeError:
                versao = ""
                projeto = ""
            if projeto == nome_projeto and versao:
                return versao
        inicio = posicao + 1
    raise ValueError(f"não foi possível encontrar a versão interna em {imagem}")


def arquivos_para_impressao_digital(pastas: list[Path]) -> list[Path]:
    arquivos: list[Path] = []
    for pasta in pastas:
        for caminho in pasta.rglob("*"):
            if not caminho.is_file() or caminho.name in ARQUIVOS_IGNORADOS:
                continue
            if any(parte in PASTAS_IGNORADAS for parte in caminho.parts):
                continue
            if (caminho.suffix.lower() not in EXTENSOES_FIRMWARE and
                    caminho.name != "CMakeLists.txt" and
                    not caminho.name.startswith("sdkconfig")):
                continue
            arquivos.append(caminho)
    return sorted(set(arquivos), key=lambda item: item.as_posix().lower())


def calcular_impressao_digital(raiz: Path, projeto: Path) -> str:
    resumo = hashlib.sha256()
    compartilhado = raiz / "firmware" / "compartilhado"
    arquivos = arquivos_para_impressao_digital([projeto, compartilhado])
    if projeto.name in ("equipe-s3", "visitantes-s3"):
        arquivos.extend(
            arquivo for arquivo in (projeto / "web").rglob("*")
            if arquivo.is_file() and arquivo.suffix.lower() in EXTENSOES_WEB
        )
        arquivos.append(compartilhado / "web" / "logo_carro_eletrico.png")
    for arquivo in sorted(set(arquivos), key=lambda item: item.as_posix().lower()):
        relativo = arquivo.relative_to(raiz).as_posix().encode("utf-8")
        resumo.update(len(relativo).to_bytes(4, "little"))
        resumo.update(relativo)
        conteudo = arquivo.read_bytes()
        resumo.update(len(conteudo).to_bytes(8, "little"))
        resumo.update(conteudo)
    return resumo.hexdigest()


def executar(comando: list[str], raiz: Path, ambiente: dict[str, str]) -> None:
    print("\nExecutando:", subprocess.list2cmdline(comando), flush=True)
    subprocess.run(comando, cwd=raiz, env=ambiente, check=True)


def carregar_json(caminho: Path) -> dict:
    if not caminho.is_file():
        return {}
    try:
        return json.loads(caminho.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return {}


def estado_inicial(raiz: Path) -> dict:
    estado = {
        "versao_formato": 1,
        "versao_release": VERSAO_INICIAL,
        "firmwares": {
            alvo: {"versao": VERSAO_INICIAL, "impressao_digital": ""}
            for alvo in PROJETOS
        },
    }
    manifesto_antigo = carregar_json(
        raiz / "versoes" / "saida" / "manifesto-mestre.json"
    )
    if manifesto_antigo.get("versao_formato") == 1:
        versao = manifesto_antigo.get("versao", VERSAO_INICIAL)
        try:
            estado["versao_release"] = validar_versao(versao)
            estado["firmwares"]["mestre-s3"]["versao"] = validar_versao(versao)
        except ValueError:
            pass
    return estado


def carregar_estado(raiz: Path) -> dict:
    caminho = raiz / "versoes" / "estado_versoes.json"
    recebido = carregar_json(caminho)
    if recebido.get("versao_formato") != 1 or not isinstance(
        recebido.get("firmwares"), dict
    ):
        return estado_inicial(raiz)
    for alvo in PROJETOS:
        item = recebido["firmwares"].get(alvo)
        if not isinstance(item, dict):
            return estado_inicial(raiz)
        validar_versao(str(item.get("versao", "")))
    validar_versao(str(recebido.get("versao_release", "")))
    return recebido


def arquivar_saida_anterior(diretorio_saida: Path, diretorio_historico: Path,
                            versao_release: str) -> None:
    arquivos = list(diretorio_saida.iterdir()) if diretorio_saida.exists() else []
    if not arquivos:
        return
    destino = diretorio_historico / f"v{versao_release}"
    if destino.exists():
        destino = diretorio_historico / (
            f"v{versao_release}-{datetime.now().strftime('%Y%m%d-%H%M%S')}"
        )
    destino.mkdir(parents=True, exist_ok=False)
    for arquivo in arquivos:
        shutil.move(str(arquivo), destino / arquivo.name)
    print(f"Release anterior arquivado em {destino}")


def preparar_release(repositorio: str, forcar: set[str],
                    incremento: str = "patch") -> tuple[Path, str]:
    if incremento not in ("major", "minor", "patch"):
        raise ValueError(f"incremento inválido: {incremento}")
    raiz = Path(__file__).resolve().parents[1]
    estado = carregar_estado(raiz)
    diretorio_versoes = raiz / "versoes"
    diretorio_saida = diretorio_versoes / "saida"
    diretorio_temporario = diretorio_versoes / ".preparacao"
    diretorio_historico = diretorio_versoes / "historico"
    gerador = raiz / "ferramentas" / "criar_manifesto_ota.py"

    # O .c incorporado é parte do firmware: atualize-o antes de calcular os
    # hashes, mesmo quando não haverá incremento de versão.
    ambiente_geracao = os.environ.copy()
    for alvo in ("equipe-s3", "visitantes-s3"):
        executar(
            [sys.executable, str(raiz / "firmware" / alvo / "web" / "gerar_painel.py")],
            raiz,
            ambiente_geracao,
        )

    alterados: set[str] = set(forcar)
    for alvo, dados in PROJETOS.items():
        projeto = raiz / "firmware" / dados["pasta"]
        impressao = calcular_impressao_digital(raiz, projeto)
        anterior = estado["firmwares"][alvo].get("impressao_digital", "")
        if not anterior or anterior != impressao:
            alterados.add(alvo)

    arquivos_esperados = [
        diretorio_saida / dados["binario"] for dados in PROJETOS.values()
    ] + [diretorio_saida / "manifesto-firmwares.json"]
    saida_incompleta = not all(arquivo.is_file() for arquivo in arquivos_esperados)
    if not saida_incompleta:
        for alvo, dados in PROJETOS.items():
            try:
                versao_interna = obter_versao_interna(
                    diretorio_saida / dados["binario"], dados["projeto"]
                )
            except (OSError, UnicodeDecodeError, ValueError):
                saida_incompleta = True
                break
            if versao_interna != estado["firmwares"][alvo]["versao"]:
                saida_incompleta = True
                break
    if not alterados and not saida_incompleta:
        print("Nenhuma fonte mudou desde o último release. Use --forcar para versionar mesmo assim.")
        return diretorio_saida, estado["versao_release"]

    versao_release_anterior = estado["versao_release"]
    nova_versao_release, novas_versoes = calcular_novas_versoes(
        estado, alterados, incremento
    )

    alvos_para_compilar = {
        alvo for alvo, dados in PROJETOS.items()
        if alvo in alterados or not (diretorio_saida / dados["binario"]).is_file()
    }
    for alvo, dados in PROJETOS.items():
        if alvo in alvos_para_compilar:
            continue
        imagem_anterior = diretorio_saida / dados["binario"]
        try:
            versao_interna = obter_versao_interna(imagem_anterior, dados["projeto"])
        except (OSError, UnicodeDecodeError, ValueError):
            alvos_para_compilar.add(alvo)
            continue
        if versao_interna != novas_versoes[alvo]:
            print(
                f"{alvo}: binário anterior contém a versão {versao_interna}; "
                f"será recompilado como {novas_versoes[alvo]}"
            )
            alvos_para_compilar.add(alvo)
    platformio = localizar_platformio() if alvos_para_compilar else None

    if diretorio_temporario.exists():
        shutil.rmtree(diretorio_temporario)
    diretorio_temporario.mkdir(parents=True)
    ambiente_base = os.environ.copy()

    for alvo, dados in PROJETOS.items():
        projeto = raiz / "firmware" / dados["pasta"]
        imagem_compilada = (
            projeto / ".pio" / "build" / dados["ambiente"] / "firmware.bin"
        )
        destino = diretorio_temporario / dados["binario"]
        origem_anterior = diretorio_saida / dados["binario"]
        precisa_compilar = alvo in alvos_para_compilar
        if precisa_compilar:
            if platformio is None:
                raise FileNotFoundError("PlatformIO não encontrado")
            ambiente = ambiente_base.copy()
            ambiente["VERSAO_FIRMWARE"] = novas_versoes[alvo]
            # Força somente a configuração do CMake; os objetos não são apagados.
            (projeto / "CMakeLists.txt").touch()
            executar(
                [str(platformio), "run", "-d", str(projeto), "-e", dados["ambiente"]],
                raiz,
                ambiente,
            )
            if not imagem_compilada.is_file():
                raise FileNotFoundError(f"binário não gerado: {imagem_compilada}")
            versao_interna = obter_versao_interna(imagem_compilada, dados["projeto"])
            if versao_interna != novas_versoes[alvo]:
                raise ValueError(
                    f"{alvo} foi compilado como {versao_interna}; "
                    f"esperado {novas_versoes[alvo]}"
                )
            shutil.copy2(imagem_compilada, destino)
        else:
            shutil.copy2(origem_anterior, destino)

    etiqueta = f"v{nova_versao_release}"
    manifesto = diretorio_temporario / "manifesto-firmwares.json"
    executar(
        [
            sys.executable,
            str(gerador),
            "--firmware-mestre", str(diretorio_temporario / "mestre-s3.bin"),
            "--firmware-equipe", str(diretorio_temporario / "equipe-s3.bin"),
            "--firmware-visitantes", str(diretorio_temporario / "visitantes-s3.bin"),
            "--versao-mestre", novas_versoes["mestre-s3"],
            "--versao-equipe", novas_versoes["equipe-s3"],
            "--versao-visitantes", novas_versoes["visitantes-s3"],
            "--etiqueta", etiqueta,
            "--repositorio", repositorio,
            "--saida", str(manifesto),
        ],
        raiz,
        ambiente_base,
    )

    diretorio_saida.mkdir(parents=True, exist_ok=True)
    diretorio_historico.mkdir(parents=True, exist_ok=True)
    arquivar_saida_anterior(
        diretorio_saida, diretorio_historico, versao_release_anterior
    )
    for arquivo in diretorio_temporario.iterdir():
        shutil.move(str(arquivo), diretorio_saida / arquivo.name)
    diretorio_temporario.rmdir()

    estado["versao_release"] = nova_versao_release
    for alvo, dados in PROJETOS.items():
        projeto = raiz / "firmware" / dados["pasta"]
        estado["firmwares"][alvo] = {
            "versao": novas_versoes[alvo],
            "impressao_digital": calcular_impressao_digital(raiz, projeto),
        }
    (diretorio_versoes / "estado_versoes.json").write_text(
        json.dumps(estado, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )

    acao = "preparado" if alterados else "reconstruído"
    print(f"\nRelease OTA {acao} com sucesso.")
    print(f"Etiqueta do GitHub Release: {etiqueta}")
    for alvo in PROJETOS:
        marcador = "atualizado" if alvo in alterados else "reutilizado"
        print(f"  {alvo}: {novas_versoes[alvo]} ({marcador})")
    print("Arquivos para anexar no GitHub:")
    for arquivo in sorted(diretorio_saida.iterdir()):
        print(f"  {arquivo}")
    print(f"Página de releases: https://github.com/{repositorio}/releases")
    return diretorio_saida, nova_versao_release


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Detecta alterações, incrementa versões independentes, compila "
            "incrementalmente e prepara um release OTA único"
        )
    )
    parser.add_argument(
        "--repositorio",
        default=REPOSITORIO_PADRAO,
        help=f"Proprietário/repositório (padrão: {REPOSITORIO_PADRAO})",
    )
    parser.add_argument(
        "--forcar",
        nargs="*",
        choices=[*PROJETOS, "todos"],
        default=[],
        help="Força incremento mesmo sem alteração detectada",
    )
    parser.add_argument(
        "--bump", "--incremento",
        choices=("major", "minor", "patch"),
        help="Parte SemVer a incrementar nos firmwares alterados e no release "
             "(dispensa o menu interativo)",
    )
    argumentos = parser.parse_args()
    try:
        if not re.fullmatch(r"[0-9A-Za-z_.-]+/[0-9A-Za-z_.-]+", argumentos.repositorio):
            raise ValueError("repositório inválido; use proprietário/repositório")
        incremento = argumentos.bump
        if incremento is None:
            if not sys.stdin.isatty():
                raise ValueError("sem terminal interativo; informe --bump patch, minor ou major")
            incremento = escolher_incremento()
            if incremento is None:
                return 0
        forcar = set(PROJETOS) if "todos" in argumentos.forcar else set(argumentos.forcar)
        preparar_release(argumentos.repositorio, forcar, incremento)
        return 0
    except (FileNotFoundError, OSError, ValueError, subprocess.CalledProcessError) as erro:
        print(f"\nERRO: {erro}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
