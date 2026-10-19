package io.github.nfsmwrecomp;

import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Busca servidores de la LAN (nfsmw-server-lan o un movil anfitrion,
 * ServidorEa): un datagrama "NFSMW_DISCOVER" al UDP 10902 (el mismo numero que
 * la comprobacion de gamertag, que es TCP) y cada servidor contesta
 *
 *     NFSMW_SERVER
 *     NAME=<nombre>
 *     PLAYERS=<jugadores en Xbox Live>
 *
 * La pregunta va por broadcast, tres veces (es UDP), y ademas una vez a cada IP
 * de la /24 de la Wi-Fi o del punto de acceso: hay routers y moviles que se
 * comen los broadcast. Solo las redes locales, nunca los datos moviles.
 * Bloquea el plazo entero (1,5 s, mas si las preguntas tardan en salir):
 * llamarlo fuera del hilo principal.
 */
final class BuscadorServidores {
    static final int PUERTO = ComprobacionGamertag.PUERTO;
    static final String PREGUNTA = "NFSMW_DISCOVER";
    private static final String RESPUESTA = "NFSMW_SERVER";
    private static final int PLAZO_MS = 1500;
    // Cuando se repite el broadcast, en ms desde el principio.
    private static final int[] RONDAS_MS = {0, 300, 700};
    // Lo que se sigue escuchando tras la ultima pregunta a la /24, si acaba tarde.
    private static final int ULTIMA_MS = 400;
    // Las preguntas a IP sin nadie se quedan en el socket esperando al ARP (~1 s):
    // con el bufer normal, la /24 entera tarda 2-3 s en salir.
    private static final int BUFER_ENVIO = 1 << 20;

    static final class Encontrado {
        final String ip;
        final String nombre;
        final int jugadores;

        Encontrado(String ip, String nombre, int jugadores) {
            this.ip = ip;
            this.nombre = nombre;
            this.jugadores = jugadores;
        }
    }

    private BuscadorServidores() {}

    /** Lo que contesta el servidor (ServidorEa). */
    static byte[] respuesta(String nombre, int jugadores) {
        return (RESPUESTA + "\nNAME=" + nombre + "\nPLAYERS=" + jugadores + "\n")
                .getBytes(StandardCharsets.UTF_8);
    }

    static boolean esPregunta(DatagramPacket p) {
        return new String(p.getData(), p.getOffset(), p.getLength(), StandardCharsets.ISO_8859_1)
                .startsWith(PREGUNTA);
    }

    /** Los que contestan, por orden de llegada. null si este movil no esta en ninguna red local. */
    static List<Encontrado> buscar() {
        Set<InetAddress> propias = new LinkedHashSet<>();
        Set<InetAddress> difusion = new LinkedHashSet<>();
        Set<InetAddress> red = new LinkedHashSet<>();
        destinos(propias, difusion, red);
        if (propias.isEmpty()) {
            return null;
        }
        Map<String, Encontrado> encontrados = new LinkedHashMap<>();
        byte[] pregunta = (PREGUNTA + "\n").getBytes(StandardCharsets.ISO_8859_1);
        try (DatagramSocket s = new DatagramSocket()) {
            s.setBroadcast(true);
            long inicio = System.nanoTime();
            s.setSendBufferSize(BUFER_ENVIO);
            // Las preguntas, en sus hilos (enviar puede bloquear) y las respuestas
            // aqui mientras tanto. Se escucha hasta el plazo y, si la /24 acaba de
            // salir despues, hasta un poco despues.
            Thread rondas = enviar(() -> {
                for (int espera : RONDAS_MS) {
                    long falta = espera - (System.nanoTime() - inicio) / 1_000_000;
                    if (falta > 0) {
                        Thread.sleep(falta);
                    }
                    preguntar(s, pregunta, difusion);
                }
                return null;
            });
            Thread barrido = enviar(() -> {
                preguntar(s, pregunta, red);
                return null;
            });
            long finEnvio = -1;
            byte[] buf = new byte[512];
            while (true) {
                long pasado = (System.nanoTime() - inicio) / 1_000_000;
                if (finEnvio < 0 && !rondas.isAlive() && !barrido.isAlive()) {
                    finEnvio = pasado;
                }
                long plazo = finEnvio < 0 ? Long.MAX_VALUE : Math.max(PLAZO_MS, finEnvio + ULTIMA_MS);
                if (pasado >= plazo) {
                    break;
                }
                s.setSoTimeout((int) Math.max(1, Math.min(100, plazo - pasado)));
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                try {
                    s.receive(p);
                } catch (SocketTimeoutException e) {
                    continue;
                }
                if (propias.contains(p.getAddress())) {
                    continue;
                }
                Encontrado e = leer(p);
                if (e != null) {
                    encontrados.putIfAbsent(e.ip, e);
                }
            }
        } catch (IOException e) {
            // Sin socket no hay busqueda: lo que haya llegado.
        }
        return new ArrayList<>(encontrados.values());
    }

    private static Thread enviar(java.util.concurrent.Callable<Void> tarea) {
        Thread t = new Thread(() -> {
            try {
                tarea.call();
            } catch (Exception ignored) {
                // Socket cerrado o hilo interrumpido: se acabo la busqueda.
            }
        }, "buscar-servidores-envio");
        t.setDaemon(true);
        t.start();
        return t;
    }

    private static void preguntar(DatagramSocket s, byte[] pregunta, Set<InetAddress> destinos) {
        for (InetAddress a : destinos) {
            if (s.isClosed()) {
                return;
            }
            try {
                s.send(new DatagramPacket(pregunta, pregunta.length, a, PUERTO));
            } catch (IOException ignored) {
                // Un destino sin ruta: los demas siguen.
            }
        }
    }

    private static Encontrado leer(DatagramPacket p) {
        String[] lineas = new String(p.getData(), p.getOffset(), p.getLength(), StandardCharsets.UTF_8)
                .split("\n");
        if (lineas.length == 0 || !lineas[0].trim().equals(RESPUESTA)) {
            return null;
        }
        String nombre = "";
        int jugadores = 0;
        for (String l : lineas) {
            if (l.startsWith("NAME=")) {
                nombre = l.substring(5).trim();
            } else if (l.startsWith("PLAYERS=")) {
                try {
                    jugadores = Math.max(0, Integer.parseInt(l.substring(8).trim()));
                } catch (NumberFormatException ignored) {
                }
            }
        }
        return new Encontrado(p.getAddress().getHostAddress(), nombre, jugadores);
    }

    /**
     * Las IPv4 de este movil en redes locales (propias) y a donde preguntar: el
     * broadcast de cada red y 255.255.255.255 (difusion) y, en redes /24 o
     * menores, cada IP (red).
     */
    private static void destinos(Set<InetAddress> propias, Set<InetAddress> difusion, Set<InetAddress> red) {
        try {
            Enumeration<NetworkInterface> it = NetworkInterface.getNetworkInterfaces();
            if (it == null) {
                return;
            }
            for (NetworkInterface ni : Collections.list(it)) {
                if (!ni.isUp() || ni.isLoopback() || ServidorEa.puntosInterfaz(ni.getName()) < 0) {
                    continue;
                }
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    InetAddress a = ia.getAddress();
                    if (!(a instanceof Inet4Address) || !a.isSiteLocalAddress()) {
                        continue;
                    }
                    propias.add(a);
                    if (ia.getBroadcast() != null) {
                        difusion.add(ia.getBroadcast());
                    }
                    int prefijo = ia.getNetworkPrefixLength();
                    if (prefijo < 24 || prefijo > 30) {
                        continue;
                    }
                    byte[] b = a.getAddress();
                    int ip = (b[0] & 0xFF) << 24 | (b[1] & 0xFF) << 16 | (b[2] & 0xFF) << 8 | (b[3] & 0xFF);
                    int mascara = -1 << (32 - prefijo);
                    int base = ip & mascara;
                    for (int h = base + 1; h < (base | ~mascara); h++) {
                        if (h != ip) {
                            red.add(InetAddress.getByAddress(new byte[] {
                                    (byte) (h >>> 24), (byte) (h >>> 16), (byte) (h >>> 8), (byte) h}));
                        }
                    }
                }
            }
            if (!propias.isEmpty()) {
                difusion.add(InetAddress.getByName("255.255.255.255"));
            }
        } catch (IOException e) {
            // Lo que se haya podido mirar.
        }
    }
}
