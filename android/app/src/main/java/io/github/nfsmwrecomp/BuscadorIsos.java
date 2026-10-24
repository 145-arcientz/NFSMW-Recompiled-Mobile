package io.github.nfsmwrecomp;

import android.content.Context;
import android.net.Uri;
import android.os.Environment;

import java.io.File;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * Busca las .iso del juego en el almacenamiento interno y en las tarjetas SD,
 * para elegirla sin el selector de Android: en algunos moviles el selector no
 * ensena el almacenamiento interno. Solo salen las de las ediciones que lleva
 * el APK (buscarCompatibles). Necesita el acceso a todos los archivos
 * (Partidas.accesoTotal); sin el, no ve nada fuera de la app.
 *
 * Se mira hasta PROFUNDIDAD_MAX carpetas por debajo de cada raiz, sin Android/
 * (data y obb: ni se puede entrar ni hay ISO) ni las carpetas ocultas, y con un
 * tope de carpetas y de tiempo para que un movil con muchos archivos no se
 * quede buscando.
 */
final class BuscadorIsos {

    private static final int PROFUNDIDAD_MAX = 6;
    private static final int CARPETAS_MAX = 20_000;
    private static final long TIEMPO_MAX_MS = 8_000;
    // Menos que esto no es una ISO de Xbox 360 (la de NFS MW pasa de 6 GB).
    private static final long TAMANO_MIN = 100L * 1024 * 1024;

    private BuscadorIsos() {
    }

    /** Las raices de cada almacenamiento: el interno y las tarjetas SD montadas. */
    static List<File> raices(Context ctx) {
        Set<File> raices = new LinkedHashSet<>();
        raices.add(Environment.getExternalStorageDirectory());
        // getExternalFilesDirs da <raiz>/Android/data/<paquete>/files de cada volumen.
        for (File f : ctx.getExternalFilesDirs(null)) {
            if (f == null) continue;
            String ruta = f.getAbsolutePath();
            int android = ruta.indexOf("/Android/data/");
            if (android > 0) raices.add(new File(ruta.substring(0, android)));
        }
        return new ArrayList<>(raices);
    }

    /** Una ISO que vale: de una edicion que lleva el APK. */
    static final class Encontrada {
        final File iso;
        final Ediciones.Edicion edicion;
        /** Lo que IsoXex leyo de ella: se guarda al elegirla y no hace falta mirarla otra vez. */
        final IsoXex.Info info;

        Encontrada(File iso, Ediciones.Edicion edicion, IsoXex.Info info) {
            this.iso = iso;
            this.edicion = edicion;
            this.info = info;
        }
    }

    /** Lo que encontro buscarCompatibles: las que valen y cuantas .iso no. */
    static final class Busqueda {
        final List<Encontrada> compatibles = new ArrayList<>();
        int descartadas;
    }

    /**
     * Las .iso de las ediciones del juego que lleva el APK: se lee el SHA-256 de
     * su default.xex (IsoXex), como al elegir una. Las de otros juegos, las que no
     * son de Xbox 360 y las de ediciones que el APK no trae se descartan. Si el
     * APK no sabe el SHA-256 de su edicion, vale cualquier ISO de Xbox 360 (como
     * Ediciones.ajena, que entonces no avisa).
     */
    static Busqueda buscarCompatibles(Context ctx) {
        Busqueda b = new Busqueda();
        boolean conHuella = !Ediciones.principal().xex.isEmpty();
        for (File iso : buscar(ctx)) {
            IsoXex.Info info = IsoXex.mirar(ctx, Uri.fromFile(iso));
            String xex = info != null ? info.xex : "";
            Ediciones.Edicion e = Ediciones.deXex(xex);
            if (e == null && !conHuella && !xex.isEmpty() && !IsoXex.NO_ES_ISO.equals(xex)) {
                e = Ediciones.principal();
            }
            if (e != null) {
                b.compatibles.add(new Encontrada(iso, e, info));
            } else {
                b.descartadas++;
            }
        }
        return b;
    }

    /** Las .iso encontradas, por nombre. */
    static List<File> buscar(Context ctx) {
        List<File> isos = new ArrayList<>();
        long limite = System.currentTimeMillis() + TIEMPO_MAX_MS;
        int[] carpetas = {0};
        for (File raiz : raices(ctx)) {
            recorrer(raiz, 0, isos, carpetas, limite);
        }
        isos.sort((a, b) -> a.getName().compareToIgnoreCase(b.getName()));
        return isos;
    }

    private static void recorrer(File carpeta, int profundidad, List<File> isos, int[] carpetas, long limite) {
        if (profundidad > PROFUNDIDAD_MAX || carpetas[0]++ > CARPETAS_MAX || System.currentTimeMillis() > limite) {
            return;
        }
        File[] hijos = carpeta.listFiles();
        if (hijos == null) return;
        for (File h : hijos) {
            String nombre = h.getName();
            if (nombre.startsWith(".")) continue;
            if (h.isDirectory()) {
                if (profundidad == 0 && nombre.equals("Android")) continue;
                recorrer(h, profundidad + 1, isos, carpetas, limite);
            } else if (nombre.toLowerCase(Locale.ROOT).endsWith(".iso") && h.length() >= TAMANO_MIN) {
                isos.add(h);
            }
        }
    }

    /** La carpeta de una ISO como la ve el jugador: sin /storage/emulated/0 delante. */
    static String carpetaVisible(File iso) {
        String carpeta = iso.getParent();
        if (carpeta == null) return "";
        String interno = Environment.getExternalStorageDirectory().getAbsolutePath();
        if (carpeta.equals(interno)) return "/";
        if (carpeta.startsWith(interno + "/")) return carpeta.substring(interno.length() + 1);
        return carpeta;
    }
}
