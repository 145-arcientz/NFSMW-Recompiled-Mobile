#!/usr/bin/env python3
"""
Un APK con varias ediciones del juego (motor nativo): PAL Espana y las que esten
preparadas (USA, Japon, las PAL alemana, francesa, italiana e inglesa...).

    python tools/android/apk_ediciones.py                    # todas las preparadas
    python tools/android/apk_ediciones.py --ediciones usa    # PAL Espana y USA
    python tools/android/apk_ediciones.py --registro         # con el log del juego

Antes, cada edicion preparada con tools/android/preparar_nativo.py (la PAL
Espana primero). Sale out/apk/nfsmw-all-<version>-release.apk (la versionName
de android/app/build.gradle.kts). Como todo APK
con el juego dentro, NO SE PUBLICA.


COMO
====

Cada edicion es su propio codigo traducido: una libmain distinta (unos 40 MB,
16 comprimida). El resto -el SDK (librexruntime.so), SDL, libc++ y los hooks de
Turnip- no depende de la edicion: las apps de cada una (app, app_usa...) solo
cambian direcciones, y todas compilan el mismo SDK. Asi que el APK lleva el
resto una vez y una libmain por edicion, como el fork Evolved:

  1. Compila la parte nativa de cada edicion (gradlew externalNativeBuildRelease,
     con -Pnfsmw.edicion=<id> si no es la principal). Cada una tiene su carpeta
     .cxx: despues de la primera vez, solo se recompila lo que cambia.
  2. Comprueba que el librexruntime.so de cada una exporta exactamente lo mismo
     que el de la principal: si no, el SDK cambio entre una compilacion y otra y
     una libmain podria llamar a lo que el APK no lleva.
  3. Deja en android/app/build/ediciones las libmain de las demas
     (jniLibs/arm64-v8a/libmain_<id>.so), sus bibliotecas de shaders
     (assets/nativo/<id>/nfsmw_shaders.nfsp) y sus fichas (ediciones.json).
  4. gradlew assembleRelease -Pnfsmw.motor=nativo -Pnfsmw.ediciones=<esa carpeta>:
     la principal en libmain.so y lo demas al lado (build.gradle.kts).

Al jugar, la app mira el SHA-256 del default.xex de la ISO y carga la libmain de
su edicion, con sus shaders, su idioma y su pais (Ediciones.java). Las partidas
son las mismas para todas.
"""

import argparse
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys

RAIZ = pathlib.Path(__file__).resolve().parents[2]
ANDROID = RAIZ / "android"
APP = ANDROID / "app"
CARPETA = APP / "build" / "ediciones"
ABI = "arm64-v8a"
# El orden en que salen en el APK (y en la pantalla de inicio), detras de la principal.
ORDEN = ["usa", "jpn", "pal_de", "pal_fr", "pal_it", "pal_en", "pal_pl", "kor", "cht"]


def fallar(msg):
    sys.exit(f"[ERROR] {msg}")


def version_java(carpeta):
    """La version principal de un JDK (7, 17, 21...) por su fichero release; 0 si no se sabe."""
    try:
        texto = (pathlib.Path(carpeta) / "release").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return 0
    m = re.search(r'^JAVA_VERSION="(?:1\.)?(\d+)', texto, re.M)
    return int(m.group(1)) if m else 0


def gradle(*args):
    env = dict(os.environ)
    # Gradle 9 pide Java 17 o mas. Si JAVA_HOME no lo es (o no esta), el de
    # Android Studio.
    if version_java(env.get("JAVA_HOME", "")) < 17:
        jbr = pathlib.Path(r"C:\Program Files\Android\Android Studio\jbr")
        if version_java(jbr) >= 17:
            env["JAVA_HOME"] = str(jbr)
    exe = ANDROID / ("gradlew.bat" if platform.system() == "Windows" else "gradlew")
    orden = [str(exe), *args, "-Pnfsmw.motor=nativo", "--offline", "-q"]
    print("== " + " ".join(orden[1:]), flush=True)
    r = subprocess.run(orden, cwd=ANDROID, env=env)
    if r.returncode != 0:
        fallar(f"gradle ha fallado (codigo {r.returncode}).")


def ficha(arbol, edicion):
    """El edicion.json que deja preparar_nativo.py, comprobando que esta todo."""
    out = arbol / "out" if edicion is None else arbol / "out" / edicion
    app = arbol / ("app" if edicion is None else f"app_{edicion}")
    if not (out / "edicion.json").is_file():
        fallar(f"Falta {out / 'edicion.json'}. Prepara esa edicion con tools/android/preparar_nativo.py.")
    for f in (out / "nfsmw_shaders.nfsp", app / "generated" / "default" / "sources.cmake"):
        if not f.is_file():
            fallar(f"Falta {f}. Prepara esa edicion con tools/android/preparar_nativo.py.")
    with open(out / "edicion.json", encoding="utf-8") as fh:
        datos = json.load(fh)
    return datos, out / "nfsmw_shaders.nfsp"


def preparadas(arbol):
    """Las que tienen out/<id>/edicion.json, en el orden de ORDEN y despues las demas."""
    return sorted((p.parent.name for p in (arbol / "out").glob("*/edicion.json")),
                  key=lambda e: (ORDEN.index(e) if e in ORDEN else len(ORDEN), e))


def carpeta_nativa(app_nativa):
    """Las librerias que compilo CMake para esa app del arbol nativo (app, app_usa...):
    AGP da a cada juego de argumentos su carpeta, .cxx/Release/<huella>."""
    candidatas = []
    for cache in (APP / ".cxx" / "Release").glob(f"*/{ABI}/CMakeCache.txt"):
        texto = cache.read_text(encoding="utf-8", errors="replace")
        if "NFSMW_MOTOR:STRING=nativo" not in texto:
            continue
        if not re.search(rf"^NFSMW_NATIVO_APP:STRING={re.escape(app_nativa)}$", texto, re.M):
            continue
        obj = APP / "build" / "intermediates" / "cxx" / "Release" / cache.parent.parent.name / "obj" / ABI
        if (obj / "libmain.so").is_file():
            candidatas.append(obj)
    if not candidatas:
        fallar(f"No encuentro la libmain.so compilada de {app_nativa} en android/app/.cxx.")
    return max(candidatas, key=lambda o: (o / "libmain.so").stat().st_mtime)


def salida():
    """out/apk/nfsmw-all-<versionName>-release.apk, como los de una edicion
    (nfsmw-pal_es-0.4.1-release.apk)."""
    texto = (APP / "build.gradle.kts").read_text(encoding="utf-8")
    version = re.search(r'versionName\s*=\s*"([^"]+)"', texto).group(1)
    return RAIZ / "out" / "apk" / f"nfsmw-all-{version}-release.apk"


def ndk():
    texto = (APP / "build.gradle.kts").read_text(encoding="utf-8")
    version = re.search(r'ndkVersion\s*=\s*"([^"]+)"', texto).group(1)
    sdk = None
    props = ANDROID / "local.properties"
    if props.is_file():
        m = re.search(r"^sdk\.dir=(.*)$", props.read_text(encoding="utf-8"), re.M)
        if m:
            sdk = pathlib.Path(m.group(1).strip().replace("\\:", ":").replace("\\\\", "\\"))
    sdk = sdk or pathlib.Path(os.environ.get("ANDROID_HOME", ""))
    exe = "llvm-nm.exe" if platform.system() == "Windows" else "llvm-nm"
    nm = next(iter((sdk / "ndk" / version / "toolchains" / "llvm" / "prebuilt").glob(f"*/bin/{exe}")), None)
    if nm is None:
        fallar(f"No encuentro llvm-nm del NDK {version} en {sdk}.")
    return nm


def exportados(nm, so):
    r = subprocess.run([str(nm), "-D", "--defined-only", "--format=just-symbols", str(so)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        fallar(f"llvm-nm ha fallado con {so}: {r.stderr.strip()}")
    return set(r.stdout.split())


def main():
    ap = argparse.ArgumentParser(description="Un APK del motor nativo con varias ediciones del juego.")
    ap.add_argument("--arbol", default=str(RAIZ / "nativo"),
                    help="el arbol del motor nativo (por defecto, nativo/ en este repositorio)")
    ap.add_argument("--ediciones",
                    help="las que van ademas de la principal, separadas por comas (por defecto, "
                         "todas las preparadas: out/<id>/edicion.json)")
    ap.add_argument("--registro", action="store_true", help="con el log del juego (-Pnfsmw.registro=true)")
    ap.add_argument("--depurable", action="store_true", help="depurable con run-as (-Pnfsmw.depurable=true)")
    args = ap.parse_args()
    arbol = pathlib.Path(args.arbol).resolve()

    principal, _ = ficha(arbol, None)
    otras = args.ediciones.split(",") if args.ediciones else preparadas(arbol)
    otras = [e.strip() for e in otras if e.strip() and e.strip() != principal["id"]]
    if not otras:
        fallar("No hay otra edicion preparada que meter. Prepara la USA o la japonesa con "
               "tools/android/preparar_nativo.py --iso <su ISO>.")
    fichas = {e: ficha(arbol, e) for e in otras}
    print(f"== Ediciones: {principal['nombre']} (principal), "
          + ", ".join(f[0]["nombre"] for f in fichas.values()), flush=True)

    # 1. La parte nativa de cada una, la principal tambien: asi las comprobaciones
    # comparan con el SDK que va a llevar el APK.
    gradle(":app:externalNativeBuildRelease")
    for e in otras:
        gradle(":app:externalNativeBuildRelease", f"-Pnfsmw.edicion={e}")

    # 2. El mismo SDK en todas.
    nm = ndk()
    obj_principal = carpeta_nativa("app")
    sdk = exportados(nm, obj_principal / "librexruntime.so")
    for e in otras:
        suyo = exportados(nm, carpeta_nativa(f"app_{e}") / "librexruntime.so")
        if suyo != sdk:
            faltan = sorted(sdk ^ suyo)
            fallar(f"El SDK de {e} no exporta lo mismo que el de la principal ({len(faltan)} simbolos, "
                   f"p. ej. {faltan[:3]}). Vuelve a lanzar este script: compila todas con el SDK de ahora.")
    print(f"[ok] el mismo SDK en todas ({len(sdk)} simbolos exportados)")

    # 3. Lo de las demas, en su carpeta.
    if CARPETA.exists():
        shutil.rmtree(CARPETA)
    libs = CARPETA / "jniLibs" / ABI
    libs.mkdir(parents=True)
    for e, (datos, shaders) in fichas.items():
        shutil.copy2(carpeta_nativa(f"app_{e}") / "libmain.so", libs / f"libmain_{e}.so")
        destino = CARPETA / "assets" / "nativo" / e
        destino.mkdir(parents=True)
        shutil.copy2(shaders, destino / "nfsmw_shaders.nfsp")
    with open(CARPETA / "ediciones.json", "w", encoding="utf-8", newline="\n") as fh:
        json.dump([d for d, _ in fichas.values()], fh, ensure_ascii=False, indent=2)
        fh.write("\n")

    # 4. El APK.
    extra = [f"-Pnfsmw.ediciones={CARPETA}"]
    if args.registro:
        extra.append("-Pnfsmw.registro=true")
    if args.depurable:
        extra.append("-Pnfsmw.depurable=true")
    gradle(":app:assembleRelease", *extra)
    apk = APP / "build" / "outputs" / "apk" / "release" / "app-release.apk"
    destino = salida()
    destino.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(apk, destino)
    print(f"\n[ok] {destino} ({destino.stat().st_size / 1e6:.1f} MB): "
          + ", ".join([principal["nombre"]] + [f[0]["nombre"] for f in fichas.values()]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
