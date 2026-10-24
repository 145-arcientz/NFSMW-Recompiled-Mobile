package io.github.nfsmwrecomp;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/**
 * Las partidas guardadas, en un .zip que se puede ver y llevar a otro sitio.
 *
 * El motor nativo guarda en el almacenamiento interno de la app
 * (files/nativo/usuario), donde no entran ni el explorador de archivos ni el
 * PC; el de Xenos, en Android/data/.../files/datos. En esa carpeta estan el
 * perfil y las partidas (&lt;xuid&gt;/&lt;title id&gt;/..., como en la consola), los
 * logros (achievements/) y las copias legibles de cada guardado
 * (saves/&lt;perfil&gt;/actual y anterior, content_backup_root en nfsmw_app.h).
 *
 * Exportar mete todo eso en un .zip donde diga el selector de Android, y
 * deja fuera lo que no son partidas: los ajustes del motor (nfsmw.toml), las
 * bibliotecas de shaders (*.nfsp), que pone la app, y las caches. Importar
 * sustituye las partidas de ahora por las del .zip; antes deja las de ahora en
 * partidas_antes_de_importar.zip.
 *
 * Con acceso a todos los archivos (accesoTotal), el .zip exportado y la copia
 * de antes de importar van a NFSMW/partidas del almacenamiento interno, sin el
 * selector de Android: en algunos moviles el selector no ensena ese
 * almacenamiento. Sin el, el .zip va donde diga el selector y la copia, a
 * Android/data/.../files. Importar es siempre con el selector.
 */
final class Partidas {

    /** La primera entrada del .zip: dice que es una copia de partidas de esta app. */
    static final String MARCA = "nfsmw_partidas.txt";
    /** Donde queda la copia de las partidas de antes de importar, en Android/data/.../files. */
    static final String ANTES_DE_IMPORTAR = "partidas_antes_de_importar.zip";
    // Hasta donde se baja por las carpetas (como MotorNativo.traerPartidas).
    private static final int PROFUNDIDAD_MAX = 16;

    /** Lo que se copio: archivos y bytes. */
    static final class Resultado {
        int archivos;
        long bytes;
    }

    /** El .zip no es una copia de partidas de esta app (no empieza por MARCA). */
    static final class NoValido extends IOException {
        NoValido() {
            super("sin " + MARCA);
        }
    }

    private Partidas() {
    }

    /** Donde guarda el motor de este APK. */
    static File carpeta(Context ctx) {
        return MotorNativo.ACTIVO ? MotorNativo.carpetaUsuario(ctx) : Ajustes.carpetaDatos(ctx);
    }

    /** Lo de la raiz de la carpeta que es de las partidas (no ajustes, shaders ni caches). */
    static boolean esPartida(String nombre) {
        String n = nombre.toLowerCase(Locale.ROOT);
        return !n.equals("nfsmw.toml") && !n.endsWith(".nfsp") && !n.startsWith("cache")
                && !n.equals(MARCA) && !n.startsWith(".");
    }

    /** Si hay algun archivo que exportar. */
    static boolean hay(Context ctx) {
        File[] hijos = carpeta(ctx).listFiles();
        if (hijos == null) return false;
        for (File h : hijos) {
            if (esPartida(h.getName()) && tieneArchivos(h, 0)) return true;
        }
        return false;
    }

    private static boolean tieneArchivos(File f, int profundidad) {
        if (f.isFile()) return true;
        File[] hijos = f.listFiles();
        if (hijos == null || profundidad > PROFUNDIDAD_MAX) return false;
        for (File h : hijos) {
            if (tieneArchivos(h, profundidad + 1)) return true;
        }
        return false;
    }

    /** Lo que abre el .zip que se importa. */
    interface Abrir {
        InputStream abrir() throws IOException;
    }

    /** NFSMW/partidas del almacenamiento interno, tal como lo ensena el explorador. */
    static final String RUTA_VISIBLE = "NFSMW/partidas";

    static File carpetaVisible() {
        return new File(Environment.getExternalStorageDirectory(), RUTA_VISIBLE);
    }

    /**
     * Si la app puede escribir en el almacenamiento interno fuera de su carpeta:
     * en Android 11+, el acceso a todos los archivos; en 8-10, el permiso de
     * almacenamiento de antes.
     */
    static boolean accesoTotal(Context ctx) {
        if (Build.VERSION.SDK_INT >= 30) {
            return Environment.isExternalStorageManager();
        }
        return ctx.checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                == PackageManager.PERMISSION_GRANTED;
    }

    /**
     * Donde queda la copia de las partidas de antes de importar: con acceso
     * total, en NFSMW/partidas; si no, en Android/data/.../files (sin
     * almacenamiento externo, junto a la carpeta de las partidas).
     */
    static File copiaAntesDeImportar(Context ctx) {
        if (accesoTotal(ctx)) {
            return new File(carpetaVisible(), ANTES_DE_IMPORTAR);
        }
        File externa = ctx.getExternalFilesDir(null);
        return new File(externa != null ? externa : carpeta(ctx).getParentFile(), ANTES_DE_IMPORTAR);
    }

    /** La ruta de esa copia como la ve el jugador. */
    static String rutaCopiaAntesDeImportar(Context ctx) {
        return accesoTotal(ctx) ? RUTA_VISIBLE + "/" + ANTES_DE_IMPORTAR
                : "Android/data/" + ctx.getPackageName() + "/files/" + ANTES_DE_IMPORTAR;
    }

    /** El nombre que se propone al exportar: nfsmw_partidas_2026-10-24_1530.zip. */
    static String nombreSugerido() {
        return "nfsmw_partidas_" + new SimpleDateFormat("yyyy-MM-dd_HHmm", Locale.ROOT).format(new Date()) + ".zip";
    }

    /** Exporta al documento que creo el selector de Android. */
    static Resultado exportar(Context ctx, Uri destino) throws IOException {
        try (OutputStream out = ctx.getContentResolver().openOutputStream(destino, "wt")) {
            if (out == null) throw new IOException("no se puede escribir " + destino);
            return exportar(ctx, out);
        }
    }

    /** Exporta a un archivo (NFSMW/partidas/...); si falla, no deja uno a medias. */
    static Resultado exportar(Context ctx, File destino) throws IOException {
        File padre = destino.getParentFile();
        if (padre != null && !padre.isDirectory() && !padre.mkdirs()) {
            throw new IOException("no se puede crear " + padre);
        }
        boolean bien = false;
        try (OutputStream out = new FileOutputStream(destino)) {
            Resultado r = exportar(ctx, out);
            bien = true;
            return r;
        } finally {
            if (!bien) {
                //noinspection ResultOfMethodCallIgnored
                destino.delete();
            }
        }
    }

    static Resultado exportar(Context ctx, OutputStream out) throws IOException {
        Resultado r = new Resultado();
        ZipOutputStream zip = new ZipOutputStream(out);
        zip.putNextEntry(new ZipEntry(MARCA));
        String marca = "NFS Most Wanted Recompiled: partidas exportadas el "
                + new SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.ROOT).format(new Date()) + "\n"
                + "motor=" + (MotorNativo.ACTIVO ? "nativo" : "xenos") + "\n"
                + "version=" + BuildConfig.VERSION_NAME + "\n";
        zip.write(marca.getBytes(StandardCharsets.UTF_8));
        zip.closeEntry();
        File[] hijos = carpeta(ctx).listFiles();
        if (hijos != null) {
            for (File h : hijos) {
                if (esPartida(h.getName())) {
                    anadir(zip, h, h.getName(), 0, r);
                }
            }
        }
        zip.finish();
        return r;
    }

    private static void anadir(ZipOutputStream zip, File f, String ruta, int profundidad, Resultado r)
            throws IOException {
        if (f.isDirectory()) {
            File[] hijos = f.listFiles();
            if (hijos == null || profundidad > PROFUNDIDAD_MAX) return;
            for (File h : hijos) {
                anadir(zip, h, ruta + "/" + h.getName(), profundidad + 1, r);
            }
            return;
        }
        ZipEntry e = new ZipEntry(ruta);
        e.setTime(f.lastModified());
        zip.putNextEntry(e);
        try (InputStream in = new FileInputStream(f)) {
            r.bytes += volcar(in, zip);
        }
        zip.closeEntry();
        r.archivos++;
    }

    /**
     * Importa el .zip que eligio el selector de Android. Primero lo descomprime
     * aparte (si el .zip esta roto, las partidas de ahora no se tocan), luego
     * guarda las de ahora en ANTES_DE_IMPORTAR y al final cambia unas por otras.
     */
    static Resultado importar(Context ctx, Uri origen) throws IOException {
        return importar(ctx, () -> {
            InputStream in = ctx.getContentResolver().openInputStream(origen);
            if (in == null) throw new IOException("no se puede leer " + origen);
            return in;
        });
    }

    static Resultado importar(Context ctx, Abrir abrir) throws IOException {
        File carpeta = carpeta(ctx);
        File temporal = new File(carpeta.getParentFile(), carpeta.getName() + ".importando");
        borrar(temporal, 0);
        if (!temporal.mkdirs()) throw new IOException("no se puede crear " + temporal);
        try {
            Resultado r = descomprimir(abrir, temporal);
            exportar(ctx, copiaAntesDeImportar(ctx));
            File[] actuales = carpeta.listFiles();
            if (actuales != null) {
                for (File h : actuales) {
                    if (esPartida(h.getName())) borrar(h, 0);
                }
            }
            File[] nuevas = temporal.listFiles();
            if (nuevas != null) {
                for (File h : nuevas) {
                    if (!h.renameTo(new File(carpeta, h.getName()))) {
                        throw new IOException("no se puede mover " + h.getName());
                    }
                }
            }
            return r;
        } finally {
            borrar(temporal, 0);
        }
    }

    private static Resultado descomprimir(Abrir abrir, File destino) throws IOException {
        Resultado r = new Resultado();
        String raiz = destino.getCanonicalPath() + File.separator;
        try (InputStream in = abrir.abrir()) {
            ZipInputStream zip = new ZipInputStream(in);
            ZipEntry e = zip.getNextEntry();
            if (e == null || !MARCA.equals(e.getName())) throw new NoValido();
            while ((e = zip.getNextEntry()) != null) {
                String nombre = e.getName().replace('\\', '/');
                int barra = nombre.indexOf('/');
                String primero = barra < 0 ? nombre : nombre.substring(0, barra);
                if (e.isDirectory() || !esPartida(primero)) continue;
                File f = new File(destino, nombre);
                // Nada fuera de la carpeta (rutas con .. o absolutas).
                if (!f.getCanonicalPath().startsWith(raiz)) throw new NoValido();
                File padre = f.getParentFile();
                if (padre != null && !padre.isDirectory() && !padre.mkdirs()) {
                    throw new IOException("no se puede crear " + padre);
                }
                try (OutputStream out = new FileOutputStream(f)) {
                    r.bytes += volcar(zip, out);
                }
                if (e.getTime() > 0) {
                    //noinspection ResultOfMethodCallIgnored
                    f.setLastModified(e.getTime());
                }
                r.archivos++;
            }
        }
        return r;
    }

    private static long volcar(InputStream in, OutputStream out) throws IOException {
        byte[] b = new byte[64 * 1024];
        long total = 0;
        int n;
        while ((n = in.read(b)) > 0) {
            out.write(b, 0, n);
            total += n;
        }
        return total;
    }

    private static void borrar(File f, int profundidad) {
        if (f.isDirectory() && profundidad <= PROFUNDIDAD_MAX) {
            File[] hijos = f.listFiles();
            if (hijos != null) {
                for (File h : hijos) borrar(h, profundidad + 1);
            }
        }
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }
}
