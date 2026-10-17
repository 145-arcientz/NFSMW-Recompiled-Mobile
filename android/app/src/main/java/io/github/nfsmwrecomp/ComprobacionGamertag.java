package io.github.nfsmwrecomp;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.Reader;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;

/**
 * Pregunta al servidor de la LAN (nfsmw-server-lan, TCP 10902) si un gamertag
 * esta libre: una linea "GAMERTAG <nombre>" y contesta "FREE" o "IN_USE". En
 * uso = otro aparato esta conectado con ese nombre o lo tiene apartado; el
 * propio no cuenta. Un "FREE" aparta el nombre para este movil unos minutos (10
 * por defecto en el servidor), y soltarlo es pedir otro.
 * Va por un socket y no por HTTP porque Android 9+ no deja HTTP sin cifrar.
 * Bloquea: llamarlo fuera del hilo principal.
 */
final class ComprobacionGamertag {
    static final int PUERTO = 10902;
    private static final int PLAZO_MS = 2000;

    enum Resultado { LIBRE, EN_USO, SIN_RESPUESTA }

    private ComprobacionGamertag() {}

    /** El nombre que el juego usara: vacio es el de fabrica (user_gamertag). */
    static String efectivo(String gamertag) {
        String g = gamertag == null ? "" : gamertag.trim();
        return g.isEmpty() ? "User" : g;
    }

    static Resultado comprobar(String ip, String gamertag) {
        try (Socket s = new Socket()) {
            s.connect(new InetSocketAddress(ip, PUERTO), PLAZO_MS);
            s.setSoTimeout(PLAZO_MS);
            OutputStream salida = s.getOutputStream();
            salida.write(("GAMERTAG " + efectivo(gamertag) + "\n").getBytes(StandardCharsets.UTF_8));
            salida.flush();
            Reader r = new InputStreamReader(s.getInputStream(), StandardCharsets.UTF_8);
            String respuesta = new BufferedReader(r).readLine();
            if ("FREE".equals(respuesta)) {
                return Resultado.LIBRE;
            }
            if ("IN_USE".equals(respuesta)) {
                return Resultado.EN_USO;
            }
            // Un servidor sin esta comprobacion (version vieja) cierra o no
            // contesta: no se sabe, no se bloquea.
            return Resultado.SIN_RESPUESTA;
        } catch (IOException e) {
            return Resultado.SIN_RESPUESTA;
        }
    }
}
