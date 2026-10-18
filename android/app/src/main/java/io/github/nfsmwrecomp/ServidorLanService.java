package io.github.nfsmwrecomp;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;

/**
 * El servidor de EA de la LAN (ServidorEa) en este movil, para que haga de
 * anfitrion sin PC: los demas ponen la IP de este como "Servidor LAN".
 * Servicio en primer plano del proceso principal: el juego va en el suyo
 * (:juego, ver GameActivity) y se cierra en cada partida, y el servidor tiene
 * que seguir vivo mientras tanto, con la app en segundo plano. Lo arranca
 * SetupActivity con el ajuste "anfitrion" y se para al quitarlo o con el boton
 * de la notificacion.
 */
public class ServidorLanService extends Service {
    private static final String CANAL = "servidor_lan";
    private static final int NOTIFICACION = 1;
    private static final String PARAR = "io.github.nfsmwrecomp.PARAR_SERVIDOR";

    private final Handler hiloPrincipal = new Handler(Looper.getMainLooper());
    private ServidorEa servidor;
    private boolean enMarcha;

    static void arrancar(Context ctx) {
        ctx.startForegroundService(new Intent(ctx, ServidorLanService.class));
    }

    static void parar(Context ctx) {
        ctx.stopService(new Intent(ctx, ServidorLanService.class));
    }

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(Idioma.envolver(base));
    }

    @Override
    public void onCreate() {
        super.onCreate();
        NotificationManager nm = getSystemService(NotificationManager.class);
        NotificationChannel canal = new NotificationChannel(CANAL,
                getString(R.string.servidor_notificacion_canal), NotificationManager.IMPORTANCE_LOW);
        canal.setShowBadge(false);
        nm.createNotificationChannel(canal);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && PARAR.equals(intent.getAction())) {
            // Desde la notificacion: que no vuelva a arrancar al abrir la app.
            new Ajustes(this).anfitrionLan(false);
            stopSelf();
            return START_NOT_STICKY;
        }
        // Siempre, y enseguida: startForegroundService obliga a llamarlo.
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(NOTIFICACION, notificacion(), ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(NOTIFICACION, notificacion());
        }
        if (!enMarcha) {
            enMarcha = true;
            // Abrir los puertos es red: fuera del hilo principal.
            new Thread(this::abrir, "servidor-ea").start();
        }
        return START_STICKY;
    }

    private void abrir() {
        try {
            ServidorEa s = new ServidorEa(new File(getFilesDir(), "servidor_lan"), Ajustes.carpetaLogs(this),
                    () -> hiloPrincipal.post(this::actualizar));
            s.arrancar();
            hiloPrincipal.post(() -> {
                if (enMarcha) {
                    servidor = s;
                    actualizar();
                } else {
                    s.parar();
                }
            });
        } catch (IOException e) {
            // Lo normal: los puertos ya los tiene otro (otra copia del servidor).
            hiloPrincipal.post(() -> {
                Toast.makeText(this, getString(R.string.servidor_error, e.getMessage()),
                        Toast.LENGTH_LONG).show();
                new Ajustes(this).anfitrionLan(false);
                stopSelf();
            });
        }
    }

    private void actualizar() {
        if (!enMarcha) {
            return;
        }
        getSystemService(NotificationManager.class).notify(NOTIFICACION, notificacion());
    }

    private Notification notificacion() {
        String ip = ServidorEa.ipLocal();
        int jugadores = servidor != null ? servidor.jugadores() : 0;
        String texto = ip == null ? getString(R.string.servidor_anfitrion_sin_red)
                : getString(R.string.servidor_notificacion_texto, ip, jugadores);
        PendingIntent abrir = PendingIntent.getActivity(this, 0,
                new Intent(this, SetupActivity.class).setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_IMMUTABLE);
        PendingIntent parar = PendingIntent.getService(this, 1,
                new Intent(this, ServidorLanService.class).setAction(PARAR),
                PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Builder(this, CANAL)
                .setSmallIcon(R.drawable.ic_servidor)
                .setContentTitle(getString(R.string.servidor_notificacion_titulo))
                .setContentText(texto)
                .setContentIntent(abrir)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .addAction(new Notification.Action.Builder(null, getString(R.string.servidor_detener), parar)
                        .build())
                .build();
    }

    @Override
    public void onDestroy() {
        enMarcha = false;
        hiloPrincipal.removeCallbacksAndMessages(null);
        if (servidor != null) {
            ServidorEa s = servidor;
            servidor = null;
            // Cerrar sockets tambien es red.
            new Thread(s::parar, "servidor-ea-parar").start();
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
