# Audio en Android

La salida del juego convierte los seis canales Xbox a estéreo, a 48 kHz. La bomba
SDL pide bloques de 256 muestras cada 5,33 ms y mantiene una reserva de 12 bloques
(64 ms). La salida puede consumir varios bloques por petición sin imponer ese
ritmo por ráfagas al servidor de audio del juego. El bloqueo de la cola se mantiene
solo para retirar o devolver buffers, dejando que el productor la rellene durante
la conversión y el envío a SDL.

Los ajustes `audio_sdl_bomba = true` y `audio_sdl_bomba_cola = 12` son los valores
predeterminados de Android. `audio_maxqframes` sigue en 12. La reserva agrega
latencia y absorbe retrasos breves; cargas más largas todavía pueden agotar la cola.
El registro `[audio] SDL en 10 s` cuenta bloques entregados y silencios por falta de
datos. No detecta silencios que ya vengan dentro de una mezcla del juego.

La mezcla estéreo de Android conserva sus picos flotantes hasta el limitador.
Recortar antes de limitar destruía esos picos y podía alterar el balance estéreo
con volúmenes altos. El limitador mantiene ambos canales enlazados, limita a 0,97
y recupera ganancia en unos 80 ms. La recuperación usa la frecuencia de la fuente,
también en las películas de 44,1 kHz. Las otras salidas conservan el recorte previo
que ya utilizaban.

Las películas tienen una pista SDL independiente, decodificada con FFmpeg. Se
admiten WMA Pro y WMA v2: el archivo `ealogo` de la edición PAL española usa WMA v2,
aunque las otras intros usan WMA Pro. Mientras la pista nativa suena se silencia
la salida del mezclador del juego para evitar superposición y ruido. Se sigue
consumiendo esa mezcla y devolviendo buffers. La salida del juego se restaura al
vaciar la pista, destruir la película o transcurrir 800 ms sin pedir fotogramas.
El registro indica cuándo se silencia y restaura.

La prueba `tools/tests/audio_output_test.cpp` verifica la conversión de los canales
Xbox, picos por encima del margen anterior, el balance estéreo tras limitar, el
paso de sonidos suaves, la continuidad entre bloques y la recuperación a 44,1 y
48 kHz. Se ejecutó en ARM64 Android; la compilación Release también pasó.
Para comprobar la reproducción, escuchar una intro, saltar una cinemática y
entrar a una carrera; revisar los contadores y la restauración de la salida.

Las cabeceras WAVE del contenedor aportan tambi�n el bitrate y los bits de la
fuente a FFmpeg. El logo EA usa WMA v2 a 192 kb/s: con el bitrate predeterminado,
26 de sus 28 paquetes fallaban al decodificar.

La correcci�n de las intros est� en el enlace Android: `libmain` y `rexruntime`
contienen cada una FFmpeg y sus tablas FFT privadas. Los s�mbolos C ya estaban
ocultos, pero las funciones NEON en ensamblador se exportaban desde el runtime.
As�, el reproductor inicializaba sus tablas y llamaba a funciones del runtime,
que consultaban otras tablas a�n sin inicializar. Se ocultan los s�mbolos de
ambos archivos est�ticos FFmpeg con `--exclude-libs` en las dos bibliotecas.
Esto conserva NEON y evita depender del orden en que se reproduzcan sonidos.

`python tools/tests/android_ffmpeg_bindings_test.py <APK>` verifica que las dos
bibliotecas no importen ni exporten DSP interno de FFmpeg. La prueba falla con
el APK anterior y pasa con el corregido. En el S25 Ultra, la captura de los
primeros cinco segundos de PSA y del logo EA coincide exactamente con la
misma decodificaci�n fuera del juego (error m�ximo 0). El usuario confirm� que
las intros ya suenan bien. Se retir� la captura temporal de audio del APK final.
