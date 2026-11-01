"""Gera o recurso HTML embutido no firmware, sem dependências externas."""

import base64
from pathlib import Path


ROOT = Path(__file__).resolve().parent
TARGET = ROOT.parent / "src" / "servicos" / "painel_web.c"


def main() -> None:
    html = (ROOT / "index.html").read_text(encoding="utf-8")
    css = (ROOT / "style.css").read_text(encoding="utf-8")
    css += "\n" + (ROOT / "layout.css").read_text(encoding="utf-8")
    js = (ROOT / "app.js").read_text(encoding="utf-8")
    html = html.replace("/* NEXUS_CSS */", css).replace("/* NEXUS_JS */", js)
    logo = ROOT.parent.parent / "compartilhado" / "web" / "logo_carro_eletrico.png"
    html = html.replace("/* PROJECT_LOGO */", "data:image/png;base64," + base64.b64encode(logo.read_bytes()).decode("ascii"))
    lines = ["// Gerado por web/gerar_painel.py. Edite os arquivos em web/.",
             '#include "servicos/painel_web.h"', "", "const char PAGINA_WEB[] ="]
    for offset in range(0, len(html), 900):
        chunk = html[offset:offset + 900]
        escaped = chunk.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
        escaped = escaped.replace("??", "?\\?")
        lines.append(f'    "{escaped}"')
    lines[-1] += ";"
    TARGET.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Gerado {TARGET} ({len(html.encode('utf-8'))} bytes de HTML)")


if __name__ == "__main__":
    main()
