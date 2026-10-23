#!/usr/bin/env python3
"""Los textos de la app de iOS, en sus 10 idiomas: los de la app de Android
(android/app/src/main/res/values*/strings.xml) y, encima, los propios de iOS
(ios/motor/textos/values*/strings.xml, las mismas carpetas y el mismo formato).

Sale un JSON, {"en": {"clave": "texto", ...}, "es": {...}, ...}, que compilar.sh mete
en la app y leen el lanzador y el mando tactil (textos.mm). "values" es el ingles, el
idioma por defecto: lo que falte en otro idioma sale en ingles.

Los formatos de Android pasan a los de NSString: %1$s es %1$@ (los %d se quedan).

    python3 textos.py <res de Android> <textos de iOS> <salida.json>
"""

import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def idioma(carpeta: Path) -> str:
    """values -> en, values-es -> es, values-zh-rTW -> zh-TW."""
    if carpeta.name == "values":
        return "en"
    codigo = carpeta.name[len("values-"):]
    return codigo.replace("-r", "-")


def texto(elemento: ET.Element) -> str:
    """El texto de un <string> como lo da getString() de Android."""
    s = "".join(elemento.itertext())
    # Lo que va entre comillas se queda tal cual; fuera de ellas, los espacios
    # seguidos (y los saltos de linea del XML) son uno.
    partes = re.split(r'(?<!\\)"', s)
    s = "".join(p if i % 2 else re.sub(r"\s+", " ", p) for i, p in enumerate(partes))
    if len(partes) == 1:
        s = s.strip()
    s = re.sub(r"\\u([0-9a-fA-F]{4})", lambda m: chr(int(m.group(1), 16)), s)
    s = re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t"}.get(m.group(1), m.group(1)), s)
    # %s y %1$s son %@ y %1$@ en NSString.
    return re.sub(r"%(\d+\$)?s", lambda m: "%" + (m.group(1) or "") + "@", s)


def leer(res: Path, textos: dict) -> None:
    for carpeta in sorted(res.glob("values*")):
        fichero = carpeta / "strings.xml"
        if not fichero.is_file():
            continue
        tabla = textos.setdefault(idioma(carpeta), {})
        for e in ET.parse(fichero).getroot().iter("string"):
            tabla[e.get("name")] = texto(e)


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    textos: dict = {}
    leer(Path(sys.argv[1]), textos)
    leer(Path(sys.argv[2]), textos)
    # Los que no se traducen (translatable="false") solo estan en ingles.
    base = textos.get("en", {})
    for codigo, tabla in textos.items():
        for clave, valor in base.items():
            tabla.setdefault(clave, valor)
    Path(sys.argv[3]).write_text(json.dumps(textos, ensure_ascii=False, sort_keys=True), encoding="utf-8")
    print(f"{sys.argv[3]}: {len(textos)} idiomas, {len(base)} textos")
    return 0


if __name__ == "__main__":
    sys.exit(main())
