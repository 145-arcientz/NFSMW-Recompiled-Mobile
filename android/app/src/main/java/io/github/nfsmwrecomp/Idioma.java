package io.github.nfsmwrecomp;

import android.content.Context;
import android.content.res.Configuration;
import android.content.res.Resources;

import java.util.Locale;

/**
 * El idioma de la app: el del telefono, o uno forzado desde la pantalla de
 * inicio (los textos estan en res/values*; los de italiano, aleman, frances,
 * polaco, japones, coreano y chino tradicional salen de values/strings.xml con
 * las mismas claves).
 *
 * Cada actividad lo aplica en attachBaseContext. Ahi todavia no hay Intent,
 * asi que GameActivity, que vive en otro proceso, lo lee de las preferencias
 * igual que la pantalla de inicio.
 */
final class Idioma {

    /**
     * "" = el del telefono. Despues, por el nombre de cada idioma en su lengua:
     * los de alfabeto latino por orden alfabetico y luego japones, coreano y
     * chino tradicional. NOMBRES va en el mismo orden.
     */
    static final String[] CODIGOS = {"", "de", "en", "es", "fr", "it", "pl", "pt", "ja", "ko", "zh-TW"};
    static final int[] NOMBRES = {R.string.idioma_auto, R.string.idioma_de, R.string.idioma_en,
            R.string.idioma_es, R.string.idioma_fr, R.string.idioma_it, R.string.idioma_pl,
            R.string.idioma_pt, R.string.idioma_ja, R.string.idioma_ko, R.string.idioma_zh_tw};

    private static final String CLAVE = "idioma";

    private Idioma() {}

    static String elegido(Context ctx) {
        return ctx.getSharedPreferences(Ajustes.PREFS, Context.MODE_PRIVATE).getString(CLAVE, "");
    }

    /**
     * Con commit() y no apply(): la partida arranca en otro proceso y lee el
     * fichero de preferencias al empezar; tiene que estar ya en disco.
     */
    static void elegir(Context ctx, String codigo) {
        ctx.getSharedPreferences(Ajustes.PREFS, Context.MODE_PRIVATE)
                .edit().putString(CLAVE, codigo).commit();
    }

    static Context envolver(Context base) {
        String codigo = elegido(base);
        if (codigo.isEmpty()) {
            // Por si antes se forzo otro en este mismo proceso.
            Locale.setDefault(Resources.getSystem().getConfiguration().getLocales().get(0));
            return base;
        }
        Locale locale = Locale.forLanguageTag(codigo);
        Locale.setDefault(locale);
        Configuration conf = new Configuration(base.getResources().getConfiguration());
        conf.setLocale(locale);
        return base.createConfigurationContext(conf);
    }
}
