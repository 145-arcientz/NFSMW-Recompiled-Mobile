#!/usr/bin/env python3
"""El servidor de salas de iOS (servidor_ea.cpp) frente al de Python (ea_server.py de
nfsmw-server-lan), sin telefonos: dos jugadores de mentira hacen lo mismo contra los dos -entrar,
crear una sala y su partida, unirse, correr, mandar los resultados, mirar la clasificacion, salir y
borrarla- y se compara lo que recibe cada uno, mensaje a mensaje. Tambien la comprobacion del
gamertag y la busqueda de servidores.

Lo que mandan los jugadores sale de una partida LAN de verdad, la del registro del servidor de
Python (logs/server.log): sus campos (MADDR, SESS, RESU...) son los del juego. Los dos jugadores van
desde 127.0.0.5 ("User") y 127.0.0.4 ("victo"), como 10.0.0.5 y 10.0.0.4 en esa partida.

En WSL o en Linux, con el servidor de iOS compilado para Linux:

    clang++ -std=c++20 -O1 -I <SDK>/thirdparty/inja/third_party/include \\
        ios/motor/servidor_ea.cpp ios/motor/red_lan.cpp ios/motor/pruebas/prueba_servidor.cpp \\
        -o /tmp/prueba_servidor -lpthread
    python3 ios/motor/pruebas/prueba_servidor_ea.py <nfsmw-server-lan> /tmp/prueba_servidor

Los campos que cambian en cada arranque (SESS y MASK de @dir, LKEY, SEED, WHEN, REF, LGAME, el
nombre del servidor) no se comparan.
"""

import ast
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

DIRECTORIO, SALAS, APP = 30960, 10901, 10902
ESPERA = 0.06
A, B = "127.0.0.5", "127.0.0.4"
AL_AZAR = {"MASK", "LKEY", "SEED", "WHEN", "REF", "LGAME"}


def leer_log(ruta: Path, inicio: str) -> list[str]:
    """Las lineas de la tanda del registro que empieza a la hora `inicio`."""
    lineas = ruta.read_text(encoding="utf-8", errors="replace").splitlines()
    desde = next(i for i, l in enumerate(lineas) if l.startswith(inicio) and "=====" in l)
    hasta = next((i for i in range(desde + 1, len(lineas)) if "=====" in lineas[i]), len(lineas))
    return lineas[desde:hasta]


def campos(lineas: list[str], patron: str, n: int = 0) -> dict:
    """Los campos del n-esimo mensaje recibido que casa con `patron`, como los mando el juego."""
    vistos = [l for l in lineas if " <- " in l and re.search(patron, l)]
    texto = re.search(r"sub=[0-9a-f]{8} (\{.*\})$", vistos[n]).group(1)
    return ast.literal_eval(texto)


def empaquetar(orden: str, c: dict) -> bytes:
    cuerpo = "".join(f"{k}={v}\n" for k, v in c.items()).encode("latin-1")
    return orden.encode("latin-1") + b"\0\0\0\0" + (12 + len(cuerpo) + 1).to_bytes(4, "big") + cuerpo + b"\0"


class Cliente:
    def __init__(self, ip: str, puerto: int):
        self.s = socket.socket()
        self.s.bind((ip, 0))
        self.s.connect(("127.0.0.1", puerto))
        self.s.setblocking(False)
        self.buf = b""
        self.recibidos: list[tuple[str, dict]] = []

    def mandar(self, orden: str, c: dict) -> None:
        self.s.setblocking(True)
        self.s.sendall(empaquetar(orden, c))
        self.s.setblocking(False)

    def leer(self) -> None:
        while True:
            try:
                d = self.s.recv(65536)
            except BlockingIOError:
                break
            if not d:
                break
            self.buf += d
        while len(self.buf) >= 12:
            largo = int.from_bytes(self.buf[8:12], "big")
            if len(self.buf) < largo:
                break
            m, self.buf = self.buf[:largo], self.buf[largo:]
            orden = m[:4].decode("latin-1")
            sub = m[4:8].hex()
            c = {}
            for linea in m[12:].rstrip(b"\0").decode("latin-1").split("\n"):
                if "=" in linea:
                    k, v = linea.split("=", 1)
                    c[k] = v
            if sub != "00000000":
                c["(sub)"] = m[4:8].decode("latin-1")
            self.recibidos.append((orden, c))

    def cerrar(self) -> None:
        self.leer()
        self.s.close()


def gamertag(ip: str, nombre: str) -> str:
    with socket.socket() as s:
        s.bind((ip, 0))
        s.connect(("127.0.0.1", APP))
        s.sendall(f"GAMERTAG {nombre}\n".encode())
        s.settimeout(3)
        r = b""
        while not r.endswith(b"\n"):
            d = s.recv(64)
            if not d:
                break
            r += d
        return r.decode().strip()


def buscar(ip: str) -> str:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind((ip, 0))
        s.settimeout(3)
        s.sendto(b"NFSMW_DISCOVER\n", ("127.0.0.1", APP))
        return s.recvfrom(512)[0].decode()


def escenario(log: list[str]) -> list:
    """Lo que hace cada jugador, en orden: (quien, orden, campos), ("abrir", quien, puerto),
    ("cerrar", quien) y las preguntas de la app."""
    auxi = [campos(log, r"<- auxi .*'TEXT': 'SCF%3d0%0aV%3d0%0a'"),
            campos(log, r"<- auxi .*'TEXT': 'SCF%3d0%0aCN%3d-1944964556%0aPT%3d0.0354%0aPH%3d0.0625%0aPA%3d0.0%0aV%3d0%0a'")]
    base = campos(log, r"<- sele .*'MYGAME': '1', 'STATS': '5000', 'ASYNC': '1', 'MESGS': '1', 'ROOMS': '1', 'USERSETS': '1'\}")
    en_carrera = campos(log, r"<- sele .*'INGAME': '1'")
    con_sala = campos(log, r"<- sele .*'USERSETS': '1', 'USERSET0': '086.User'")
    uadm = campos(log, r"<- uadm .*'NAME': '086.User', 'DESC'")
    gset = campos(log, r"<- gset .*'NAME': 'User', 'MINSIZE': '2'")
    pasos = [("gamertag", A, "User"), ("gamertag", B, "victo"), ("buscar", A)]
    for quien, mid, gtag in ((A, r"\$001dd8000005", "User"), (B, r"\$001dd8000004", "victo")):
        pasos += [("abrir", quien, DIRECTORIO, "d"),
                  ((quien, "d"), "@tic", {}),
                  ((quien, "d"), "@dir", campos(log, rf"<- @dir .*'MID': '{mid}'")),
                  ("cerrar", (quien, "d")),
                  ("abrir", quien, SALAS, "s"),
                  ((quien, "s"), "addr", {"ADDR": quien, "PORT": "1"}),
                  ((quien, "s"), "skey", campos(log, r"<- skey ")),
                  ((quien, "s"), "news", campos(log, r"<- news ")),
                  ((quien, "s"), "auth", campos(log, rf"<- auth .*'GTAG': '{gtag}'")),
                  ((quien, "s"), "pers", campos(log, rf"<- pers .*'GTAG': '{gtag}'")),
                  ((quien, "s"), "sele", base)]
    a, b = (A, "s"), (B, "s")
    pasos += [
        # El anfitrion crea la sala y la partida.
        (a, "user", {"PERS": "User"}),
        (a, "usea", campos(log, r"<- usea .*'CUSTFLAGS': '-2147474432'")),
        (a, "~png", {"REF": "2026.10.18-15:11:32", "TIME": "8"}),
        (a, "auxi", auxi[0]),
        (a, "ucre", campos(log, r"<- ucre .*'NAME': '086.User'")),
        (a, "auxi", auxi[1]),
        (a, "gcre", campos(log, r"<- gcre .*'NAME': 'User'")),
        (a, "uadm", uadm),
        (a, "gset", gset),
        (a, "gset", campos(log, r"<- gset sub=[0-9a-f]{8} \{'SESS'")),
        # El otro la busca y se une.
        (b, "user", {"PERS": "victo"}),
        (b, "usea", campos(log, r"<- usea .*'CUSTFLAGS': '-2147473920'")),
        (b, "sele", {"USERSET0": "086.User"}),
        (b, "ujoi", {"NAME": "086.User"}),
        (b, "gjoi", {"NAME": "User"}),
        (b, "mesg", {"TEXT": "42", "ATTR": "EGS"}),
        (b, "auxi", auxi[0]),
        (b, "usea", campos(log, r"<- usea .*'NAME': '086.User'")),
        (a, "mesg", campos(log, r"<- mesg .*'ATTR': 'EGT'")),
        # La carrera.
        (a, "gsta", {"NAME": "User"}),
        (a, "uadm", uadm),
        (a, "sele", en_carrera),
        (b, "sele", en_carrera),
        (a, "gset", gset),
        ("gamertag", B, "User"),
        ("gamertag", A, "victo"),
        ("buscar", B),
        # Al acabar: los resultados de cada uno, la clasificacion y su perfil.
        (b, "sele", {"INGAME": "0"}),
        (b, "sele", con_sala),
        (b, "rank", campos(log, r"<- rank .*'REPT': 'victo'")),
        (a, "sele", {"INGAME": "0"}),
        (a, "sele", con_sala),
        (a, "rank", campos(log, r"<- rank .*'REPT': 'User'")),
        (a, "snap", {"INDEX": "1", "CHAN": "8", "RANGE": "1", "FIND": "$"}),
        (a, "snap", {"INDEX": "1", "CHAN": "5", "START": "0", "RANGE": "100"}),
        (b, "snap", {"INDEX": "1", "CHAN": "5", "START": "1", "RANGE": "1"}),
        (b, "user", {"PERS": "victo"}),
        # Salen: el otro deja la partida y la sala, y el anfitrion las borra.
        (b, "glea", {"NAME": "User", "FORCE": "1"}),
        (b, "sele", {"USERSET0": ""}),
        (b, "ulea", {"NAME": "086.User"}),
        (b, "usea", {"START": "0", "COUNT": "1000", "CUSTFLAGS": "0", "CUSTMASK": "66060289"}),
        (b, "zzzz", {"X": "1"}),
        (a, "gdel", {"NAME": "User", "FORCE": "1"}),
        (a, "udel", {"NAME": "086.User"}),
        # Otra sala del otro, y el anfitrion se va con ella abierta.
        (b, "ucre", {"NAME": "098.victo", "SIZE": "4", "TYPE": "0", "SYSFLAGS": "KV", "CUSTFLAGS": "IJKMY-",
                     "PARAMS": "V%3d192641%0aP%3d0%0aL%3d1%0aM%3d4%0a"}),
        (a, "sele", {"USERSET0": "098.victo"}),
        (a, "ujoi", {"NAME": "098.victo"}),
        ("cerrar", a),
        ("gamertag", B, "User"),
        ("gamertag", A, "Nuevo"),
        ("gamertag", B, "Nuevo"),
        ("cerrar", b),
    ]
    return pasos


def jugar(pasos: list) -> dict:
    """Los pasos contra el servidor que escucha ahora; lo que recibio cada uno."""
    clientes: dict = {}
    salida: dict = {"app": []}
    vivos = []
    for p in pasos:
        if p[0] == "abrir":
            _, ip, puerto, tipo = p
            clientes[(ip, tipo)] = Cliente(ip, puerto)
            salida.setdefault((ip, tipo), [])
            vivos.append((ip, tipo))
        elif p[0] == "cerrar":
            clientes[p[1]].cerrar()
            salida[p[1]] += clientes[p[1]].recibidos
            vivos.remove(p[1])
        elif p[0] == "gamertag":
            salida["app"].append(("gamertag", p[1], p[2], gamertag(p[1], p[2])))
        elif p[0] == "buscar":
            salida["app"].append(("buscar", p[1], buscar(p[1])))
        else:
            quien, orden, c = p
            clientes[quien].mandar(orden, c)
        time.sleep(ESPERA)
        for v in vivos:
            clientes[v].leer()
    for v in vivos:
        clientes[v].cerrar()
        salida[v] += clientes[v].recibidos
    return salida


def normalizar(m):
    if isinstance(m, tuple) and len(m) == 2 and isinstance(m[1], dict):
        orden, c = m
        c = {k: ("*" if k in AL_AZAR or (orden == "@dir" and k == "SESS") else v) for k, v in c.items()}
        # El ~png del servidor sale cada 20 s: segun cuando caiga.
        return None if orden == "~png" else (orden, c)
    if m[0] == "buscar":
        return ("buscar", m[1], re.sub(r"NAME=.*\n", "NAME=*\n", m[2]))
    return m


# Python guarda las sesiones en un set(): los registros de varios miembros (+usm, +usr...) salen
# en el orden de sus hash, distinto en cada arranque. Java y iOS los mandan en el orden en que
# llegaron. Los seguidos del mismo tipo se comparan sin mirar el orden.
SIN_ORDEN = {"+usm", "+usr", "+uss", "+snp"}


def ordenar_grupos(mensajes: list) -> list:
    salida, grupo = [], []
    for m in mensajes + [None]:
        if m and isinstance(m[1], dict) and m[0] in SIN_ORDEN and (not grupo or grupo[0][0] == m[0]):
            grupo.append(m)
            continue
        salida += sorted(grupo, key=repr)
        grupo = [m] if m and isinstance(m[1], dict) and m[0] in SIN_ORDEN else []
        if m and not grupo:
            salida.append(m)
    return salida


def esperar_log(ruta: Path, texto: str) -> None:
    for _ in range(100):
        if ruta.exists() and texto in ruta.read_text(encoding="utf-8", errors="replace"):
            return
        time.sleep(0.1)
    raise SystemExit(f"el servidor no arranca: {ruta}")


def con_python(proyecto: Path, tmp: Path, pasos: list) -> dict:
    carpeta = tmp / "python"
    carpeta.mkdir()
    shutil.copy(proyecto / "ea_server.py", carpeta)
    log = carpeta / "logs" / "server.log"
    p = subprocess.Popen([sys.executable, "ea_server.py", "--ip", "127.0.0.1", "--log", str(log), "--name", "prueba"],
                         cwd=carpeta, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        esperar_log(log, "EA server on")
        return jugar(pasos)
    finally:
        p.terminate()
        p.wait()


def con_cpp(binario: Path, tmp: Path, pasos: list) -> dict:
    carpeta = tmp / "cpp"
    p = subprocess.Popen([str(binario), str(carpeta / "datos"), str(carpeta / "logs")], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, text=True)
    try:
        if p.stdout.readline().strip() != "listo":
            raise SystemExit("el servidor de iOS no arranca")
        return jugar(pasos)
    finally:
        p.stdin.close()
        p.wait(timeout=10)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    proyecto, binario = Path(sys.argv[1]), Path(sys.argv[2])
    pasos = escenario(leer_log(proyecto / "logs" / "server.log", "15:09:36"))
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        python = con_python(proyecto, tmp, pasos)
        time.sleep(1)
        ios = con_cpp(binario, tmp, pasos)
    fallos = 0
    for quien in python:
        uno = ordenar_grupos([x for x in map(normalizar, python[quien]) if x])
        otro = ordenar_grupos([x for x in map(normalizar, ios.get(quien, [])) if x])
        iguales = sum(1 for x, y in zip(uno, otro) if x == y)
        print(f"{quien}: Python {len(uno)} mensajes, iOS {len(otro)}, iguales {iguales}")
        for i, (x, y) in enumerate(zip(uno, otro)):
            if x != y:
                fallos += 1
                print(f"  {i}: Python {x}\n  {' ' * len(str(i))}  iOS    {y}")
                if fallos > 12:
                    return 1
        if len(uno) != len(otro):
            fallos += 1
            resto = uno[len(otro):] or otro[len(uno):]
            print(f"  sobran en {'Python' if len(uno) > len(otro) else 'iOS'}: {resto[:4]}")
    print("iguales" if not fallos else f"{fallos} diferencias")
    return 1 if fallos else 0


if __name__ == "__main__":
    sys.exit(main())
