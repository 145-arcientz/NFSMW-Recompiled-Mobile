#!/usr/bin/env python3
"""Iconos de la app de iOS a partir del de Android (compilar.sh lo llama al empaquetar).

    python3 iconos.py <ic_launcher.png> <carpeta de nfsmw.app>

Saca los PNG que nombra CFBundleIconFiles en Info.plist (AppIcon60x60@2x.png, ...) reduciendo el
ic_launcher de mayor densidad de Android (192x192). Sin Pillow (no esta en WSL ni en Windows): lee y
escribe el PNG con zlib y reduce con un filtro de caja (la media del area que cubre cada pixel).
Sin Mac no hay actool para compilar un Assets.car, asi que van sueltos en el bundle, como los de Theos.
iOS redondea las esquinas el solo; la transparencia, si la hay, se aplana sobre negro (iOS la pintaria
negra igualmente).
"""

import struct
import sys
import zlib
from pathlib import Path

# Nombre en el bundle -> lado en pixeles. Los de ~ipad solo los mira el iPad.
ICONOS = {
    "AppIcon60x60@2x.png": 120,
    "AppIcon60x60@3x.png": 180,
    "AppIcon40x40@2x.png": 80,
    "AppIcon40x40@3x.png": 120,
    "AppIcon29x29@2x.png": 58,
    "AppIcon29x29@3x.png": 87,
    "AppIcon76x76@2x~ipad.png": 152,
    "AppIcon83.5x83.5@2x~ipad.png": 167,
}


def leer_png(ruta):
    """RGB o RGBA de 8 bits sin entrelazar -> (ancho, alto, filas de [r, g, b] en float)."""
    datos = Path(ruta).read_bytes()
    if datos[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"{ruta}: no es un PNG")
    pos, idat, cabecera = 8, b"", None
    while pos < len(datos):
        largo = struct.unpack(">I", datos[pos:pos + 4])[0]
        tipo = datos[pos + 4:pos + 8]
        cuerpo = datos[pos + 8:pos + 8 + largo]
        if tipo == b"IHDR":
            cabecera = struct.unpack(">IIBBBBB", cuerpo)
        elif tipo == b"IDAT":
            idat += cuerpo
        pos += 12 + largo
    ancho, alto, bits, color, _, _, entrelazado = cabecera
    if bits != 8 or color not in (2, 6) or entrelazado:
        sys.exit(f"{ruta}: solo RGB/RGBA de 8 bits sin entrelazar (es {bits} bits, tipo {color})")
    bpp = 4 if color == 6 else 3
    crudo = zlib.decompress(idat)
    paso = ancho * bpp
    anterior = bytearray(paso)
    filas = []
    for y in range(alto):
        inicio = y * (paso + 1)
        filtro = crudo[inicio]
        fila = bytearray(crudo[inicio + 1:inicio + 1 + paso])
        for i in range(paso):
            a = fila[i - bpp] if i >= bpp else 0
            b = anterior[i]
            c = anterior[i - bpp] if i >= bpp else 0
            if filtro == 1:
                fila[i] = (fila[i] + a) & 0xFF
            elif filtro == 2:
                fila[i] = (fila[i] + b) & 0xFF
            elif filtro == 3:
                fila[i] = (fila[i] + ((a + b) >> 1)) & 0xFF
            elif filtro == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                fila[i] = (fila[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        anterior = fila
        pixeles = []
        for x in range(ancho):
            r, g, b = fila[x * bpp], fila[x * bpp + 1], fila[x * bpp + 2]
            alfa = fila[x * bpp + 3] / 255.0 if bpp == 4 else 1.0
            pixeles.append([r * alfa, g * alfa, b * alfa])  # sobre negro
        filas.append(pixeles)
    return ancho, alto, filas


def pesos(origen, destino):
    """Para cada pixel de destino, los (pixel de origen, peso) del area que cubre."""
    escala = origen / destino
    tabla = []
    for d in range(destino):
        x0, x1 = d * escala, (d + 1) * escala
        lista = []
        for s in range(int(x0), min(int(x1 - 1e-9) + 1, origen)):
            cubierto = min(x1, s + 1) - max(x0, s)
            if cubierto > 0:
                lista.append((s, cubierto / escala))
        tabla.append(lista)
    return tabla


def reducir(ancho, alto, filas, lado):
    ph, pv = pesos(ancho, lado), pesos(alto, lado)
    horizontal = [[[sum(fila[s][c] * w for s, w in ph[x]) for c in range(3)] for x in range(lado)]
                  for fila in filas]
    salida = bytearray()
    for y in range(lado):
        salida.append(0)  # sin filtro
        for x in range(lado):
            for c in range(3):
                v = sum(horizontal[s][x][c] * w for s, w in pv[y])
                salida.append(max(0, min(255, int(v + 0.5))))
    return bytes(salida)


def escribir_png(ruta, lado, crudo):
    def trozo(tipo, cuerpo):
        return (struct.pack(">I", len(cuerpo)) + tipo + cuerpo +
                struct.pack(">I", zlib.crc32(tipo + cuerpo) & 0xFFFFFFFF))
    Path(ruta).write_bytes(b"\x89PNG\r\n\x1a\n" +
                           trozo(b"IHDR", struct.pack(">IIBBBBB", lado, lado, 8, 2, 0, 0, 0)) +
                           trozo(b"IDAT", zlib.compress(crudo, 9)) + trozo(b"IEND", b""))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    ancho, alto, filas = leer_png(sys.argv[1])
    if ancho != alto:
        sys.exit(f"{sys.argv[1]}: el icono no es cuadrado ({ancho}x{alto})")
    app = Path(sys.argv[2])
    for nombre, lado in ICONOS.items():
        if lado > ancho:
            print(f"Aviso: {nombre} ({lado}) sale de un icono de {ancho}: se amplia", file=sys.stderr)
        escribir_png(app / nombre, lado, reducir(ancho, alto, filas, lado))
    print(f"{app}: {len(ICONOS)} iconos desde {sys.argv[1]} ({ancho}x{alto})")


if __name__ == "__main__":
    main()
