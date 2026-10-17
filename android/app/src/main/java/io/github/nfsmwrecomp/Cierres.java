package io.github.nfsmwrecomp;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * Por que se cerro el juego la ultima vez, sin adb. El juego va en su proceso
 * (":juego"); cuando muere, Android guarda el motivo (ApplicationExitInfo,
 * Android 11+) y, si fue un fallo nativo, el informe del fallo (el tombstone).
 * Al volver a la pantalla de inicio se apunta en files/logs/cierre.txt, con las
 * cadenas legibles del tombstone (funciones, bibliotecas, mensaje de abort), y
 * el tombstone entero en files/logs/cierre_&lt;hora&gt;.trace.
 *
 * El APK normal no escribe el log del juego: sin esto, un movil que se queda en
 * negro y se cierra (el Galaxy S21 con Snapdragon 888) no deja nada que mirar.
 */
final class Cierres {
    private static final String PREF = "cierre_visto";
    /** Lo que se copia del tombstone a cierre.txt (el .trace va entero). */
    private static final int MAX_CADENAS = 400;

    private Cierres() {}

    /** En segundo plano: leer los motivos y escribir el fichero es E/S. */
    static void guardar(Context ctx) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return;
        final Context app = ctx.getApplicationContext();
        new Thread(() -> {
            try {
                guardarYa(app);
            } catch (RuntimeException ignorada) {
                // Solo es diagnostico.
            }
        }, "cierres").start();
    }

    private static void guardarYa(Context ctx) {
        ActivityManager am = (ActivityManager) ctx.getSystemService(Context.ACTIVITY_SERVICE);
        if (am == null) return;
        List<ApplicationExitInfo> lista = am.getHistoricalProcessExitReasons(ctx.getPackageName(), 0, 10);
        SharedPreferences p = ctx.getSharedPreferences("cierres", Context.MODE_PRIVATE);
        long visto = p.getLong(PREF, 0);
        long nuevo = visto;
        File carpeta = ctx.getExternalFilesDir("logs");
        if (carpeta == null) return;
        // La lista va de mas nuevo a mas viejo: el primero del juego que no se
        // haya visto es el ultimo cierre.
        for (ApplicationExitInfo i : lista) {
            if (i.getTimestamp() <= visto) break;
            nuevo = Math.max(nuevo, i.getTimestamp());
            String proceso = i.getProcessName();
            if (proceso == null || !proceso.endsWith(":juego")) continue;
            escribir(carpeta, i);
            break;
        }
        if (nuevo != visto) p.edit().putLong(PREF, nuevo).apply();
    }

    private static void escribir(File carpeta, ApplicationExitInfo i) {
        String hora = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.ROOT).format(new Date(i.getTimestamp()));
        StringBuilder t = new StringBuilder();
        t.append("NFSMW Recompiled - ultimo cierre del juego\n");
        t.append("Cuando: ").append(hora).append('\n');
        t.append("Motivo: ").append(motivo(i.getReason())).append(" (").append(i.getReason()).append(")\n");
        t.append("Estado: ").append(i.getStatus());
        if (i.getReason() == ApplicationExitInfo.REASON_SIGNALED
                || i.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE) {
            t.append(" (senal)");
        }
        t.append('\n');
        if (i.getDescription() != null) t.append("Descripcion: ").append(i.getDescription()).append('\n');
        t.append("Memoria al morir: PSS ").append(i.getPss() / 1024).append(" MB, RSS ")
                .append(i.getRss() / 1024).append(" MB\n");
        t.append("Movil: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
                .append(" (").append(Build.DEVICE).append("), Android API ").append(Build.VERSION.SDK_INT)
                .append(", SoC ")
                .append(Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ? Build.SOC_MODEL : Build.HARDWARE)
                .append('\n');
        t.append("Version de la app: ").append(BuildConfig.VERSION_NAME).append(", motor ")
                .append(BuildConfig.MOTOR).append('\n');

        byte[] traza = leerTraza(i);
        if (traza != null && traza.length > 0) {
            String nombre = "cierre_" + new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.ROOT)
                    .format(new Date(i.getTimestamp())) + ".trace";
            try (FileOutputStream f = new FileOutputStream(new File(carpeta, nombre))) {
                f.write(traza);
                t.append("Informe completo: ").append(nombre).append(" (").append(traza.length).append(" bytes)\n");
            } catch (IOException e) {
                t.append("No se pudo guardar el informe: ").append(e.getMessage()).append('\n');
            }
            // El tombstone de Android 12+ es un protobuf: sus cadenas (mensaje
            // de abort, registros, funciones y bibliotecas de la pila) se leen
            // tal cual. En Android 11 y en los ANR ya es texto.
            t.append("\n--- cadenas del informe ---\n");
            t.append(cadenas(traza));
        } else {
            t.append("\n(Android no guardo informe del fallo para este cierre)\n");
        }

        try (FileWriter f = new FileWriter(new File(carpeta, "cierre.txt"), false)) {
            f.write(t.toString());
        } catch (IOException ignorada) {
            // Solo es diagnostico.
        }
    }

    private static byte[] leerTraza(ApplicationExitInfo i) {
        try (InputStream in = i.getTraceInputStream()) {
            if (in == null) return null;
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            byte[] buf = new byte[16384];
            int n;
            while ((n = in.read(buf)) > 0) b.write(buf, 0, n);
            return b.toByteArray();
        } catch (IOException | RuntimeException e) {
            return null;
        }
    }

    /** Las secuencias imprimibles de 4 o mas caracteres, una por linea. */
    private static String cadenas(byte[] datos) {
        StringBuilder salida = new StringBuilder();
        StringBuilder actual = new StringBuilder();
        int lineas = 0;
        for (int k = 0; k <= datos.length && lineas < MAX_CADENAS; k++) {
            int c = k < datos.length ? datos[k] & 0xFF : 0;
            if (c >= 0x20 && c < 0x7F) {
                actual.append((char) c);
                continue;
            }
            if (actual.length() >= 4) {
                salida.append(actual).append('\n');
                lineas++;
            }
            actual.setLength(0);
        }
        if (lineas >= MAX_CADENAS) salida.append("... (sigue en el .trace)\n");
        return salida.toString();
    }

    private static String motivo(int r) {
        switch (r) {
            case ApplicationExitInfo.REASON_EXIT_SELF: return "salio el mismo";
            case ApplicationExitInfo.REASON_SIGNALED: return "matado por una senal";
            case ApplicationExitInfo.REASON_LOW_MEMORY: return "falta de memoria (el sistema lo cerro)";
            case ApplicationExitInfo.REASON_CRASH: return "fallo en Java";
            case ApplicationExitInfo.REASON_CRASH_NATIVE: return "fallo nativo";
            case ApplicationExitInfo.REASON_ANR: return "no respondia (ANR)";
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: return "fallo al arrancar";
            case ApplicationExitInfo.REASON_PERMISSION_CHANGE: return "cambio de permisos";
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "uso excesivo de recursos";
            case ApplicationExitInfo.REASON_USER_REQUESTED: return "lo pidio el usuario";
            case ApplicationExitInfo.REASON_USER_STOPPED: return "lo paro el usuario";
            case ApplicationExitInfo.REASON_DEPENDENCY_DIED: return "murio algo de lo que dependia";
            case ApplicationExitInfo.REASON_OTHER: return "otro";
            default: return "desconocido";
        }
    }
}
