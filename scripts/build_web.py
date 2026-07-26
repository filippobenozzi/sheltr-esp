"""Comprime gli asset web e li incorpora nel firmware come header C.

L'interfaccia locale vive in `web/index.html` (file unico, senza dipendenze
esterne). Qui viene compressa con gzip e trasformata in un array PROGMEM, cosi'
il firmware serve la pagina direttamente dalla flash senza bisogno di caricare
un filesystem separato: un solo binario da flashare, anche da browser.
"""

import gzip
import os
from pathlib import Path

Import("env")  # noqa: F821  (fornito da PlatformIO)

PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))  # noqa: F821
WEB_DIR = PROJECT_DIR / "web"
OUT_FILE = PROJECT_DIR / "src" / "generated" / "web_assets.h"

ASSETS = [
    ("index.html", "WEB_INDEX"),
]


def to_c_array(name: str, data: bytes) -> str:
    lines = [f"static const uint8_t {name}_GZ[] PROGMEM = {{"]
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        lines.append("    " + ", ".join(f"0x{byte:02X}" for byte in chunk) + ",")
    lines.append("};")
    lines.append(f"static const size_t {name}_GZ_LEN = {len(data)};")
    return "\n".join(lines)


def build() -> None:
    OUT_FILE.parent.mkdir(parents=True, exist_ok=True)
    blocks = [
        "// File generato da scripts/build_web.py - non modificare a mano.",
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "",
    ]
    total_raw = 0
    total_gz = 0
    for filename, symbol in ASSETS:
        source = WEB_DIR / filename
        raw = source.read_bytes()
        packed = gzip.compress(raw, 9)
        total_raw += len(raw)
        total_gz += len(packed)
        blocks.append(to_c_array(symbol, packed))
        blocks.append("")
    OUT_FILE.write_text("\n".join(blocks), encoding="utf-8")
    print(
        f"[web] asset incorporati: {total_raw} byte -> {total_gz} byte gzip "
        f"({OUT_FILE.relative_to(PROJECT_DIR)})"
    )


def needs_rebuild() -> bool:
    if not OUT_FILE.exists():
        return True
    newest = max((WEB_DIR / name).stat().st_mtime for name, _ in ASSETS)
    return newest > OUT_FILE.stat().st_mtime or os.environ.get("SHELTR_FORCE_WEB") == "1"


if needs_rebuild():
    build()
else:
    print("[web] asset gia' aggiornati")
