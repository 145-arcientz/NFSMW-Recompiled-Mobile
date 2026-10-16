#!/usr/bin/env python3
"""
Servidor de salas de EA minimo, para jugar Most Wanted (360) en la LAN.

    python tools/red/servidor_ea.py --ip 10.0.0.4

El juego, con --red_lan=true --red_live=true --red_dns=<IP del PC>, busca
stestlobby03.beta.ea.com y se conecta aqui (ver docs/diario/red-y-privilegios.md).

EL PROTOCOLO
============
Es el de salas de EA de 2003-2006 (DirtySock, "Aries"): cada mensaje lleva una
cabecera de 12 bytes

    4 bytes   orden, en texto: "@dir", "addr", "skey", "auth"...
    4 bytes   subcodigo: 0 si va bien; en una respuesta de error, el codigo en texto
    4 bytes   largo total del mensaje, cabecera incluida, en big-endian

y luego texto CLAVE=valor separado por saltos de linea, acabado en un 0.

Dos servidores:
  - directorio (30960): el juego pide @dir y se le contesta con la IP y el
    puerto del servidor de salas.
  - salas (10901): el resto (addr, skey, news, auth, pers, salas, partidas...).

Lo que no se conoce todavia se contesta con la misma orden y sin datos, y queda
apuntado: el log (out/red/servidor.log) es lo que dice que falta.
"""
import argparse
import asyncio
import datetime
import pathlib
import random
import struct
import sys

RAIZ = pathlib.Path(__file__).resolve().parents[2]
LOG = RAIZ / "out" / "red" / "servidor.log"

PUERTO_DIR = 30960
PUERTO_SALAS = 10901


def apuntar(texto):
    linea = f"{datetime.datetime.now():%H:%M:%S.%f}"[:-3] + " " + texto
    print(linea, flush=True)
    with LOG.open("a", encoding="utf-8") as f:
        f.write(linea + "\n")


def empaquetar(orden, campos=None, sub=b"\0\0\0\0"):
    cuerpo = b""
    if campos:
        cuerpo = "".join(f"{k}={v}\n" for k, v in campos.items()).encode("latin-1")
    cuerpo += b"\0"
    return orden + sub + struct.pack(">I", 12 + len(cuerpo)) + cuerpo


def leer_campos(cuerpo):
    texto = cuerpo.rstrip(b"\0").decode("latin-1", "replace")
    campos = {}
    for linea in texto.split("\n"):
        if "=" in linea:
            k, v = linea.split("=", 1)
            campos[k] = v
    return campos


class Sesion:
    """Lo que el servidor sabe de un jugador conectado."""

    def __init__(self, ip, escritor=None):
        self.ip = ip
        self.nombre = "VICTOR"
        self.persona = None
        self.sala = None
        self.partida = None
        self.escritor = escritor
        self.maddr = ""
        self.opparam = ""
        Sesion.cuenta = getattr(Sesion, "cuenta", 0) + 1
        self.id = Sesion.cuenta


class Servidor:
    def __init__(self, ip):
        self.ip = ip
        self.salas = {}
        self.partidas = {}
        self.siguiente = 1000

    def olvidar(self, ses):
        """El jugador se fue: su sala desaparece y deja su partida."""
        self.salir_de_partida(ses)
        if ses.sala in self.salas and self.salas[ses.sala]["O"] == (ses.persona or ses.nombre):
            apuntar(f"   sala {ses.sala} de {self.salas[ses.sala]['O']} borrada")
            del self.salas[ses.sala]

    # -- directorio -------------------------------------------------------
    def dir_(self, ses, campos):
        return [empaquetar(b"@dir", {
            "ADDR": self.ip,
            "PORT": PUERTO_SALAS,
            "SESS": random.randint(1000000000, 1999999999),
            "MASK": "%032x" % random.getrandbits(128),
        })]

    def tic(self, ses, campos):
        # "@tic RC4+MD5-V2": el cliente ofrece cifrar la sesion. Sin respuesta,
        # sigue en claro (manda @dir sin esperar).
        return []

    # -- salas -------------------------------------------------------------
    def addr(self, ses, campos):
        return []  # el cliente dice su IP y puerto; no se contesta

    def skey(self, ses, campos):
        return [empaquetar(b"skey", {"SKEY": "$51ba8aee64ddfacae5baefa6bf61e009"})]

    def news(self, ses, campos):
        return [empaquetar(b"news", {
            "BUDDY_SERVER": self.ip,
            "BUDDY_PORT": 10899,
            "TOSAC_URL": "",
            "TOSA_URL": "",
            "TOS_URL": "",
        })]

    def sele(self, ses, campos):
        # Se le devuelve lo mismo que pide ("si, todo eso") y, detras, su propia
        # ficha (+who): los servidores de EA la mandaban al entrar.
        return [empaquetar(b"sele", campos), self.ficha(ses)]

    def user(self, ses, campos):
        # Pide la ficha de un perfil (al entrar, la suya).
        return [empaquetar(b"user", {
            "PERS": campos.get("PERS", ses.persona or ses.nombre),
            "LAST": "2005.11.15-00:00:00",
            "EXPR": "1072566000",
            "STAT": "",
        })]

    def usea(self, ses, campos):
        # Busqueda de salas de jugador (la de partida rapida, con CUSTFLAGS/CUSTMASK).
        # La respuesta solo dice cuantas (COUNT); cada una llega despues en un
        # '+uss' con su ficha: la tabla de mensajes del juego (0x828F2264) manda
        # '+uss' a sub_8282EF18, que la mete en la lista de resultados.
        otras = [s for s in self.salas.values() if s["O"] != (ses.persona or ses.nombre)]
        r = [empaquetar(b"usea", {"START": campos.get("START", 0), "COUNT": len(otras)})]
        r += [empaquetar(b"+uss", self.ficha_sala(s)) for s in otras]
        return r

    # -- partidas ------------------------------------------------------------
    # Una partida es la carrera dentro de una sala. La ficha que lee el juego
    # (sub_82825C90): IDENT NAME SELF HOST PARAMS PLATPARAMS ROOM CUSTFLAGS
    # SYSFLAGS COUNT PRIV MINSIZE MAXSIZE NUMPART SEED WHEN AUTH SESS, y por cada
    # jugador i (cadenas del juego): OPPO OPID ADDR LADDR MADDR OPPART OPPARAM
    # OPFLAG PRES, con PARTSIZE0/PARTPARAMS0 para la unica "parte".
    # Avisos al resto: '+mgm' (tu partida ha cambiado) y '+ses' (empieza).

    def ficha_partida(self, p, para):
        f = {
            "IDENT": p["IDENT"],
            "NAME": p["NAME"],
            "SELF": para.persona or para.nombre,
            "HOST": p["HOST"],
            "PARAMS": p["PARAMS"],
            "PLATPARAMS": p["PLATPARAMS"],
            "ROOM": p["ROOM"],
            "CUSTFLAGS": p["CUSTFLAGS"],
            "SYSFLAGS": p["SYSFLAGS"],
            "COUNT": len(p["jugadores"]),
            "PRIV": 0,
            "MINSIZE": p["MINSIZE"],
            "MAXSIZE": p["MAXSIZE"],
            "NUMPART": 1,
            "PARTSIZE0": p["MAXSIZE"],
            "PARTPARAMS0": "",
            "SEED": p["SEED"],
            "WHEN": p["WHEN"],
            "AUTH": "",
            "SESS": p.get("SESS", ""),
        }
        for i, j in enumerate(p["jugadores"]):
            f[f"OPPO{i}"] = j.persona or j.nombre
            f[f"OPID{i}"] = j.id
            f[f"ADDR{i}"] = j.ip
            f[f"LADDR{i}"] = j.ip
            f[f"MADDR{i}"] = j.maddr
            f[f"OPPART{i}"] = 0
            f[f"OPPARAM{i}"] = j.opparam
            f[f"OPFLAG{i}"] = 0
            f[f"PRES{i}"] = 0
        return f

    def avisar(self, p, orden=b"+mgm", menos=None):
        """Manda la ficha de la partida a todos sus jugadores (menos uno)."""
        for j in p["jugadores"]:
            if j is menos or not j.escritor:
                continue
            msg = empaquetar(orden, self.ficha_partida(p, j))
            apuntar(f"   {orden.decode()} -> {j.persona or j.nombre} (COUNT={len(p['jugadores'])})")
            j.escritor.write(msg)

    def gcre(self, ses, campos):
        yo = ses.persona or ses.nombre
        self.siguiente += 1
        p = {
            "IDENT": self.siguiente,
            "NAME": campos.get("NAME", yo),
            "HOST": yo,
            "PARAMS": campos.get("PARAMS", ""),
            "PLATPARAMS": campos.get("PLATPARAMS", ""),
            "ROOM": ses.sala or 0,
            "CUSTFLAGS": campos.get("CUSTFLAGS", 0),
            "SYSFLAGS": campos.get("SYSFLAGS", 0),
            "MINSIZE": campos.get("MINSIZE", 2),
            "MAXSIZE": campos.get("MAXSIZE", 4),
            "SEED": random.randint(1, 2**31 - 1),
            "WHEN": datetime.datetime.now().strftime("%Y.%m.%d-%H:%M:%S"),
            "jugadores": [ses],
        }
        self.partidas[p["NAME"]] = p
        ses.partida = p
        return [empaquetar(b"gcre", self.ficha_partida(p, ses))]

    def gjoi(self, ses, campos):
        p = self.partidas.get(campos.get("NAME", ""))
        if not p:
            apuntar(f"   gjoi: no hay partida '{campos.get('NAME')}'")
            return [empaquetar(b"gjoi", {}, sub=b"nfnd")]
        if ses not in p["jugadores"]:
            p["jugadores"].append(ses)
        ses.partida = p
        self.avisar(p, menos=ses)
        # La sala de juego del 360 ya es una sesion entre los jugadores
        # (LobbyGameSessions::JoinSession): sin '+ses' el que entra se queda en
        # "Esperando a que se conecten los jugadores" y nadie abre la conexion
        # directa. Se manda a todos en cuanto hay alguien mas.
        if not p.get("SESS"):
            p["SESS"] = random.randint(1000000, 9999999)
        respuesta = [empaquetar(b"gjoi", self.ficha_partida(p, ses))]
        self.avisar(p, orden=b"+ses", menos=ses)
        respuesta.append(empaquetar(b"+ses", self.ficha_partida(p, ses)))
        return respuesta

    def gset(self, ses, campos):
        p = ses.partida
        if not p:
            return [empaquetar(b"gset", {})]
        for clave in ("PARAMS", "PLATPARAMS", "CUSTFLAGS", "SYSFLAGS", "MINSIZE", "MAXSIZE"):
            if clave in campos:
                p[clave] = campos[clave]
        self.avisar(p, menos=ses)
        return [empaquetar(b"gset", self.ficha_partida(p, ses))]

    def salir_de_partida(self, ses):
        p = getattr(ses, "partida", None)
        if not p:
            return
        if ses in p["jugadores"]:
            p["jugadores"].remove(ses)
        ses.partida = None
        if not p["jugadores"] or p["HOST"] == (ses.persona or ses.nombre):
            self.partidas.pop(p["NAME"], None)
            for j in p["jugadores"]:
                j.partida = None
            apuntar(f"   partida {p['NAME']} cerrada")
        else:
            self.avisar(p)

    def glea(self, ses, campos):
        self.salir_de_partida(ses)
        return [empaquetar(b"glea", {})]

    def gdel(self, ses, campos):
        self.salir_de_partida(ses)
        return [empaquetar(b"gdel", {})]

    def gsta(self, ses, campos):
        # El anfitrion arranca: '+ses' con la ficha (y su SESS) a todos.
        p = ses.partida
        if not p:
            return [empaquetar(b"gsta", {})]
        p["SESS"] = random.randint(1000000, 9999999)
        self.avisar(p, orden=b"+ses")
        return [empaquetar(b"gsta", {})]

    def buscar_sala(self, campos):
        for clave in ("I", "IDENT", "ROOM"):
            if campos.get(clave, "").lstrip("-").isdigit():
                sala = self.salas.get(int(campos[clave]))
                if sala:
                    return sala
        nombre = campos.get("NAME") or campos.get("N")
        for sala in self.salas.values():
            if sala["N"] == nombre or sala["O"] == nombre:
                return sala
        return None

    def ujoi(self, ses, campos):
        # Unirse a la sala de otro (lo que se ha encontrado con usea). Formato de
        # la peticion aun por ver: se busca la sala por id o por nombre.
        sala = self.buscar_sala(campos)
        if not sala:
            apuntar(f"   ujoi: no encuentro la sala de {campos}")
            return [empaquetar(b"ujoi", {}, sub=b"nfnd")]
        sala["C"] = int(sala["C"]) + 1
        ses.sala = sala["I"]
        return [empaquetar(b"ujoi", self.ficha_sala(sala))]

    def ulea(self, ses, campos):
        sala = self.salas.get(ses.sala)
        if sala and int(sala["C"]) > 1:
            sala["C"] = int(sala["C"]) - 1
        ses.sala = None
        return [empaquetar(b"ulea", {})]

    def mesg(self, ses, campos):
        # Mensaje a la sala (TEXT=42 ATTR=EGS al crear la partida). Se acepta.
        return [empaquetar(b"mesg", {})]

    def uadm(self, ses, campos):
        # El anfitrion cambia su sala (parametros, banderas): se guarda y se
        # contesta con la ficha nueva.
        sala = self.salas.get(ses.sala)
        if sala:
            sala["N"] = campos.get("NAME", sala["N"])
            sala["D"] = campos.get("DESC", sala["D"])
            sala["P"] = campos.get("PARAMS", sala["P"])
            sala["CF"] = campos.get("CUSTFLAGS", sala["CF"])
            return [empaquetar(b"uadm", self.ficha_sala(sala))]
        return [empaquetar(b"uadm", {})]

    def ficha_sala(self, sala):
        # Las claves que lee el juego de la respuesta (sub_82825B28, en orden):
        # I ident, T tipo, SF/CF banderas, O dueno (si va vacio el juego no sigue
        # con gcre), S plazas, C ocupadas, N nombre, D descripcion, P parametros.
        return {
            "I": sala["I"],
            "T": sala["T"],
            "SF": sala["SF"],
            "CF": sala["CF"],
            "O": sala["O"],
            "S": sala["S"],
            "C": sala["C"],
            "N": sala["N"],
            "D": sala["D"],
            "P": sala["P"],
        }

    def ucre(self, ses, campos):
        # Crear sala de jugador ("Crear partida" en partida rapida). Despues, el
        # juego manda gcre para crear la partida dentro.
        self.siguiente += 1
        sala = {
            "I": self.siguiente,
            "T": campos.get("TYPE", 0),
            "SF": campos.get("SYSFLAGS", ""),
            "CF": campos.get("CUSTFLAGS", ""),
            "O": ses.persona or ses.nombre,
            "S": campos.get("SIZE", 4),
            "C": 1,
            "N": campos.get("NAME", ""),
            "D": "",
            "P": campos.get("PARAMS", ""),
        }
        self.salas[sala["I"]] = sala
        ses.sala = sala["I"]
        return [empaquetar(b"ucre", self.ficha_sala(sala))]

    def auxi(self, ses, campos):
        # Texto auxiliar del jugador (su presencia); se guarda y se contesta igual.
        ses.auxi = campos.get("TEXT", "")
        return [empaquetar(b"auxi", campos)]

    def ficha(self, ses):
        return empaquetar(b"+who", {
            "M": ses.nombre,
            "N": ses.persona or ses.nombre,
            "A": ses.ip,
            "LA": ses.ip,
            "P": 1,
            "F": "U",
            "LO": "esES",
            "PRES": 1,
        })

    def auth(self, ses, campos):
        ses.nombre = campos.get("NAME") or campos.get("GTAG") or ses.nombre
        ses.maddr = campos.get("MADDR", ses.maddr)
        return [empaquetar(b"auth", {
            "NAME": ses.nombre,
            "ADDR": ses.ip,
            "PERSONAS": ses.nombre,
            "LOC": "esES",
            "MAIL": "lan@lan",
            "SPAM": "NN",
            "TOS": 1,
            "SINCE": "2005.11.15-00:00:00",
            "LAST": "2005.11.15-00:00:00",
        })]

    def pers(self, ses, campos):
        ses.persona = campos.get("PERS") or campos.get("GTAG") or ses.nombre
        ses.maddr = campos.get("MADDR", ses.maddr)
        return [empaquetar(b"pers", {
            "PERS": ses.persona,
            "LKEY": "%032x" % random.getrandbits(128),
            "LOC": "esES",
            "A": ses.ip,
            "LA": ses.ip,
            "IDLE": 600000,
        })]

    def png(self, ses, campos):
        # La respuesta del cliente a nuestro ping: no se contesta (seria un
        # bucle de pings).
        return []

    def responder(self, ses, orden, campos):
        nombre = orden.decode("latin-1").lstrip("@~").replace(" ", "_")
        metodo = getattr(self, nombre + "_", None) or getattr(self, nombre, None)
        if metodo:
            return metodo(ses, campos), True
        return [empaquetar(orden)], False


async def latido(escritor, puerto):
    """El servidor de salas manda ~png cada poco; sin trafico, el cliente da la
    conexion por muerta a los 60 s y vuelve al menu principal."""
    try:
        while True:
            await asyncio.sleep(20)
            ref = datetime.datetime.now().strftime("%Y.%m.%d-%H:%M:%S")
            escritor.write(empaquetar(b"~png", {"REF": ref, "TIME": 2}))
            await escritor.drain()
    except (ConnectionError, asyncio.CancelledError):
        pass


async def cliente(lector, escritor, servidor, puerto):
    otro = escritor.get_extra_info("peername")
    ses = Sesion(otro[0], escritor)
    apuntar(f"[{puerto}] conexion de {otro[0]}:{otro[1]}")
    bufer = b""
    tarea_latido = asyncio.create_task(latido(escritor, puerto)) if puerto == PUERTO_SALAS else None
    try:
        while True:
            datos = await lector.read(65536)
            if not datos:
                break
            bufer += datos
            while len(bufer) >= 12:
                largo = struct.unpack(">I", bufer[8:12])[0]
                if largo < 12 or largo > 1 << 20:
                    apuntar(f"[{puerto}] largo imposible {largo}: {bufer[:32].hex()}")
                    bufer = b""
                    break
                if len(bufer) < largo:
                    break
                msg, bufer = bufer[:largo], bufer[largo:]
                orden, sub, cuerpo = msg[:4], msg[4:8], msg[12:]
                campos = leer_campos(cuerpo)
                apuntar(f"[{puerto}] <- {orden.decode('latin-1')} sub={sub.hex()} {campos}")
                respuestas, conocida = servidor.responder(ses, orden, campos)
                if not conocida:
                    apuntar(f"[{puerto}]    (orden sin manejar: se contesta vacia)")
                for r in respuestas:
                    apuntar(f"[{puerto}] -> {r[:4].decode('latin-1')} {leer_campos(r[12:])}")
                    escritor.write(r)
                await escritor.drain()
    except ConnectionError as e:
        apuntar(f"[{puerto}] {e}")
    if tarea_latido:
        tarea_latido.cancel()
    servidor.olvidar(ses)
    apuntar(f"[{puerto}] {otro[0]}:{otro[1]} cerro")
    escritor.close()


async def principal(ip):
    servidor = Servidor(ip)
    for p in (PUERTO_DIR, PUERTO_SALAS):
        await asyncio.start_server(lambda r, w, p=p: cliente(r, w, servidor, p), "0.0.0.0", p)
    apuntar(f"servidor de EA en {ip}: directorio {PUERTO_DIR}, salas {PUERTO_SALAS}")
    await asyncio.Event().wait()


def main():
    ap = argparse.ArgumentParser(description="Servidor de salas de EA minimo para la LAN.")
    ap.add_argument("--ip", required=True, help="la IP de este PC en la LAN (la que va en red_dns)")
    a = ap.parse_args()
    LOG.parent.mkdir(parents=True, exist_ok=True)
    apuntar("=" * 60)
    try:
        asyncio.run(principal(a.ip))
    except KeyboardInterrupt:
        apuntar("parado")


if __name__ == "__main__":
    sys.exit(main())
