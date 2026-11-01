"""Gera os QR codes da rede e do painel público dos visitantes."""

from pathlib import Path
import re

import qrcode
from qrcode.image.svg import SvgPathImage


ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "firmware/visitantes-s3/include/nucleo/configuracao.h"
OUTPUT = ROOT / "artefatos/qr_visitantes"


def macro(name: str) -> str:
    match = re.search(rf'^#define {name} "([^"]+)"', CONFIG.read_text(encoding="utf-8"), re.MULTILINE)
    if not match:
        raise ValueError(f"Macro {name} não encontrada em {CONFIG}")
    return match.group(1)


def wifi_escape(value: str) -> str:
    return re.sub(r"([\\;,:])", r"\\\1", value)


def save_qr(name: str, payload: str) -> None:
    qr = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_M, box_size=12, border=4)
    qr.add_data(payload)
    qr.make(fit=True)
    qr.make_image(fill_color="black", back_color="white").save(OUTPUT / f"{name}.png")
    qr.make_image(image_factory=SvgPathImage).save(OUTPUT / f"{name}.svg")


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    ssid, password, ip = macro("WIFI_SSID"), macro("WIFI_SENHA"), macro("IP_PAINEL")
    save_qr("wifi_visitantes", f"WIFI:T:WPA;S:{wifi_escape(ssid)};P:{wifi_escape(password)};;")
    save_qr("painel_visitantes", f"http://{ip}/")
    print(f"QR codes gerados em {OUTPUT}")


if __name__ == "__main__":
    main()
