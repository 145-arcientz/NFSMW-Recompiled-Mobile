#!/usr/bin/env python3
"""
Hace de "servidor de EA" en la LAN: escucha y apunta lo que manda el juego.

    python tools/red/escucha_ea.py --puertos 10600,10900-10910
    python tools/red/escucha_ea.py --puertos 1-1024 --udp

No contesta nada (todavia): sirve para ver a que puerto llama el juego y con que
protocolo, que es lo primero que hay que saber para imitar al servidor.

El juego va al PC porque el motor nativo, con --red_dns=<IP del PC>, contesta esa
IP a cualquier nombre que busque (ver docs/diario/red-y-privilegios.md). El
puerto sale del log del juego: la linea "[red] connect(..., IP:puerto)".

Lo recibido va a la consola y a out/red/escucha.log.

La primera vez, Windows pregunta si deja a Python recibir conexiones: hay que
decirle que si en redes privadas, o el movil no llega.
"""
import argparse
import asyncio
import datetime
import pathlib
import sys

RAIZ = pathlib.Path(__file__).resolve().parents[2]
LOG = RAIZ / "out" / "red" / "escucha.log"


def apuntar(texto):
    linea = f"{datetime.datetime.now():%H:%M:%S.%f}"[:-3] + " " + texto
    print(linea, flush=True)
    with LOG.open("a", encoding="utf-8") as f:
        f.write(linea + "\n")


def volcado(datos, ancho=32):
    """Hexadecimal y texto, en filas, como un editor hexadecimal."""
    filas = []
    for i in range(0, len(datos), ancho):
        trozo = datos[i:i + ancho]
        hexa = " ".join(f"{b:02X}" for b in trozo)
        txt = "".join(chr(b) if 32 <= b < 127 else "." for b in trozo)
        filas.append(f"    {i:04X}  {hexa:<{ancho * 3}} |{txt}|")
    return "\n".join(filas)


def puertos(texto):
    salida = []
    for parte in texto.split(","):
        parte = parte.strip()
        if not parte:
            continue
        if "-" in parte:
            a, b = parte.split("-", 1)
            salida.extend(range(int(a), int(b) + 1))
        else:
            salida.append(int(parte))
    return salida


async def cliente_tcp(lector, escritor, puerto):
    otro = escritor.get_extra_info("peername")
    apuntar(f"TCP {puerto}: conexion de {otro[0]}:{otro[1]}")
    total = 0
    try:
        while True:
            datos = await lector.read(65536)
            if not datos:
                break
            total += len(datos)
            apuntar(f"TCP {puerto} <- {otro[0]}:{otro[1]} {len(datos)} bytes\n{volcado(datos)}")
    except ConnectionError as e:
        apuntar(f"TCP {puerto}: {e}")
    apuntar(f"TCP {puerto}: {otro[0]}:{otro[1]} cerro ({total} bytes en total)")
    escritor.close()


class Udp(asyncio.DatagramProtocol):
    def __init__(self, puerto):
        self.puerto = puerto

    def datagram_received(self, datos, otro):
        apuntar(f"UDP {self.puerto} <- {otro[0]}:{otro[1]} {len(datos)} bytes\n{volcado(datos)}")


async def principal(lista, con_udp, ip):
    bucle = asyncio.get_running_loop()
    abiertos, fallidos = [], []
    for p in lista:
        try:
            await asyncio.start_server(lambda r, w, p=p: cliente_tcp(r, w, p), ip, p)
            abiertos.append(f"tcp/{p}")
        except OSError:
            fallidos.append(f"tcp/{p}")
        if con_udp:
            try:
                await bucle.create_datagram_endpoint(lambda p=p: Udp(p), local_addr=(ip, p))
                abiertos.append(f"udp/{p}")
            except OSError:
                fallidos.append(f"udp/{p}")
    resumen = ", ".join(abiertos[:12]) + (" ..." if len(abiertos) > 12 else "")
    apuntar(f"escuchando en {len(abiertos)} puertos ({resumen})")
    if fallidos:
        apuntar(f"no se pudieron abrir {len(fallidos)} (ocupados o reservados): "
                + ", ".join(fallidos[:20]))
    await asyncio.Event().wait()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[1])
    ap.add_argument("--puertos", required=True, help="lista y rangos: 80,443,10000-10100")
    ap.add_argument("--udp", action="store_true", help="escuchar tambien UDP en los mismos puertos")
    ap.add_argument("--ip", default="0.0.0.0", help="interfaz (por defecto, todas)")
    a = ap.parse_args()
    LOG.parent.mkdir(parents=True, exist_ok=True)
    apuntar("=" * 60)
    try:
        asyncio.run(principal(puertos(a.puertos), a.udp, a.ip))
    except KeyboardInterrupt:
        apuntar("parado")


if __name__ == "__main__":
    sys.exit(main())
