#!/usr/bin/env python3
"""Gera o cabeçalho local de Wi-Fi a partir de variáveis de ambiente."""

import json
import os
from pathlib import Path


def literal_c(valor: str) -> str:
    return json.dumps(valor, ensure_ascii=True)


ssid = os.environ.get("OTA_WIFI_SSID", "")
senha = os.environ.get("OTA_WIFI_SENHA", "")
if not ssid or not senha:
    raise SystemExit("defina OTA_WIFI_SSID e OTA_WIFI_SENHA")
if len(ssid.encode("utf-8")) > 32 or len(senha) > 63:
    raise SystemExit("SSID ou senha excede o limite do Wi-Fi")

destino = Path("firmware/mestre-s3/include/nucleo/segredos_ota.h")
destino.write_text(
    "#pragma once\n\n"
    f"#define OTA_WIFI_SSID {literal_c(ssid)}\n"
    f"#define OTA_WIFI_SENHA {literal_c(senha)}\n",
    encoding="utf-8",
)
print(f"Configuração OTA criada em {destino}")
