package io.github.nfsmwrecomp;

import java.util.ArrayList;
import java.util.List;

/**
 * Las ediciones del juego (PAL Espana, USA, Japon) cuyo codigo lleva el APK.
 *
 * Cada edicion es un default.xex distinto, y su codigo traducido es una
 * libmain distinta. Un APK normal lleva una: la que compila CMake, en
 * libmain.so. Con el motor nativo, tools/android/apk_ediciones.py hace uno con
 * varias: las demas van en libmain_usa.so, libmain_jpn.so..., cada una con su
 * biblioteca de shaders en assets/nativo/usa..., y todas comparten el SDK (librexruntime.so),
 * SDL y los hooks de Turnip, que no dependen de la edicion.
 *
 * Al jugar, el SHA-256 del default.xex de la ISO (IsoXex) dice cual cargar.
 * Las partidas son las mismas para todas: la misma carpeta, como al instalar
 * el APK de una edicion encima del de otra.
 *
 * La tabla la escribe build.gradle.kts en BuildConfig.EDICIONES: una ficha por
 * edicion separada por ';', con los campos separados por '|'. La primera es la
 * principal, la de libmain.so.
 */
final class Ediciones {

    static final class Edicion {
        /** pal_es, usa, jpn. */
        final String id;
        /** "PAL España", "USA", "Japón". */
        final String nombre;
        /** SHA-256 del default.xex de esa edicion; "" si no se sabe (codigo de antes de las ediciones). */
        final String xex;
        /** El idioma y el pais de Xbox 360 que se le pasan al juego. */
        final int idioma;
        final int pais;
        /** Para System.loadLibrary: "main" o "main_usa". */
        final String libreria;
        /** La biblioteca de shaders del motor nativo, en los assets. */
        final String shaders;

        Edicion(String ficha) {
            String[] c = ficha.split("\\|", -1);
            id = c[0];
            nombre = c[1];
            xex = c[2];
            idioma = Integer.parseInt(c[3]);
            pais = Integer.parseInt(c[4]);
            libreria = c[5];
            shaders = c[6];
        }
    }

    static final List<Edicion> TODAS = leer(BuildConfig.EDICIONES);

    private Ediciones() {
    }

    private static List<Edicion> leer(String tabla) {
        List<Edicion> l = new ArrayList<>();
        for (String ficha : tabla.split(";")) {
            l.add(new Edicion(ficha));
        }
        return l;
    }

    /** La de libmain.so. */
    static Edicion principal() {
        return TODAS.get(0);
    }

    static boolean varias() {
        return TODAS.size() > 1;
    }

    /** La edicion de ese default.xex si el APK la lleva; null si no (o si no se pudo leer). */
    static Edicion deXex(String xex) {
        if (xex == null || xex.isEmpty()) {
            return null;
        }
        for (Edicion e : TODAS) {
            if (xex.equals(e.xex)) {
                return e;
            }
        }
        return null;
    }

    /**
     * Si se sabe que esa ISO es de una edicion que el APK no lleva. Sin saber
     * de que edicion es el codigo (sin SHA-256), no se puede decir.
     */
    static boolean ajena(String xex) {
        return xex != null && !xex.isEmpty() && !principal().xex.isEmpty() && deXex(xex) == null;
    }

    /** Con la que se juega esa ISO: la suya si va en el APK; si no, la principal. */
    static Edicion paraJugar(String xex) {
        Edicion e = deXex(xex);
        return e != null ? e : principal();
    }

    static Edicion porId(String id) {
        for (Edicion e : TODAS) {
            if (e.id.equals(id)) {
                return e;
            }
        }
        return principal();
    }

    /** "PAL España / USA / Japón", para los textos. */
    static String nombres() {
        StringBuilder s = new StringBuilder();
        for (Edicion e : TODAS) {
            if (s.length() > 0) {
                s.append(" / ");
            }
            s.append(e.nombre);
        }
        return s.toString();
    }
}
