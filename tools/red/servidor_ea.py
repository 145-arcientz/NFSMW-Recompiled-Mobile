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
        self.auxi = ""
        # User sets a los que esta suscrito: nombre de sala -> ranura (0..4).
        self.sets = {}
        Sesion.cuenta = getattr(Sesion, "cuenta", 0) + 1
        self.id = Sesion.cuenta


class Servidor:
    def __init__(self, ip):
        self.ip = ip
        self.salas = {}
        self.partidas = {}
        self.sesiones = set()
        self.siguiente = 1000

    def olvidar(self, ses):
        """El jugador se fue: su sala desaparece y deja su partida."""
        self.salir_de_partida(ses)
        if ses.sala in self.salas:
            self.avisar_set(self.salas[ses.sala], ses, ses, quitar=True)  # lo suyo, nadie
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
        respuesta = [empaquetar(b"sele", campos), self.ficha(ses)]
        # USERSETk=<sala>: se suscribe en la ranura k a los miembros de esa sala
        # (el que se une lo manda antes de ujoi). Se le manda la sala (+ust) y
        # cada miembro que ya tiene (+usm).
        for clave, valor in campos.items():
            if not (clave.startswith("USERSET") and clave[7:].isdigit()):
                continue
            ranura = int(clave[7:])
            ses.sets = {n: k for n, k in ses.sets.items() if k != ranura}
            if not valor:
                continue
            ses.sets[valor] = ranura
            sala = self.sala_por_nombre(valor)
            if sala:
                respuesta.append(empaquetar(b"+ust", self.ficha_sala(sala)))
                for m in self.miembros(sala):
                    respuesta.append(self.ficha_miembro(m, ranura))
        return respuesta

    # -- user sets (las "salas" de ucre/ujoi/ulea/usea) ------------------------
    # La sala de juego (LobbyGameSessions, lista +360) es la lista de miembros
    # del user set en la ranura 0 (lista 24 del lobby). Se llena con '+usm'
    # (sub_8282EB78): S ranura, I, N, F, G y X (registro de 160 bytes,
    # sub_82826530: +4 nombre, +28 texto auxi con SCF/LT/CD/CE). Sin N, quita
    # al miembro I. El miembro 0 de la sala de juego es el que se llama como uno
    # mismo (sub_82544C28): sin su propia entrada la lista sale vacia y no llega
    # al minimo de jugadores.

    def sala_por_nombre(self, nombre):
        for sala in self.salas.values():
            if sala["N"] == nombre:
                return sala
        return None

    def miembros(self, sala):
        return [o for o in self.sesiones if o.sala == sala["I"]]

    def ficha_miembro(self, ses, ranura, quitar=False):
        if quitar:
            return empaquetar(b"+usm", {"S": ranura, "I": ses.id})
        return empaquetar(b"+usm", {
            "S": ranura,
            "I": ses.id,
            "N": ses.persona or ses.nombre,
            "F": "U",
            "G": ses.partida["IDENT"] if ses.partida else 0,
            "X": ses.auxi,
        })

    def avisar_set(self, sala, ses, quien, quitar=False):
        """'+usm' de `ses` a los suscritos a `sala`; lo de `quien` se devuelve."""
        propio = []
        for o in self.sesiones:
            ranura = o.sets.get(sala["N"])
            if ranura is None or not o.escritor:
                continue
            msg = self.ficha_miembro(ses, ranura, quitar)
            if o is quien:
                propio.append(msg)
            else:
                o.escritor.write(msg)
        return propio

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
        # Despues, su ficha con G = esta partida (ver ficha()).
        return [empaquetar(b"gcre", self.ficha_partida(p, ses)), self.ficha(ses)]

    def gjoi(self, ses, campos):
        p = self.partidas.get(campos.get("NAME", ""))
        if not p:
            apuntar(f"   gjoi: no hay partida '{campos.get('NAME')}'")
            return [empaquetar(b"gjoi", {}, sub=b"nfnd")]
        if ses not in p["jugadores"]:
            p["jugadores"].append(ses)
        ses.partida = p
        self.avisar(p, menos=ses)
        # Solo la ficha: el que entra se conecta al anfitrion con el SESS de esta
        # (sub_82542400). '+ses' es para cuando el anfitrion arranca (gsta): si le
        # llega antes, borra su sesion de Xbox.
        return [empaquetar(b"gjoi", self.ficha_partida(p, ses)), self.ficha(ses)]

    def gset(self, ses, campos):
        p = ses.partida
        if not p:
            return [empaquetar(b"gset", {})]
        # Se guarda todo lo que mande el anfitrion. Ojo con SESS: es su sesion de
        # Xbox codificada ("$..."); el que se une solo se conecta si su ficha trae
        # un SESS que empiece por '$' (sub_82542400 -> sub_82547838).
        for clave, valor in campos.items():
            if clave != "NAME":
                p[clave] = valor
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
        # Los que ya estan en la sala reciben la ficha del que entra ('+usr') y el
        # que entra, la de cada uno de ellos. Con esa ficha (LA, MA) el anfitrion da
        # de alta al nuevo en su conexion entre jugadores (sub_82547C50 ->
        # ConnApiAddClient); sin ella no abre sus puertos y no contesta al saludo.
        respuesta = [empaquetar(b"ujoi", self.ficha_sala(sala))]
        for otro in self.sesiones:
            if otro is not ses and otro.sala == sala["I"] and otro.escritor:
                apuntar(f"   +usr {ses.persona or ses.nombre} -> {otro.persona or otro.nombre}")
                otro.escritor.write(self.ficha(ses, b"+usr"))
                respuesta.append(self.ficha(otro, b"+usr"))
        respuesta.append(self.ficha(ses, b"+usr"))
        respuesta += self.avisar_set(sala, ses, ses)
        return respuesta

    def ulea(self, ses, campos):
        sala = self.salas.get(ses.sala)
        if sala and int(sala["C"]) > 1:
            sala["C"] = int(sala["C"]) - 1
        ses.sala = None
        respuesta = [empaquetar(b"ulea", {})]
        if sala:
            respuesta += self.avisar_set(sala, ses, ses, quitar=True)
        return respuesta

    def mesg(self, ses, campos):
        # Mensaje a la partida. Al entrar, el que se une manda TEXT=42 ATTR=EGS: el
        # "he llegado" que hace que el anfitrion lo de de alta en la conexion entre
        # jugadores (sub_822787A0: busca su ficha +usr por el nombre y llama a
        # ConnApiAddClient). A los demas les llega como '+msg': N remitente, T
        # texto, F banderas (la clave de una letra de 0x820A152C).
        p = ses.partida
        destinos = [j for j in (p["jugadores"] if p else []) if j is not ses and j.escritor]
        if not destinos and ses.sala is not None:
            destinos = [o for o in self.sesiones
                        if o is not ses and o.sala == ses.sala and o.escritor]
        for j in destinos:
            apuntar(f"   +msg {ses.persona or ses.nombre} -> {j.persona or j.nombre}: "
                    f"{campos.get('TEXT')} {campos.get('ATTR')}")
            j.escritor.write(empaquetar(b"+msg", {
                "N": ses.persona or ses.nombre,
                "T": campos.get("TEXT", ""),
                "F": campos.get("ATTR", ""),
            }))
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
        # El dueno es miembro de su sala y no manda 'sele USERSET0': se le da la
        # ranura 0 (la de la sala de juego) y su propia entrada.
        ses.sets = {n: k for n, k in ses.sets.items() if k != 0}
        ses.sets[sala["N"]] = 0
        return [empaquetar(b"ucre", self.ficha_sala(sala)), self.ficha(ses, b"+usr"),
                self.ficha_miembro(ses, 0)]

    def auxi(self, ses, campos):
        # Texto auxiliar del jugador (su estado en la sala); se guarda, se contesta
        # igual y, como cambia su ficha, '+usr' nueva a todos los de su sala, el
        # incluido.
        ses.auxi = campos.get("TEXT", "")
        respuesta = [empaquetar(b"auxi", campos)]
        if ses.sala is not None:
            for otro in self.sesiones:
                if otro is not ses and otro.sala == ses.sala and otro.escritor:
                    otro.escritor.write(self.ficha(ses, b"+usr"))
            respuesta.append(self.ficha(ses, b"+usr"))
            sala = self.salas.get(ses.sala)
            if sala:
                respuesta += self.avisar_set(sala, ses, ses)
        return respuesta

    def ficha(self, ses, orden=b"+who"):
        # La ficha del propio jugador. El juego monta con ella su entrada en la
        # conexion entre jugadores (sub_82548818): LA (IP local) y MA (la MADDR
        # que mando en auth: "$<XUID>^<XNADDR codificada>"). Sin MA, el ConnApi se
        # queda con XUID 0 y ConnApiOnline falla.
        return empaquetar(orden, {
            "I": ses.id,
            "M": ses.nombre,
            "N": ses.persona or ses.nombre,
            "MA": ses.maddr,
            "A": ses.ip,
            "LA": ses.ip,
            # G: la partida en la que esta (su IDENT; 0 = ninguna). El juego lo lee
            # en su ficha (+304, clave de una letra en 0x8202AA18) al recibir +mgm:
            # con 0 da la partida por perdida y la desmonta (sub_82542560).
            "G": ses.partida["IDENT"] if ses.partida else 0,
            "P": 1,
            "F": "U",
            "LO": "esES",
            "PRES": 1,
            # X: su texto auxi. El juego guarda cada ficha +usr en la lista de la
            # sala (LobbyGameSessions, +360) y lee de ese texto (registro +28) SCF,
            # LT, CD y CE: estado de conexion, latencia y estado de carrera.
            "X": ses.auxi,
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
    servidor.sesiones.add(ses)
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
    servidor.sesiones.discard(ses)
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
