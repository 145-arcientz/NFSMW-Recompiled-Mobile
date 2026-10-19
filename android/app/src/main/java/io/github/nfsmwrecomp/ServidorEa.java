package io.github.nfsmwrecomp;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.NetworkInterface;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketException;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.SecureRandom;
import java.time.LocalDate;
import java.time.LocalDateTime;
import java.time.format.DateTimeFormatter;
import java.time.temporal.IsoFields;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;
import java.util.Collections;
import java.util.Enumeration;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;

/**
 * El servidor de salas de EA de la LAN dentro de la app: lo mismo que
 * ea_server.py del proyecto nfsmw-server-lan (version del commit 9ebeb11,
 * 2026-10-16), para que un movil haga de anfitrion sin PC. Un cambio de
 * protocolo hay que hacerlo en los dos. El protocolo y lo que costo cada
 * mensaje estan en el README de ese proyecto y en
 * docs/diario/red-y-privilegios.md; aqui solo lo que cambia al pasarlo a Java:
 *  - Hilos y no asyncio: uno por conexion y un candado (el propio servidor)
 *    para todo lo que toca salas, partidas y sesiones. Las respuestas se
 *    escriben dentro del candado, asi que en cada socket salen en el mismo
 *    orden que en el de Python.
 *  - La IP del directorio (@dir ADDR) y del amigo (news) es la de la conexion
 *    por la que llega cada cliente, no una fija: el movil puede estar a la vez
 *    en una Wi-Fi y dando punto de acceso.
 *  - Estadisticas (stats.json, races.jsonl) y registro en la carpeta que se le
 *    pase, con el mismo formato.
 *  - Contesta a la busqueda de servidores de la app (BuscadorServidores, UDP
 *    10902) con el nombre que se le pase, como Discovery en Python.
 * Lo arranca y lo para ServidorLanService.
 */
final class ServidorEa {
    static final int PUERTO_DIRECTORIO = 30960;
    static final int PUERTO_SALAS = 10901;
    // No es de EA: la app pregunta aqui si un gamertag esta libre
    // (ComprobacionGamertag).
    static final int PUERTO_GAMERTAG = ComprobacionGamertag.PUERTO;
    // Lo que un gamertag comprobado queda apartado para el movil que pregunto.
    private static final long APARTADO_MS = 10 * 60 * 1000;
    private static final long REGISTRO_MAX = 4L << 20;

    private static final DateTimeFormatter FECHA = DateTimeFormatter.ofPattern("yyyy.MM.dd-HH:mm:ss");
    private static final DateTimeFormatter HORA = DateTimeFormatter.ofPattern("HH:mm:ss.SSS");
    private static final SecureRandom AZAR = new SecureRandom();

    private final File datos;
    private final Writer registro;
    private final Runnable alCambiar;
    private final Supplier<String> nombre;
    private final List<ServerSocket> escuchas = new ArrayList<>();
    private DatagramSocket busqueda;
    private final ScheduledExecutorService latido = Executors.newSingleThreadScheduledExecutor();
    private volatile boolean parado;

    private final Map<Integer, Sala> salas = new LinkedHashMap<>();
    private final Map<String, Partida> partidas = new LinkedHashMap<>();
    private final Set<Sesion> sesiones = new LinkedHashSet<>();
    private int siguienteId = 1000;
    // Gamertags apartados: nombre en minusculas -> (IP, fin). Uno por IP.
    private final Map<String, Apartado> apartados = new LinkedHashMap<>();
    private final Estadisticas estadisticas;

    /**
     * datos: donde van stats.json y races.jsonl. logs: la carpeta del registro.
     * alCambiar: se llama (desde cualquier hilo) cuando entra o sale un jugador.
     * nombre: el que ve quien busca servidores; se pide en cada busqueda.
     */
    ServidorEa(File datos, File logs, Runnable alCambiar, Supplier<String> nombre) throws IOException {
        this.datos = datos;
        this.alCambiar = alCambiar;
        this.nombre = nombre;
        //noinspection ResultOfMethodCallIgnored
        datos.mkdirs();
        //noinspection ResultOfMethodCallIgnored
        logs.mkdirs();
        File f = new File(logs, "servidor_lan.log");
        // Cada mensaje va al registro: que no crezca sin fin en el movil.
        boolean seguir = f.length() < REGISTRO_MAX;
        registro = new OutputStreamWriter(new FileOutputStream(f, seguir), StandardCharsets.UTF_8);
        estadisticas = new Estadisticas(datos);
    }

    /** Abre los cuatro puertos (falla si alguno esta cogido) y empieza a atender. */
    void arrancar() throws IOException {
        log("============================================================");
        try {
            for (int puerto : new int[] {PUERTO_DIRECTORIO, PUERTO_SALAS, PUERTO_GAMERTAG}) {
                ServerSocket s = new ServerSocket();
                s.setReuseAddress(true);
                // En IPv4: con el comodin de IPv6 las IP de los clientes saldrian
                // como ::ffff:a.b.c.d y no casarian con las del juego.
                s.bind(new InetSocketAddress(InetAddress.getByName("0.0.0.0"), puerto));
                escuchas.add(s);
                Thread t = new Thread(() -> aceptar(s, puerto), "servidor-ea-" + puerto);
                t.setDaemon(true);
                t.start();
            }
            DatagramSocket d = new DatagramSocket(null);
            d.setReuseAddress(true);
            d.bind(new InetSocketAddress(InetAddress.getByName("0.0.0.0"), BuscadorServidores.PUERTO));
            busqueda = d;
            Thread t = new Thread(() -> contestarBusquedas(d), "servidor-ea-busqueda");
            t.setDaemon(true);
            t.start();
        } catch (IOException e) {
            parar();
            throw e;
        }
        // El servidor de salas manda ~png cada poco; sin trafico, el cliente da la
        // conexion por muerta a los 60 s y vuelve al menu.
        latido.scheduleWithFixedDelay(this::latir, 20, 20, TimeUnit.SECONDS);
        log("EA server: directory " + PUERTO_DIRECTORIO + ", lobby " + PUERTO_SALAS
                + ", gamertag check " + PUERTO_GAMERTAG + ", LAN search " + BuscadorServidores.PUERTO + "/udp");
    }

    void parar() {
        if (parado) {
            return;
        }
        parado = true;
        latido.shutdownNow();
        for (ServerSocket s : escuchas) {
            cerrar(s);
        }
        if (busqueda != null) {
            busqueda.close();
        }
        List<Sesion> todas;
        synchronized (this) {
            todas = new ArrayList<>(sesiones);
        }
        for (Sesion s : todas) {
            cerrar(s.socket);
        }
        log("stopped");
        synchronized (registro) {
            try {
                registro.close();
            } catch (IOException ignored) {
            }
        }
    }

    /** Jugadores dentro de Xbox Live (conectados al servidor de salas y autenticados). */
    synchronized int jugadores() {
        int n = 0;
        for (Sesion s : sesiones) {
            if (s.puerto == PUERTO_SALAS && s.autenticado) {
                n++;
            }
        }
        return n;
    }

    /**
     * La IP de este movil en la red local, la que tienen que poner los demas:
     * la de la Wi-Fi o la del punto de acceso. Nunca la de los datos moviles
     * (rmnet, ccmni: suelen ser 10.x del operador). null si no hay ninguna.
     */
    static String ipLocal() {
        String mejor = null;
        int mejorPuntos = -1;
        try {
            Enumeration<NetworkInterface> it = NetworkInterface.getNetworkInterfaces();
            if (it == null) {
                return null;
            }
            for (NetworkInterface ni : Collections.list(it)) {
                if (!ni.isUp() || ni.isLoopback()) {
                    continue;
                }
                int puntos = puntosInterfaz(ni.getName());
                if (puntos < 0) {
                    continue;
                }
                for (InetAddress a : Collections.list(ni.getInetAddresses())) {
                    if (a instanceof Inet4Address && a.isSiteLocalAddress() && puntos > mejorPuntos) {
                        mejor = a.getHostAddress();
                        mejorPuntos = puntos;
                    }
                }
            }
        } catch (SocketException e) {
            return mejor;
        }
        return mejor;
    }

    /**
     * Cuanto vale una interfaz como red local, por su nombre: la Wi-Fi 3, el punto
     * de acceso 2, cable 1, otras 0; -1 = no es la LAN (datos moviles, VPN).
     */
    static int puntosInterfaz(String nombre) {
        String n = nombre.toLowerCase(Locale.ROOT);
        return n.startsWith("wlan") ? 3
                : n.startsWith("swlan") || n.startsWith("ap") || n.startsWith("softap") ? 2
                : n.startsWith("eth") ? 1
                : n.startsWith("rmnet") || n.startsWith("ccmni") || n.startsWith("v4-")
                        || n.startsWith("tun") || n.startsWith("dummy") ? -1
                : 0;
    }

    // ---------------------------------------------------------------------
    // Conexiones

    /** La busqueda de servidores (BuscadorServidores): a cada pregunta, nombre y jugadores. */
    private void contestarBusquedas(DatagramSocket d) {
        byte[] buf = new byte[512];
        // Cada busqueda llega varias veces (broadcast y directa, tres rondas): al
        // registro, una vez por IP cada 10 s.
        Map<InetAddress, Long> anotadas = new LinkedHashMap<>();
        while (!parado) {
            DatagramPacket p = new DatagramPacket(buf, buf.length);
            try {
                d.receive(p);
            } catch (IOException e) {
                if (!parado) {
                    log("[" + BuscadorServidores.PUERTO + "/udp] " + e);
                }
                return;
            }
            if (!BuscadorServidores.esPregunta(p)) {
                continue;
            }
            String n = nombre.get();
            int j = jugadores();
            byte[] r = BuscadorServidores.respuesta(n, j);
            try {
                d.send(new DatagramPacket(r, r.length, p.getSocketAddress()));
            } catch (IOException e) {
                continue;
            }
            long ahora = System.nanoTime();
            Long antes = anotadas.get(p.getAddress());
            if (antes != null && ahora - antes < TimeUnit.SECONDS.toNanos(10)) {
                continue;
            }
            anotadas.put(p.getAddress(), ahora);
            log("[" + BuscadorServidores.PUERTO + "/udp] search from " + p.getAddress().getHostAddress()
                    + ": answered '" + n + "', " + j + " players");
        }
    }

    private void aceptar(ServerSocket escucha, int puerto) {
        while (!parado) {
            Socket s;
            try {
                s = escucha.accept();
            } catch (IOException e) {
                if (!parado) {
                    log("[" + puerto + "] accept: " + e);
                }
                return;
            }
            Thread t = new Thread(() -> {
                if (puerto == PUERTO_GAMERTAG) {
                    comprobarGamertag(s);
                } else {
                    atender(s, puerto);
                }
            }, "servidor-ea-cliente");
            t.setDaemon(true);
            t.start();
        }
    }

    private void atender(Socket socket, int puerto) {
        Sesion ses;
        try {
            socket.setTcpNoDelay(true);
            ses = new Sesion(socket, puerto);
        } catch (IOException e) {
            cerrar(socket);
            return;
        }
        String par = ses.ip + ":" + socket.getPort();
        synchronized (this) {
            sesiones.add(ses);
        }
        log("[" + puerto + "] connection from " + par);
        byte[] buffer = new byte[0];
        byte[] trozo = new byte[65536];
        try {
            InputStream entrada = socket.getInputStream();
            while (true) {
                int n = entrada.read(trozo);
                if (n < 0) {
                    break;
                }
                buffer = juntar(buffer, trozo, n);
                while (buffer.length >= 12) {
                    long largo = ((buffer[8] & 0xFFL) << 24) | ((buffer[9] & 0xFF) << 16)
                            | ((buffer[10] & 0xFF) << 8) | (buffer[11] & 0xFF);
                    if (largo < 12 || largo > 1 << 20) {
                        log("[" + puerto + "] impossible length " + largo + ": "
                                + hex(buffer, Math.min(32, buffer.length)));
                        buffer = new byte[0];
                        break;
                    }
                    if (buffer.length < largo) {
                        break;
                    }
                    int l = (int) largo;
                    String orden = new String(buffer, 0, 4, StandardCharsets.ISO_8859_1);
                    String sub = hex(Arrays.copyOfRange(buffer, 4, 8), 4);
                    Map<String, String> campos = leerCampos(buffer, 12, l);
                    buffer = Arrays.copyOfRange(buffer, l, buffer.length);
                    log("[" + puerto + "] <- " + orden + " sub=" + sub + " " + campos);
                    synchronized (this) {
                        List<byte[]> respuestas = new ArrayList<>();
                        boolean atendida = responder(ses, orden, campos, respuestas);
                        if (!atendida) {
                            log("[" + puerto + "]    (unhandled command: replying empty)");
                        }
                        for (byte[] r : respuestas) {
                            log("[" + puerto + "] -> " + new String(r, 0, 4, StandardCharsets.ISO_8859_1)
                                    + " " + leerCampos(r, 12, r.length));
                            ses.escribir(r);
                        }
                    }
                }
            }
        } catch (IOException e) {
            if (!parado) {
                log("[" + puerto + "] " + e);
            }
        }
        boolean eraJugador;
        synchronized (this) {
            eraJugador = ses.autenticado && puerto == PUERTO_SALAS;
            olvidar(ses);
            sesiones.remove(ses);
        }
        log("[" + puerto + "] " + par + " closed");
        cerrar(socket);
        if (eraJugador) {
            alCambiar.run();
        }
    }

    private void latir() {
        byte[] m = mensaje("~png", campos("REF", LocalDateTime.now().format(FECHA), "TIME", 2));
        synchronized (this) {
            for (Sesion s : sesiones) {
                if (s.puerto == PUERTO_SALAS) {
                    s.escribir(m);
                }
            }
        }
    }

    /**
     * La pregunta de la app: una linea "GAMERTAG <nombre>" y otra de vuelta,
     * "FREE" o "IN_USE". Un FREE aparta el nombre para el que pregunta (ver
     * apartar), para que a dos moviles no se les diga a la vez que esta libre.
     */
    private void comprobarGamertag(Socket socket) {
        String ip = socket.getInetAddress().getHostAddress();
        try {
            socket.setSoTimeout(5000);
            BufferedReader r = new BufferedReader(
                    new InputStreamReader(socket.getInputStream(), StandardCharsets.UTF_8));
            String linea = r.readLine();
            linea = linea == null ? "" : linea.trim();
            int espacio = linea.indexOf(' ');
            String orden = espacio < 0 ? linea : linea.substring(0, espacio);
            String gamertag = espacio < 0 ? "" : linea.substring(espacio + 1).trim();
            String respuesta;
            if (!orden.equals("GAMERTAG") || gamertag.isEmpty()) {
                respuesta = "ERROR";
            } else {
                String otro;
                synchronized (this) {
                    otro = quienUsa(gamertag, ip);
                    if (otro == null) {
                        apartar(gamertag, ip);
                    }
                }
                respuesta = otro != null ? "IN_USE" : "FREE";
                log("[" + PUERTO_GAMERTAG + "] " + ip + " asks about '" + gamertag + "': " + respuesta
                        + (otro != null ? " (" + otro + ")" : " (held for it)"));
            }
            OutputStream salida = socket.getOutputStream();
            salida.write((respuesta + "\n").getBytes(StandardCharsets.UTF_8));
            salida.flush();
        } catch (SocketTimeoutException ignored) {
        } catch (IOException ignored) {
        }
        cerrar(socket);
    }

    // ---------------------------------------------------------------------
    // Mensajes

    /** Cabecera de 12 bytes (orden, subcodigo, largo) y CLAVE=valor\n... con un 0 al final. */
    static byte[] mensaje(String orden, Map<String, ?> campos, String sub) {
        StringBuilder cuerpo = new StringBuilder();
        if (campos != null) {
            for (Map.Entry<String, ?> e : campos.entrySet()) {
                cuerpo.append(e.getKey()).append('=').append(e.getValue()).append('\n');
            }
        }
        byte[] c = cuerpo.toString().getBytes(StandardCharsets.ISO_8859_1);
        int largo = 12 + c.length + 1;
        byte[] m = new byte[largo];
        byte[] o = orden.getBytes(StandardCharsets.ISO_8859_1);
        System.arraycopy(o, 0, m, 0, 4);
        if (sub == null) {
            sub = "\0\0\0\0";
        }
        System.arraycopy(sub.getBytes(StandardCharsets.ISO_8859_1), 0, m, 4, 4);
        m[8] = (byte) (largo >>> 24);
        m[9] = (byte) (largo >>> 16);
        m[10] = (byte) (largo >>> 8);
        m[11] = (byte) largo;
        System.arraycopy(c, 0, m, 12, c.length);
        return m;
    }

    static byte[] mensaje(String orden, Map<String, ?> campos) {
        return mensaje(orden, campos, null);
    }

    static Map<String, String> leerCampos(byte[] b, int desde, int hasta) {
        while (hasta > desde && b[hasta - 1] == 0) {
            hasta--;
        }
        String texto = new String(b, desde, hasta - desde, StandardCharsets.ISO_8859_1);
        Map<String, String> campos = new LinkedHashMap<>();
        for (String linea : texto.split("\n", -1)) {
            int igual = linea.indexOf('=');
            if (igual >= 0) {
                campos.put(linea.substring(0, igual), linea.substring(igual + 1));
            }
        }
        return campos;
    }

    /** Un mapa ordenado de pares clave, valor. */
    private static Map<String, Object> campos(Object... kv) {
        Map<String, Object> m = new LinkedHashMap<>();
        for (int i = 0; i + 1 < kv.length; i += 2) {
            m.put((String) kv[i], kv[i + 1]);
        }
        return m;
    }

    /** Lo que se contesta a cada orden; false si no se conoce (se contesta vacio). */
    private boolean responder(Sesion ses, String orden, Map<String, String> f, List<byte[]> r) {
        String nombre = orden.replaceFirst("^[@~]+", "").replace(' ', '_');
        switch (nombre) {
            case "dir": dir(ses, r); break;
            case "tic": break; // ofrece cifrar; sin respuesta sigue en claro
            case "addr": break; // el cliente dice su IP y puerto; sin respuesta
            case "png": break; // la respuesta a nuestro ping: no se contesta
            case "skey": r.add(mensaje("skey", campos("SKEY", "$51ba8aee64ddfacae5baefa6bf61e009"))); break;
            case "news": news(ses, r); break;
            case "sele": sele(ses, f, r); break;
            case "user": user(ses, f, r); break;
            case "snap": snap(ses, f, r); break;
            case "rank": rank(ses, f, r); break;
            case "usea": usea(ses, f, r); break;
            case "gcre": gcre(ses, f, r); break;
            case "gjoi": gjoi(ses, f, r); break;
            case "gset": gset(ses, f, r); break;
            case "glea": dejarPartida(ses); r.add(mensaje("glea", null)); break;
            case "gdel": dejarPartida(ses); r.add(mensaje("gdel", null)); break;
            case "gsta": gsta(ses, r); break;
            case "ujoi": ujoi(ses, f, r); break;
            case "udel": udel(ses, f, r); break;
            case "ulea": ulea(ses, r); break;
            case "mesg": mesg(ses, f, r); break;
            case "uadm": uadm(ses, f, r); break;
            case "ucre": ucre(ses, f, r); break;
            case "auxi": auxi(ses, f, r); break;
            case "auth": auth(ses, f, r); break;
            case "pers": pers(ses, f, r); break;
            default:
                r.add(mensaje(orden, null));
                return false;
        }
        return true;
    }

    private void dir(Sesion ses, List<byte[]> r) {
        r.add(mensaje("@dir", campos(
                "ADDR", ses.ipLocal,
                "PORT", PUERTO_SALAS,
                "SESS", 1000000000 + AZAR.nextInt(1000000000),
                "MASK", azarHex())));
    }

    private void news(Sesion ses, List<byte[]> r) {
        r.add(mensaje("news", campos(
                "BUDDY_SERVER", ses.ipLocal,
                "BUDDY_PORT", 10899,
                "TOSAC_URL", "",
                "TOSA_URL", "",
                "TOS_URL", "")));
    }

    private void sele(Sesion ses, Map<String, String> f, List<byte[]> r) {
        // Lo mismo que pide ("si, todo eso") y su propio registro (+who).
        r.add(mensaje("sele", f));
        r.add(registroUsuario(ses, "+who"));
        // USERSETk=<sala>: se apunta en la ranura k a los miembros de esa sala
        // (el que se une lo manda antes de ujoi): la sala (+ust) y cada miembro (+usm).
        for (Map.Entry<String, String> e : f.entrySet()) {
            String clave = e.getKey();
            if (!clave.startsWith("USERSET") || clave.length() == 7 || !clave.substring(7).matches("[0-9]+")) {
                continue;
            }
            int ranura = Integer.parseInt(clave.substring(7));
            ses.conjuntos.values().removeIf(k -> k == ranura);
            if (e.getValue().isEmpty()) {
                continue;
            }
            ses.conjuntos.put(e.getValue(), ranura);
            Sala sala = salaPorNombre(e.getValue());
            if (sala != null) {
                r.add(mensaje("+ust", registroSala(sala)));
                for (Sesion m : miembros(sala)) {
                    r.add(registroMiembro(m, ranura, false));
                }
            }
        }
    }

    // -- conjuntos de usuarios (las "salas" de ucre/ujoi/ulea/usea) -----------
    // La sala de juego es la lista de miembros del conjunto en la ranura 0, y
    // solo se llena con '+usm' (S ranura, I, N, F, G, X). El miembro 0 es uno
    // mismo: sin su propia entrada la lista sale vacia.

    private Sala salaPorNombre(String nombre) {
        for (Sala s : salas.values()) {
            if (s.f.get("N").equals(nombre)) {
                return s;
            }
        }
        return null;
    }

    private List<Sesion> miembros(Sala sala) {
        List<Sesion> l = new ArrayList<>();
        for (Sesion o : sesiones) {
            if (o.sala != null && o.sala == sala.id) {
                l.add(o);
            }
        }
        return l;
    }

    private byte[] registroMiembro(Sesion ses, int ranura, boolean quitar) {
        if (quitar) {
            return mensaje("+usm", campos("S", ranura, "I", ses.id));
        }
        return mensaje("+usm", campos(
                "S", ranura,
                "I", ses.id,
                "N", ses.nombre(),
                "F", "U",
                "G", ses.partida != null ? ses.partida.f.get("IDENT") : 0,
                "X", ses.auxi));
    }

    /** '+usm' de `ses` a los apuntados a `sala`; la copia de `pide` se devuelve en `r`. */
    private void avisarConjunto(Sala sala, Sesion ses, Sesion pide, boolean quitar, List<byte[]> r) {
        for (Sesion o : sesiones) {
            Integer ranura = o.conjuntos.get(sala.f.get("N"));
            if (ranura == null) {
                continue;
            }
            byte[] m = registroMiembro(ses, ranura, quitar);
            if (o == pide) {
                if (r != null) {
                    r.add(m);
                }
            } else {
                o.escribir(m);
            }
        }
    }

    private void olvidar(Sesion ses) {
        dejarPartida(ses);
        // Su nombre le sigue apartado un rato, para que nadie lo coja nada mas
        // salir de Xbox Live.
        if (ses.autenticado) {
            apartar(ses.nombre(), ses.ip);
        }
        if (ses.sala != null && salas.containsKey(ses.sala)) {
            avisarConjunto(salas.get(ses.sala), ses, ses, true, null);
        }
        for (Sala s : new ArrayList<>(salas.values())) {
            if (s.dueno == ses) {
                borrarSala(s, ses);
            }
        }
    }

    /** Borra una sala: los que siguen en ella salen (y sus apuntados se enteran). */
    private void borrarSala(Sala sala, Sesion dueno) {
        for (Sesion m : miembros(sala)) {
            if (m != dueno) {
                avisarConjunto(sala, m, null, true, null);
                m.sala = null;
            }
        }
        if (dueno.sala != null && dueno.sala == sala.id) {
            dueno.sala = null;
        }
        salas.remove(sala.id);
        log("   room " + sala.id + " (" + sala.f.get("N") + ") of " + sala.f.get("O") + " deleted");
    }

    private void user(Sesion ses, Map<String, String> f, List<byte[]> r) {
        String nombre = f.containsKey("PERS") ? f.get("PERS") : ses.nombre();
        Map<String, Object> reg = campos(
                "PERS", nombre,
                "LAST", "2005.11.15-00:00:00",
                "EXPR", "1072566000");
        reg.putAll(estadisticas.personales(nombre));
        r.add(mensaje("user", reg));
    }

    private void snap(Sesion ses, Map<String, String> f, List<byte[]> r) {
        // Clasificacion: CHAN (donde pone el juego las filas), INDEX, START/RANGE o
        // FIND=$. La respuesta repite CHAN y dice en RANGE cuantas '+snp' siguen.
        List<Estadisticas.Fila> filas = estadisticas.clasificacion();
        if (!vacio(f.get("FIND"))) {
            String yo = minusculas(ses.nombre());
            filas.removeIf(fila -> !minusculas(fila.nombre).equals(yo));
        } else {
            int inicio = entero(f.get("START"), 0);
            int cuantas = entero(f.get("RANGE"), 100);
            int desde = Math.min(Math.max(inicio, 0), filas.size());
            filas = new ArrayList<>(filas.subList(desde, Math.min(filas.size(), desde + Math.max(cuantas, 0))));
        }
        r.add(mensaje("snap", campos(
                "CHAN", f.containsKey("CHAN") ? f.get("CHAN") : "0",
                "START", f.containsKey("START") ? f.get("START") : 0,
                "RANGE", filas.size())));
        for (Estadisticas.Fila fila : filas) {
            r.add(mensaje("+snp", campos("N", fila.nombre, "R", fila.puesto, "P", fila.puntos,
                    "O", 0, "S", Estadisticas.texto(fila.fila))));
        }
    }

    private void rank(Sesion ses, Map<String, String> f, List<byte[]> r) {
        // Resultados de una carrera de cada jugador al acabar: REPT quien, RESU
        // los resultados (base64 en URL). Su propio puesto va a sus estadisticas.
        String nombre = !vacio(f.get("REPT")) ? f.get("REPT") : ses.nombre();
        int[] ajustesCarrera = ajustesCarrera(ses);
        int modo = ajustesCarrera[0];
        boolean clasificatoria = ajustesCarrera[1] == 1;
        byte[] d;
        try {
            d = Base64.getDecoder().decode(desescapar(f.containsKey("RESU") ? f.get("RESU") : "")
                    .replaceAll("[^A-Za-z0-9+/=]", ""));
        } catch (IllegalArgumentException e) {
            d = new byte[0];
        }
        Integer contado = null;
        try {
            int[] res = Estadisticas.leerResultados(d);
            int jugadores = res[0];
            int quien = res[1];
            for (int i = 2; i + 1 < res.length; i += 2) {
                if (res[i] == quien && res[i + 1] != 0) {
                    estadisticas.sumar(nombre, jugadores, res[i + 1], modo, clasificatoria);
                    contado = res[i + 1];
                    if (!Estadisticas.BLOQUE_MODO.containsKey(modo)) {
                        log("   race mode M=" + modo + " not mapped to a STAT block yet: counted in the first one");
                    }
                    break;
                }
            }
        } catch (IllegalArgumentException e) {
            log("   rank from " + nombre + ": " + e.getMessage());
        }
        log("   rank: " + nombre + " finished " + contado + " of the race (M=" + modo
                + ", ranked=" + (clasificatoria ? "True" : "False") + ")");
        try {
            JSONObject linea = new JSONObject();
            linea.put("time", LocalDateTime.now().withNano(0).toString());
            linea.put("from", nombre);
            linea.put("fields", new JSONObject(f));
            linea.put("results", hex(d, d.length));
            linea.put("mode", modo);
            linea.put("ranked", clasificatoria);
            linea.put("counted", contado == null ? JSONObject.NULL : contado);
            try (Writer w = new OutputStreamWriter(new FileOutputStream(new File(datos, "races.jsonl"), true),
                    StandardCharsets.UTF_8)) {
                w.write(linea.toString() + "\n");
            }
        } catch (JSONException | IOException e) {
            log("   races.jsonl: " + e);
        }
        r.add(mensaje("rank", campos()));
    }

    /** {modo M, 1 si es clasificatoria} de los PARAMS de la sala de `ses` (V=... P=1 L=3 M=2). */
    private int[] ajustesCarrera(Sesion ses) {
        Sala sala = ses.sala != null ? salas.get(ses.sala) : null;
        Map<String, String> params = new LinkedHashMap<>();
        for (String linea : desescapar(sala != null ? sala.f.get("P") : "").split("\r\n|\r|\n")) {
            int igual = linea.indexOf('=');
            String clave = igual < 0 ? linea : linea.substring(0, igual);
            params.put(clave.trim(), igual < 0 ? "" : linea.substring(igual + 1).trim());
        }
        int modo;
        try {
            modo = Integer.parseInt(params.containsKey("M") ? params.get("M") : "2");
        } catch (NumberFormatException e) {
            modo = 2;
        }
        String p = params.containsKey("P") ? params.get("P") : "1";
        return new int[] {modo, "1".equals(p) ? 1 : 0};
    }

    private void usea(Sesion ses, Map<String, String> f, List<byte[]> r) {
        // Busqueda de salas (la de Partida rapida): COUNT, y cada una en un '+uss'.
        List<Sala> otras = new ArrayList<>();
        for (Sala s : salas.values()) {
            if (s.dueno != ses) {
                otras.add(s);
            }
        }
        r.add(mensaje("usea", campos("START", f.containsKey("START") ? f.get("START") : 0,
                "COUNT", otras.size())));
        for (Sala s : otras) {
            r.add(mensaje("+uss", registroSala(s)));
        }
    }

    // -- partidas (la carrera dentro de una sala) ------------------------------

    private Map<String, Object> registroPartida(Partida p, Sesion mira) {
        Map<String, Object> f = campos(
                "IDENT", p.f.get("IDENT"),
                "NAME", p.f.get("NAME"),
                "SELF", mira.nombre(),
                "HOST", p.f.get("HOST"),
                "PARAMS", p.f.get("PARAMS"),
                "PLATPARAMS", p.f.get("PLATPARAMS"),
                "ROOM", p.f.get("ROOM"),
                "CUSTFLAGS", p.f.get("CUSTFLAGS"),
                "SYSFLAGS", p.f.get("SYSFLAGS"),
                "COUNT", p.jugadores.size(),
                "PRIV", 0,
                "MINSIZE", p.f.get("MINSIZE"),
                "MAXSIZE", p.f.get("MAXSIZE"),
                "NUMPART", 1,
                "PARTSIZE0", p.f.get("MAXSIZE"),
                "PARTPARAMS0", "",
                "SEED", p.f.get("SEED"),
                "WHEN", p.f.get("WHEN"),
                "AUTH", "",
                "SESS", p.f.containsKey("SESS") ? p.f.get("SESS") : "");
        for (int i = 0; i < p.jugadores.size(); i++) {
            Sesion j = p.jugadores.get(i);
            f.put("OPPO" + i, j.nombre());
            f.put("OPID" + i, j.id);
            f.put("ADDR" + i, j.ip);
            f.put("LADDR" + i, j.ip);
            f.put("MADDR" + i, j.maddr);
            f.put("OPPART" + i, 0);
            f.put("OPPARAM" + i, j.opparam);
            f.put("OPFLAG" + i, 0);
            f.put("PRES" + i, 0);
        }
        return f;
    }

    /** El registro de la partida a todos sus jugadores (menos uno). */
    private void avisar(Partida p, String orden, Sesion salvo) {
        for (Sesion j : p.jugadores) {
            if (j == salvo) {
                continue;
            }
            log("   " + orden + " -> " + j.nombre() + " (COUNT=" + p.jugadores.size() + ")");
            j.escribir(mensaje(orden, registroPartida(p, j)));
        }
    }

    private void gcre(Sesion ses, Map<String, String> f, List<byte[]> r) {
        String yo = ses.nombre();
        siguienteId++;
        Partida p = new Partida();
        p.f.put("IDENT", String.valueOf(siguienteId));
        p.f.put("NAME", f.containsKey("NAME") ? f.get("NAME") : yo);
        p.f.put("HOST", yo);
        p.f.put("PARAMS", valor(f, "PARAMS", ""));
        p.f.put("PLATPARAMS", valor(f, "PLATPARAMS", ""));
        p.f.put("ROOM", String.valueOf(ses.sala != null && ses.sala != 0 ? ses.sala : 0));
        p.f.put("CUSTFLAGS", valor(f, "CUSTFLAGS", "0"));
        p.f.put("SYSFLAGS", valor(f, "SYSFLAGS", "0"));
        p.f.put("MINSIZE", valor(f, "MINSIZE", "2"));
        p.f.put("MAXSIZE", valor(f, "MAXSIZE", "4"));
        p.f.put("SEED", String.valueOf(1 + AZAR.nextInt(Integer.MAX_VALUE - 1)));
        p.f.put("WHEN", LocalDateTime.now().format(FECHA));
        p.jugadores.add(ses);
        partidas.put(p.f.get("NAME"), p);
        ses.partida = p;
        // Despues, su registro con G = esta partida.
        r.add(mensaje("gcre", registroPartida(p, ses)));
        r.add(registroUsuario(ses, "+who"));
    }

    private void gjoi(Sesion ses, Map<String, String> f, List<byte[]> r) {
        Partida p = partidas.get(valor(f, "NAME", ""));
        if (p == null) {
            log("   gjoi: no game '" + f.get("NAME") + "'");
            r.add(mensaje("gjoi", campos(), "nfnd"));
            return;
        }
        if (!p.jugadores.contains(ses)) {
            p.jugadores.add(ses);
        }
        ses.partida = p;
        avisar(p, "+mgm", ses);
        // Solo el registro: el que se une conecta con el SESS de este registro.
        // '+ses' es para cuando el anfitrion arranca (gsta).
        r.add(mensaje("gjoi", registroPartida(p, ses)));
        r.add(registroUsuario(ses, "+who"));
    }

    private void gset(Sesion ses, Map<String, String> f, List<byte[]> r) {
        Partida p = ses.partida;
        if (p == null) {
            r.add(mensaje("gset", campos()));
            return;
        }
        // Se guarda todo lo que manda el anfitrion; ojo con SESS, su sesion de Xbox
        // codificada ("$..."): el que se une solo conecta si empieza por '$'.
        for (Map.Entry<String, String> e : f.entrySet()) {
            if (!e.getKey().equals("NAME")) {
                p.f.put(e.getKey(), e.getValue());
            }
        }
        avisar(p, "+mgm", ses);
        r.add(mensaje("gset", registroPartida(p, ses)));
    }

    private void dejarPartida(Sesion ses) {
        Partida p = ses.partida;
        if (p == null) {
            return;
        }
        p.jugadores.remove(ses);
        ses.partida = null;
        if (p.jugadores.isEmpty() || p.f.get("HOST").equals(ses.nombre())) {
            partidas.remove(p.f.get("NAME"));
            for (Sesion j : p.jugadores) {
                j.partida = null;
            }
            log("   game " + p.f.get("NAME") + " closed");
        } else {
            avisar(p, "+mgm", null);
        }
    }

    private void gsta(Sesion ses, List<byte[]> r) {
        // El anfitrion arranca: '+ses' con el registro (y su SESS) a todos.
        Partida p = ses.partida;
        if (p != null) {
            avisar(p, "+ses", null);
        }
        r.add(mensaje("gsta", campos()));
    }

    private Sala buscarSala(Map<String, String> f) {
        for (String clave : new String[] {"I", "IDENT", "ROOM"}) {
            String v = f.get(clave);
            if (v != null && v.replaceFirst("^-+", "").matches("[0-9]+")) {
                try {
                    Sala s = salas.get(Integer.parseInt(v));
                    if (s != null) {
                        return s;
                    }
                } catch (NumberFormatException ignored) {
                }
            }
        }
        String nombre = !vacio(f.get("NAME")) ? f.get("NAME") : f.get("N");
        for (Sala s : salas.values()) {
            if (s.f.get("N").equals(nombre) || s.f.get("O").equals(nombre)) {
                return s;
            }
        }
        return null;
    }

    private void ujoi(Sesion ses, Map<String, String> f, List<byte[]> r) {
        Sala sala = buscarSala(f);
        if (sala == null) {
            log("   ujoi: can't find the room for " + f);
            r.add(mensaje("ujoi", campos(), "nfnd"));
            return;
        }
        sala.f.put("C", String.valueOf(entero(sala.f.get("C"), 0) + 1));
        ses.sala = sala.id;
        // Los que ya estan reciben el '+usr' del que entra y el que entra el de
        // cada uno: con ese registro (LA, MA) el anfitrion le da de alta en la
        // conexion entre pares.
        r.add(mensaje("ujoi", registroSala(sala)));
        for (Sesion otro : sesiones) {
            if (otro != ses && otro.sala != null && otro.sala == sala.id) {
                log("   +usr " + ses.nombre() + " -> " + otro.nombre());
                otro.escribir(registroUsuario(ses, "+usr"));
                r.add(registroUsuario(otro, "+usr"));
            }
        }
        r.add(registroUsuario(ses, "+usr"));
        avisarConjunto(sala, ses, ses, false, r);
    }

    private void udel(Sesion ses, Map<String, String> f, List<byte[]> r) {
        // El dueno borra su sala (tras gdel al salir de "Crear partida").
        Sala sala = salaPorNombre(valor(f, "NAME", ""));
        if (sala == null && ses.sala != null) {
            sala = salas.get(ses.sala);
        }
        if (sala != null && sala.dueno == ses) {
            borrarSala(sala, ses);
        }
        r.add(mensaje("udel", campos()));
    }

    private void ulea(Sesion ses, List<byte[]> r) {
        Sala sala = ses.sala != null ? salas.get(ses.sala) : null;
        if (sala != null && entero(sala.f.get("C"), 0) > 1) {
            sala.f.put("C", String.valueOf(entero(sala.f.get("C"), 0) - 1));
        }
        ses.sala = null;
        r.add(mensaje("ulea", campos()));
        if (sala != null) {
            avisarConjunto(sala, ses, ses, true, r);
        }
    }

    private void mesg(Sesion ses, Map<String, String> f, List<byte[]> r) {
        // Mensaje a la partida (al unirse, TEXT=42 ATTR=EGS: el "estoy aqui" que
        // hace que el anfitrion le de de alta). Los demas lo reciben como '+msg'.
        Partida p = ses.partida;
        List<Sesion> destinos = new ArrayList<>();
        if (p != null) {
            for (Sesion j : p.jugadores) {
                if (j != ses) {
                    destinos.add(j);
                }
            }
        }
        if (destinos.isEmpty() && ses.sala != null) {
            for (Sesion o : sesiones) {
                if (o != ses && o.sala != null && o.sala.equals(ses.sala)) {
                    destinos.add(o);
                }
            }
        }
        for (Sesion j : destinos) {
            log("   +msg " + ses.nombre() + " -> " + j.nombre() + ": " + f.get("TEXT") + " " + f.get("ATTR"));
            j.escribir(mensaje("+msg", campos("N", ses.nombre(), "T", valor(f, "TEXT", ""),
                    "F", valor(f, "ATTR", ""))));
        }
        r.add(mensaje("mesg", campos()));
    }

    private void uadm(Sesion ses, Map<String, String> f, List<byte[]> r) {
        // El anfitrion cambia su sala (parametros, banderas).
        Sala sala = ses.sala != null ? salas.get(ses.sala) : null;
        if (sala == null) {
            r.add(mensaje("uadm", campos()));
            return;
        }
        sala.f.put("N", valor(f, "NAME", sala.f.get("N")));
        sala.f.put("D", valor(f, "DESC", sala.f.get("D")));
        sala.f.put("P", valor(f, "PARAMS", sala.f.get("P")));
        sala.f.put("CF", valor(f, "CUSTFLAGS", sala.f.get("CF")));
        r.add(mensaje("uadm", registroSala(sala)));
    }

    /** Las claves que lee el juego (sub_82825B28, en orden): I T SF CF O S C N D P. */
    private static Map<String, Object> registroSala(Sala sala) {
        Map<String, Object> m = new LinkedHashMap<>();
        for (String k : new String[] {"I", "T", "SF", "CF", "O", "S", "C", "N", "D", "P"}) {
            m.put(k, sala.f.get(k));
        }
        return m;
    }

    private void ucre(Sesion ses, Map<String, String> f, List<byte[]> r) {
        // Crear sala ("Crear partida"); despues el juego manda gcre. Una por jugador.
        for (Sala vieja : new ArrayList<>(salas.values())) {
            if (vieja.dueno == ses) {
                borrarSala(vieja, ses);
            }
        }
        siguienteId++;
        Sala sala = new Sala();
        sala.id = siguienteId;
        sala.dueno = ses;
        sala.f.put("I", String.valueOf(sala.id));
        sala.f.put("T", valor(f, "TYPE", "0"));
        sala.f.put("SF", valor(f, "SYSFLAGS", ""));
        sala.f.put("CF", valor(f, "CUSTFLAGS", ""));
        sala.f.put("O", ses.nombre());
        sala.f.put("S", valor(f, "SIZE", "4"));
        sala.f.put("C", "1");
        sala.f.put("N", valor(f, "NAME", ""));
        sala.f.put("D", "");
        sala.f.put("P", valor(f, "PARAMS", ""));
        salas.put(sala.id, sala);
        ses.sala = sala.id;
        // El dueno es miembro y no manda 'sele USERSET0': se le da la ranura 0.
        ses.conjuntos.values().removeIf(k -> k == 0);
        ses.conjuntos.put(sala.f.get("N"), 0);
        r.add(mensaje("ucre", registroSala(sala)));
        r.add(registroUsuario(ses, "+usr"));
        r.add(registroMiembro(ses, 0, false));
    }

    private void auxi(Sesion ses, Map<String, String> f, List<byte[]> r) {
        // Su texto auxiliar (su estado en la sala): se guarda, se devuelve y va un
        // '+usr' nuevo a todos los de su sala, el incluido.
        ses.auxi = valor(f, "TEXT", "");
        r.add(mensaje("auxi", f));
        if (ses.sala != null) {
            for (Sesion otro : sesiones) {
                if (otro != ses && otro.sala != null && otro.sala.equals(ses.sala)) {
                    otro.escribir(registroUsuario(ses, "+usr"));
                }
            }
            r.add(registroUsuario(ses, "+usr"));
            Sala sala = salas.get(ses.sala);
            if (sala != null) {
                avisarConjunto(sala, ses, ses, false, r);
            }
        }
    }

    /** +who / +usr: G (su partida; 0 = ninguna) y MA (sin el, ConnApi sin XUID). */
    private static byte[] registroUsuario(Sesion ses, String orden) {
        return mensaje(orden, campos(
                "I", ses.id,
                "M", ses.nombreCuenta,
                "N", ses.nombre(),
                "MA", ses.maddr,
                "A", ses.ip,
                "LA", ses.ip,
                "G", ses.partida != null ? ses.partida.f.get("IDENT") : 0,
                "P", 1,
                "F", "U",
                "LO", "esES",
                "PRES", 1,
                "X", ses.auxi));
    }

    private void auth(Sesion ses, Map<String, String> f, List<byte[]> r) {
        ses.nombreCuenta = !vacio(f.get("NAME")) ? f.get("NAME")
                : !vacio(f.get("GTAG")) ? f.get("GTAG") : ses.nombreCuenta;
        ses.maddr = valor(f, "MADDR", ses.maddr);
        boolean antes = ses.autenticado;
        ses.autenticado = true;
        String otro = quienUsa(ses.nombreCuenta, ses.ip);
        if (otro != null) {
            log("   warning: gamertag '" + ses.nombreCuenta + "' already in use by " + otro);
        }
        r.add(mensaje("auth", campos(
                "NAME", ses.nombreCuenta,
                "ADDR", ses.ip,
                "PERSONAS", ses.nombreCuenta,
                "LOC", "esES",
                "MAIL", "lan@lan",
                "SPAM", "NN",
                "TOS", 1,
                "SINCE", "2005.11.15-00:00:00",
                "LAST", "2005.11.15-00:00:00")));
        if (!antes && ses.puerto == PUERTO_SALAS) {
            // Con el candado puesto: el servicio solo lo pasa a su hilo.
            alCambiar.run();
        }
    }

    private void pers(Sesion ses, Map<String, String> f, List<byte[]> r) {
        ses.persona = !vacio(f.get("PERS")) ? f.get("PERS")
                : !vacio(f.get("GTAG")) ? f.get("GTAG") : ses.nombreCuenta;
        ses.maddr = valor(f, "MADDR", ses.maddr);
        r.add(mensaje("pers", campos(
                "PERS", ses.persona,
                "LKEY", azarHex(),
                "LOC", "esES",
                "A", ses.ip,
                "LA", ses.ip,
                "IDLE", 600000)));
    }

    /** La IP de otro aparato que tiene ahora este gamertag (conectado o apartado); null si esta libre. */
    private String quienUsa(String gamertag, String ip) {
        String buscado = minusculas(gamertag.trim());
        for (Sesion o : sesiones) {
            if (o.ip.equals(ip) || !o.autenticado) {
                continue;
            }
            if (minusculas(o.nombreCuenta).equals(buscado)
                    || (o.persona != null && minusculas(o.persona).equals(buscado))) {
                return o.ip;
            }
        }
        Apartado a = apartados.get(buscado);
        if (a != null && !a.ip.equals(ip) && a.fin > System.nanoTime()) {
            return a.ip;
        }
        return null;
    }

    /** Aparta `gamertag` para `ip` unos minutos; lo que tuviera apartado esa IP se suelta. */
    private void apartar(String gamertag, String ip) {
        long ahora = System.nanoTime();
        apartados.values().removeIf(a -> a.ip.equals(ip) || a.fin <= ahora);
        apartados.put(minusculas(gamertag.trim()),
                new Apartado(ip, ahora + TimeUnit.MILLISECONDS.toNanos(APARTADO_MS)));
    }

    // ---------------------------------------------------------------------
    // Utilidades

    void log(String texto) {
        String linea = LocalDateTime.now().format(HORA) + " " + texto;
        synchronized (registro) {
            try {
                registro.write(linea + "\n");
                registro.flush();
            } catch (IOException ignored) {
            }
        }
    }

    private static String valor(Map<String, String> f, String clave, String porDefecto) {
        String v = f.get(clave);
        return v != null ? v : porDefecto;
    }

    private static boolean vacio(String s) {
        return s == null || s.isEmpty();
    }

    private static int entero(String s, int porDefecto) {
        try {
            return vacio(s) ? porDefecto : Integer.parseInt(s.trim());
        } catch (NumberFormatException e) {
            return porDefecto;
        }
    }

    static String minusculas(String s) {
        return s == null ? "" : s.toLowerCase(Locale.ROOT);
    }

    private static String azarHex() {
        byte[] b = new byte[16];
        AZAR.nextBytes(b);
        return hex(b, 16);
    }

    private static String hex(byte[] b, int n) {
        StringBuilder s = new StringBuilder();
        for (int i = 0; i < n; i++) {
            s.append(String.format(Locale.ROOT, "%02x", b[i] & 0xFF));
        }
        return s.toString();
    }

    /** urllib.parse.unquote: los %XX y nada mas (un '+' se queda, que en base64 cuenta). */
    static String desescapar(String s) {
        if (s == null || s.indexOf('%') < 0) {
            return s == null ? "" : s;
        }
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '%' && i + 2 < s.length()
                    && Character.digit(s.charAt(i + 1), 16) >= 0 && Character.digit(s.charAt(i + 2), 16) >= 0) {
                b.write(Integer.parseInt(s.substring(i + 1, i + 3), 16));
                i += 2;
            } else {
                byte[] u = String.valueOf(c).getBytes(StandardCharsets.UTF_8);
                b.write(u, 0, u.length);
            }
        }
        return new String(b.toByteArray(), StandardCharsets.UTF_8);
    }

    private static byte[] juntar(byte[] a, byte[] b, int n) {
        byte[] c = Arrays.copyOf(a, a.length + n);
        System.arraycopy(b, 0, c, a.length, n);
        return c;
    }

    private static void cerrar(java.io.Closeable c) {
        try {
            c.close();
        } catch (IOException ignored) {
        }
    }

    // ---------------------------------------------------------------------
    // Estado

    /** Lo que el servidor sabe de un jugador conectado. */
    private static final class Sesion {
        private static int cuenta;

        final Socket socket;
        final OutputStream salida;
        final int puerto;
        final String ip;
        // La IP de este movil por la que ha llegado (la que se le da en @dir).
        final String ipLocal;
        final int id;
        String nombreCuenta = "USER";
        String persona;
        Integer sala;
        Partida partida;
        String maddr = "";
        String opparam = "";
        String auxi = "";
        // Lo pone auth: hasta entonces el nombre es solo el de relleno.
        boolean autenticado;
        // Conjuntos a los que esta apuntado: nombre de la sala -> ranura (0..4).
        final Map<String, Integer> conjuntos = new LinkedHashMap<>();
        private boolean roto;

        Sesion(Socket socket, int puerto) throws IOException {
            this.socket = socket;
            this.salida = socket.getOutputStream();
            this.puerto = puerto;
            this.ip = socket.getInetAddress().getHostAddress();
            this.ipLocal = socket.getLocalAddress().getHostAddress();
            synchronized (Sesion.class) {
                this.id = ++cuenta;
            }
        }

        /** persona or name, como en el de Python. */
        String nombre() {
            return !vacio(persona) ? persona : nombreCuenta;
        }

        /** Se llama con el candado del servidor. Si el socket ha muerto, su hilo lo cerrara. */
        void escribir(byte[] m) {
            if (roto) {
                return;
            }
            try {
                salida.write(m);
                salida.flush();
            } catch (IOException e) {
                roto = true;
                cerrar(socket);
            }
        }
    }

    private static final class Sala {
        int id;
        // I T SF CF O S C N D P, como texto.
        final Map<String, String> f = new LinkedHashMap<>();
        // No se manda: la sesion duena. El nombre de "O" no basta si dos moviles
        // van con el mismo gamertag.
        Sesion dueno;
    }

    private static final class Partida {
        // IDENT NAME HOST PARAMS ... y lo que mande el anfitrion en gset.
        final Map<String, String> f = new LinkedHashMap<>();
        final List<Sesion> jugadores = new ArrayList<>();
    }

    private static final class Apartado {
        final String ip;
        final long fin;

        Apartado(String ip, long fin) {
            this.ip = ip;
            this.fin = fin;
        }
    }

    /**
     * Estadisticas y clasificaciones, en stats.json con el mismo formato que el
     * de Python (se puede copiar de uno a otro). Lo que lee el juego y de donde
     * sale cada numero: la clase Stats de ea_server.py y su README.
     */
    static final class Estadisticas {
        static final int LARGO = 44;
        static final int MODOS = 3;
        static final int PRIMER_MODO = 8;
        static final int POR_MODO = 12;
        // Parametro M de la sala (2 = circuito) -> bloque de STAT. Solo se sabe el
        // circuito; el resto, al primero.
        static final Map<Integer, Integer> BLOQUE_MODO = Collections.singletonMap(2, 0);
        static final int PUNTOS_POR_PUESTO = 10;

        private final File carpeta;
        private final File ruta;
        private JSONObject jugadores;

        Estadisticas(File carpeta) {
            this.carpeta = carpeta;
            this.ruta = new File(carpeta, "stats.json");
            try {
                String t = new String(Files.readAllBytes(ruta.toPath()), StandardCharsets.UTF_8);
                jugadores = new JSONObject(t).getJSONObject("players");
            } catch (IOException | JSONException e) {
                jugadores = new JSONObject();
            }
        }

        /** RESU: {jugadores, indice de quien lo manda, indice, puesto, indice, puesto...}. */
        static int[] leerResultados(byte[] d) {
            if (d.length < 8 || d[0] != 3) {
                throw new IllegalArgumentException("unknown results version");
            }
            int n = d[1] & 0xFF;
            int[] r = new int[2 + 2 * n];
            r[0] = n;
            r[1] = d[3] & 0xFF;
            int o = 8;
            for (int i = 0; i < n; i++) {
                if (o + 51 > d.length) {
                    throw new IllegalArgumentException("results cut short");
                }
                r[2 + 2 * i] = d[o] & 0xFF;
                r[3 + 2 * i] = d[o + 1] & 0xFF;
                o += 51 + (d[o + 50] & 0xFF);
            }
            return r;
        }

        void sumar(String nombre, int jugadores, int puesto, int modo, boolean clasificatoria) {
            try {
                JSONObject p = jugador(nombre);
                JSONArray st = p.getJSONArray("stat");
                Integer bloqueModo = BLOQUE_MODO.get(modo);
                int bloque = PRIMER_MODO + POR_MODO * (bloqueModo != null ? bloqueModo : 0);
                boolean gano = puesto == 1;
                st.put(bloque, st.getLong(bloque) + (long) (jugadores - puesto + 1) * PUNTOS_POR_PUESTO);
                int i = bloque + (gano ? 1 : 2) + (clasificatoria ? 0 : 2);
                st.put(i, st.getLong(i) + 1);
                p.put("last_game", System.currentTimeMillis() / 1000);
                guardar();
            } catch (JSONException ignored) {
            }
        }

        private void guardar() {
            try {
                //noinspection ResultOfMethodCallIgnored
                carpeta.mkdirs();
                File tmp = new File(carpeta, "stats.tmp");
                JSONObject raiz = new JSONObject();
                raiz.put("players", jugadores);
                Files.write(tmp.toPath(), raiz.toString(1).getBytes(StandardCharsets.UTF_8));
                if (!tmp.renameTo(ruta)) {
                    //noinspection ResultOfMethodCallIgnored
                    ruta.delete();
                    //noinspection ResultOfMethodCallIgnored
                    tmp.renameTo(ruta);
                }
            } catch (IOException | JSONException ignored) {
            }
        }

        private JSONObject jugador(String nombre) throws JSONException {
            String clave = minusculas(nombre.trim());
            JSONObject p = jugadores.optJSONObject(clave);
            if (p == null) {
                p = new JSONObject();
                p.put("name", nombre.trim());
                p.put("stat", ceros());
                p.put("week", JSONObject.NULL);
                p.put("month", JSONObject.NULL);
                p.put("last_game", 0);
                jugadores.put(clave, p);
            }
            // Empieza otra semana / mes: como estaban entonces.
            LocalDate hoy = LocalDate.now();
            String semana = String.format(Locale.ROOT, "%d-W%02d",
                    hoy.get(IsoFields.WEEK_BASED_YEAR), hoy.get(IsoFields.WEEK_OF_WEEK_BASED_YEAR));
            String mes = String.format(Locale.ROOT, "%04d-%02d", hoy.getYear(), hoy.getMonthValue());
            for (String[] periodo : new String[][] {{"week", semana}, {"month", mes}}) {
                JSONObject actual = p.optJSONObject(periodo[0]);
                if (actual == null || !periodo[1].equals(actual.optString("id"))) {
                    JSONObject nuevo = new JSONObject();
                    nuevo.put("id", periodo[1]);
                    nuevo.put("stat", new JSONArray(p.getJSONArray("stat").toString()));
                    p.put(periodo[0], nuevo);
                }
            }
            return p;
        }

        private static JSONArray ceros() {
            JSONArray a = new JSONArray();
            for (int i = 0; i < LARGO; i++) {
                a.put(0);
            }
            return a;
        }

        static String texto(long[] valores) {
            StringBuilder s = new StringBuilder();
            for (int i = 0; i < valores.length; i++) {
                if (i > 0) {
                    s.append(',');
                }
                long v = valores[i];
                s.append(v < 0 ? "-" + Long.toHexString(-v) : Long.toHexString(v));
            }
            return s.toString();
        }

        private static long[] numeros(JSONArray a) {
            long[] v = new long[a.length()];
            for (int i = 0; i < v.length; i++) {
                v[i] = a.optLong(i);
            }
            return v;
        }

        Map<String, Object> personales(String nombre) {
            Map<String, Object> m = new LinkedHashMap<>();
            try {
                JSONObject p = jugador(nombre);
                guardar();
                m.put("STAT", texto(numeros(p.getJSONArray("stat"))));
                m.put("LWSTAT", texto(numeros(p.getJSONObject("week").getJSONArray("stat"))));
                m.put("LMSTAT", texto(numeros(p.getJSONObject("month").getJSONArray("stat"))));
                m.put("LGAME", p.optLong("last_game"));
            } catch (JSONException ignored) {
            }
            return m;
        }

        static final class Fila {
            int puesto;
            final String nombre;
            final long puntos;
            final long victorias;
            final long[] fila;

            Fila(String nombre, long puntos, long victorias, long[] fila) {
                this.nombre = nombre;
                this.puntos = puntos;
                this.victorias = victorias;
                this.fila = fila;
            }
        }

        /** Todos los que han corrido, el mejor primero. */
        List<Fila> clasificacion() {
            List<Fila> filas = new ArrayList<>();
            Iterator<String> claves = jugadores.keys();
            while (claves.hasNext()) {
                JSONObject p = jugadores.optJSONObject(claves.next());
                JSONArray st = p != null ? p.optJSONArray("stat") : null;
                if (st == null) {
                    continue;
                }
                long puntos = 0, ganadas = 0, perdidas = 0;
                for (int m = 0; m < MODOS; m++) {
                    int b = PRIMER_MODO + POR_MODO * m;
                    puntos += st.optLong(b);
                    ganadas += st.optLong(b + 1) + st.optLong(b + 3);
                    perdidas += st.optLong(b + 2) + st.optLong(b + 4);
                }
                if (ganadas + perdidas > 0) {
                    filas.add(new Fila(p.optString("name"), puntos, ganadas,
                            new long[] {0, ganadas, perdidas, 0, 0, 0, puntos}));
                }
            }
            filas.sort((a, b) -> a.puntos != b.puntos ? Long.compare(b.puntos, a.puntos)
                    : a.victorias != b.victorias ? Long.compare(b.victorias, a.victorias)
                    : minusculas(a.nombre).compareTo(minusculas(b.nombre)));
            for (int i = 0; i < filas.size(); i++) {
                filas.get(i).puesto = i + 1;
            }
            return filas;
        }
    }
}
