# El motor nativo sobre OpenGL ES

Un segundo backend del [motor nativo](motor-nativo.md) para los móviles que no tienen Vulkan, o
cuyo driver de Vulkan falla. **Estado: en obras, pero ya dibuja.** Hechas las fases 1 a 4 (shaders,
presentador, destinos y dibujos), sin las optimizaciones de Vulkan. Con «API gráfica: OpenGL ES
(experimental)» en el lanzador, en el Samsung A22 se ven los vídeos y el menú 3D a 30 fps, y las
carreras a unos 22 fps.

## Decisiones

- **El mismo APK.** GLES es el respaldo: se usa si la sonda de Vulkan falla o si se elige en el
  lanzador. El manifiesto tendrá que dejar de exigir Vulkan (`android.hardware.vulkan.version`
  con `required="true"`).
- **GLES 3.2 como mínimo, y 3.1 donde se pueda.** Con solo arm64, los móviles que
  se quedan fuera de Vulkan no son los que no lo tienen, sino los de Vulkan 1.0 o con drivers
  rotos: Mali-T8xx/G71/G72 y Adreno 5xx. Todos ellos tienen GLES 3.2. Los shaders valen también
  para 3.1.
- **`minSdk 26` (Android 8.0) desde el 2026-10-21**, para llegar a esos móviles (Galaxy S7/S8/Note 8,
  Snapdragon 820/835, Pixel 1), que se quedaron en Android 8 o 9. Lo que pedía más: los motivos de
  cierre (`Cierres`, Android 11) y el recorte de la cámara (Android 9) ya comprueban la versión; el
  aviso de lint en SDL (`SDLGenericMotionListener_API29`) es falso y se ignora en `android/app/lint.xml`.
  **Sin probar en ningún móvil con Android 8 o 9.** Bajar a 24 (Android 7.0) pediría el audio por SDL
  en vez de AAudio (`aaudio_driver.cpp`), *desugaring* de Java (`ServidorEa`) y la vibración con
  comprobación de versión; por debajo, las funciones de GLES 3.2 con `eglGetProcAddress`.
- **Un backend paralelo.** El renderizador ya está detrás de interfaces: `DestinosNativos` es lo
  único que ve `nfsmw_nativo_sistema.cpp`, y `DibujosVulkan`/`ContextoDestinos` es la frontera
  entre C2 (destinos y copias) y C3–C6 (dibujos). El backend de GLES las implementa con otras
  clases, y lo que no toca la API (registros, texturas, vértices, cachés) se saca a ficheros
  comunes. El código de Vulkan, que está muy afinado, no se toca.

## Fases

1. **Shaders.** Hecha (ver abajo).
2. **Presentador.** Hecha (ver abajo).
3. **C2.** Hecha (ver abajo), sin las optimizaciones de Vulkan.
4. **C3–C6.** Hecha (ver abajo), sin las optimizaciones de Vulkan.
5. **Oclusión, FXAA, medidas y overlays.** Hecha (ver abajo), sin probar en el móvil.
6. **Lanzador, manifiesto y APK.** Hecha (ver abajo); falta probarla en el A22 y forzando GLES en el
   RedMagic.

## Fase 1: los shaders

### El problema

La biblioteca de Vulkan (`nfsmw_shaders.nfsp`) lee todas las texturas de unos heaps
(`g_Texture2DDescriptorHeap[i]`, con `RuntimeDescriptorArray`), y el índice sale de las
constantes compartidas. Eso es descriptor indexing, y GLES no lo tiene. Lo demás sí tiene
traducción directa: las constantes ya llegan por tres UBO (el camino de punteros de 64 bits está
muerto en la cabecera del instalador), y la constante de especialización pasa a ser un
`#define`.

### La biblioteca de GLES

[`nativo/tools/biblioteca_shaders_gles.mjs`](../nativo/tools/biblioteca_shaders_gles.mjs) genera
`nfsmw_shaders_gles.nfsp` con los mismos módulos WASM del instalador que la de Vulkan (traductor,
DXC y empaquetador), y el mismo formato NFSSPV, así que `ShadersNativos` la carga igual. Cambia
tres cosas:

- **Una textura por registro de sampler.** La cabecera (`shader_common.h`) se deriva de la del
  instalador: los heaps pasan a ser `g_Tex2D_<r>`, `g_Tex3D_<r>`, `g_TexCube_<r>` (sets 0–2) y
  `g_Sampler_<r>` (set 3), con binding = registro, y sus funciones (`tfetch2D`, `tfetch3D`,
  `tfetchCube`, `getPixelCoord`...) reciben los recursos en vez de índices. Los
  `#define X_Texture2DDescriptorIndex (...)` del traductor se reescriben para que nombren esos
  recursos (el registro sale del byte: dimensión × 64 + registro × 4). Cada reescritura comprueba
  que encontró lo que esperaba, para que un cambio del instalador no pase sin que se note.
- **El bloque compartido, de 31 `float4`.** La cabecera declara 23, pero `1/tamaño` de los
  registros 2 en adelante se lee más allá (byte 360 + registro × 8; el bloque real son 488
  bytes). En Vulkan no se nota, porque el rango enlazado lo cubre; GLSL rechaza un índice
  constante fuera de rango (19 pixel shaders).
- **Sin sombra por mínimo** (`nfsmw_nativo_sombra_minimo`): su pareja llega en la palabra del
  índice 3D del mismo registro, que aquí es una textura 3D. Sin la marca en el SPIR-V, la app no
  la pide. El PCF barato y el interruptor del desenfoque de la composición se aplican igual que
  en Vulkan.

Comprueba también que ningún módulo usa `RuntimeDescriptorArray`, `Int64` ni
`PhysicalStorageBufferAddresses`. `preparar_nativo.py` la genera al lado de la de Vulkan.

### De SPIR-V a GLSL ES, en el móvil

[`nfsmw_gles_shaders.cpp`](../nativo/app/src/nfsmw_gles_shaders.cpp) usa SPIRV-Cross
(`vulkan-sdk-1.4.309.0`, que `preparar_nativo.py` clona en `sdk/thirdparty/spirv-cross`):

- Cada textura con su sampler se convierte en un combined sampler en la unidad
  dimensión × 16 + registro (2D en 0–15, 3D en 16–31 y cubos en 32–47). Ahí el backend pondrá la
  textura del registro y su sampler object. Las texturas de las que solo se pregunta el tamaño
  llevan un sampler de relleno en la misma unidad.
- **La orientación no se toca.** Los vertex shaders ya vienen con `-fvk-invert-y`, y GL pasa de
  NDC a filas del framebuffer, a `gl_FragCoord` y a filas de textura con los mismos números que
  Vulkan: la fila 0 es y = −1 en NDC en los dos. Los render targets se quedan con la
  orientación de D3D, y solo hay que voltear la salida final a la ventana. La profundidad sí
  cambia: Vulkan recorta z a [0, 1] y GL a [−1, 1], así que sin `GL_EXT_clip_control` el vertex
  shader reescribe z (`fixup_clipspace`).
- **Las salidas del vertex shader se recortan a lo que lee el pixel shader.** El traductor
  declara todos los interpoladores que exporta el microcódigo, hasta 22 `vec4` (TEXCOORD0–15 y
  COLOR0–5). GLES garantiza 16, y Mali da justo eso. Al enlazar un programa, el VS se convierte
  con la máscara de entradas de su PS, y las demás salidas pasan a ser variables globales sin
  uso. SPIRV-Cross trae `mask_stage_output_by_location`, pero su backend de GLSL lo ignora, así
  que se hace sobre el texto.

### Comprobado en el PC

[`nativo/tools/tests/gles_shaders_test.cpp`](../nativo/tools/tests/gles_shaders_test.cpp)
convierte la biblioteca entera con ese mismo código. Prueba cada VS con todas las máscaras de PS
que hay, y escribe un `.vert`/`.frag` por shader y una pareja VS/PS compatible para cada PS. La
compilación está en la cabecera de la prueba; con `glslangValidator` (de `sdk/thirdparty/glslang`):

| Edición | Shaders únicos | Compilan (ES 3.20 y 3.10) | Parejas que enlazan |
|---|---|---|---|
| PAL España, Alemania, Italia, Reino Unido | 152 (63 VS, 89 PS) | todos | 89 de 89 |
| USA | 150 (65 VS, 85 PS) | todos | 85 de 85 |
| Japón | 142 (57 VS, 85 PS) | todos | 85 de 85 |

Un PS lee como mucho 11 interpoladores. El conversor también compila con el NDK para arm64.
Todavía no se ha probado ningún shader en un driver de verdad: eso llega con la fase 4.

## Fase 2: el presentador

En el SDK del motor nativo, solo para Android:

- **[`GlesProvider`](../nativo/sdk/include/rex/ui/gles/provider.h)** abre EGL, elige una
  configuración RGBA8 sin profundidad y crea un contexto raíz de OpenGL ES 3.2 (o 3.1) que nunca
  se hace actual. Todos los demás comparten objetos con él: el del presentador y uno por cada hilo
  que dibuja (`CrearContextoHilo`, con `EGL_KHR_surfaceless_context` o un pbuffer de 1×1). Las
  texturas, búferes, programas y objetos de sincronización se comparten; los framebuffers y los
  VAO, no. Al arrancar escribe en el log el driver y lo que tiene (clip_control, timer_query,
  buffer_storage, S3TC/RGTC, límites).
- **[`GlesPresenter`](../nativo/sdk/include/rex/ui/gles/presenter.h)** implementa la interfaz
  del presentador del SDK. La salida del juego es un buzón de tres texturas `GL_RGB10_A2`, como
  las imágenes `A2B10G10R10` del de Vulkan. El hilo que la refresca (el del anillo) dibuja en ella
  desde su contexto, y la sincronización entre contextos va por la GPU con `glWaitSync`: el
  refresco deja un sync al terminar de escribir, que el pintado espera antes de muestrear, y el
  pintado deja otro, que el siguiente refresco de esa textura espera antes de escribir. El
  contexto de pintado se hace actual con la superficie de la ventana en cada pintado y se suelta
  al acabar, porque el pintado puede cambiar de hilo (el hilo propio del presentador o el de la
  UI). La superficie se crea sobre el `ANativeWindow` que da SDL, que no crea la suya porque la
  ventana no lleva `SDL_WINDOW_OPENGL`. Con `eglSwapInterval(1)`, la sincronía vertical es
  implícita.
- Todavía no: los overlays de ImGui (`CreateImmediateDrawer` devuelve nulo, y entonces la app no
  los monta), la captura de la salida y los efectos de FidelityFX (solo bilineal).

En la app: la cvar `nfsmw_nativo_api` (`vulkan` o `gles`, que el lanzador pone con «API
gráfica») elige el proveedor. Con GLES, el hilo del anillo crea y hace actual su propio contexto
al empezar y lo destruye al acabar, y como no hay destinos (`AsegurarDestinos` devuelve falso),
cada Swap pinta el color de prueba (`LimpiarSalidaGles`): el verde sube y baja con cada Swap, y
una franja blanca cubre la octava parte de arriba de la imagen. **Si la franja sale abajo, la
orientación está mal.**

**Probado en el Samsung A22 (Mali-G57 MC2, driver r32p1) el 2026-10-18:** el color de prueba
pulsa, la franja sale arriba, el juego va a 30 Swaps por segundo y la superficie se reconecta al
volver de segundo plano. Lo que dice el driver: GLES 3.2, `EGL_KHR_surfaceless_context`,
`timer_query`, `buffer_storage` y `copy_image` sí; `clip_control`, S3TC y RGTC no (las texturas
BC tendrán que ir descomprimidas en la CPU, y z por `fixup_clipspace`); 128 componentes de salida
del VS, UBO de 64 KB con alineación 16.

Un fallo que salió en la primera prueba: en el modo «presentando en el acto», el presentador
pinta en el mismo hilo que refresca la salida (el del anillo), y el pintado soltaba el contexto
al acabar, así que desde el segundo fotograma el anillo se quedaba sin el suyo y la pantalla
congelada en la primera imagen. Ahora el pintado devuelve al hilo el contexto que tenía.

Solo va en la app de PAL España: los árboles de
las demás ediciones (`app_<edición>`) se regeneran desde `app/` con `preparar_nativo.py`.

## Fase 3: destinos, copias y presentación

[`nfsmw_nativo_destinos_gles.cpp`](../nativo/app/src/nfsmw_nativo_destinos_gles.cpp) implementa
`DestinosNativos`, la misma interfaz que el de Vulkan, con lo necesario para ser exacto:

- Un render target es una textura `GL_RGBA8` o `GL_DEPTH24_STENCIL8` por (base, formato, pitch),
  igual de alta que la de Vulkan, y una textura resuelta, otra por dirección de destino. Cada
  imagen tiene su propio framebuffer, creado una vez: reenganchar uno compartido en cada borrado
  y cada copia costaba 150 µs de CPU por copia en el Mali-G57.
- La copia es `glCopyImageSubData`, o un `glBlitFramebuffer` lineal cuando la escena es más
  grande que la textura (resolución interna por encima de la del frontbuffer), los mismos casos
  que `vkCmdCopyImage` y `vkCmdBlitImage`. Los rectángulos son los de Vulkan tal cual: las filas
  guardan la orientación de D3D.
- Los borrados van por `glClearBuffer`, que obedece a las máscaras y a la tijera; se dejan
  abiertas antes de cada borrado porque los dibujos las cambiarán.
- Las texturas resueltas pequeñas (la exposición) se leen de vuelta con un pixel pack buffer y
  una valla, y se escriben en la memoria del juego cuando la valla se ha señalado, sin esperar a
  la GPU.
- La salida aplica la rampa de gamma del juego y el posproceso con el mismo shader que
  `nfsmw_salida_rampa_gamma.frag`, sin FXAA todavía.
- Lo que no depende de la API (el rectángulo de la copia, las direcciones con mosaico y la
  escritura de texels al juego) está en
  [`nfsmw_nativo_copias_comun.h`](../nativo/app/src/nfsmw_nativo_copias_comun.h), y el de Vulkan
  lo usa desde ahí, con los mismos cuerpos que tenía.

Todavía no: intercambiar imágenes en vez de copiar, las copias aplazadas, el mapa de sombras
escalado y la oclusión por GPU (sin ella, el sistema usa sus cuentas fingidas seguras).

**Probado en el A22 el 2026-10-19**, con `--nfsmw_nativo_diag_borrado=true` (cada render target
se borra con un color fijo): en pantalla sale el color del destino de 1040×1040 (`72a833`, que
la rampa del juego deja en `79ae36`), que es el que el juego resuelve a la textura del Swap
(`09430000`, 1024×576). En 20 s: 1.222 copias, 1.032 borrados, 516 Swaps presentados y 0
rechazos; 94–131 µs de CPU por copia y 2,6 ms por Swap en presentar.

## Fase 4: los dibujos

[`nfsmw_nativo_dibujos_gles.cpp`](../nativo/app/src/nfsmw_nativo_dibujos_gles.cpp) hace el trabajo de
`DibujosVulkan` con las mismas reglas en todo lo que cambia la imagen, en el orden de su `Dibujar`:

- Tipo de primitiva, destinos y motivos para rechazar o saltar un dibujo, leídos de los mismos
  registros (con cuadriláteros convertidos a triángulos y el reinicio de primitivas con
  `GL_PRIMITIVE_RESTART_FIXED_INDEX`).
- Los índices se rebasan al primer vértice en la CPU, así que no hace falta `BaseVertex`.
- La entrada de vértices del VS parcheado es `CalcularEntrada` con formatos de GL (los USCALED de
  Vulkan son atributos sin normalizar, y las entradas `uint4`, `glVertexAttribIPointer`).
- Vértices, índices y los tres bloques uniformes van a un búfer de subida de 64 MB en cuatro
  segmentos con valla: persistente y coherente con `GL_EXT_buffer_storage` (el A22 lo tiene), y si
  no, con `glBufferSubData`. Las constantes se reaprovechan mientras no cambia su generación.
- Los programas salen de la biblioteca GLES con el conversor de la fase 1. El VS se convierte con
  la máscara de entradas de su PS, y sin PS (modo 5, solo profundidad) se usa una etapa de
  fragmentos vacía.
- Texturas: [`nfsmw_nativo_texturas_comun.h`](../nativo/app/src/nfsmw_nativo_texturas_comun.h)
  describe la textura de una fetch constant (tamaños, mips, regiones, cola empaquetada) y la lee
  desenmosaicada al formato del host, con las reglas de `PrepararTextura`. Las BC se descomprimen
  en la CPU; los 16 bits unorm sin `GL_EXT_texture_norm16` se quedan con el byte alto. El swizzle
  de la fetch constant va en los parámetros de swizzle de la textura, y el muestreo, en sampler
  objects (filtros, direccionamiento, mips, borde y anisotropía). Las texturas que no cambian se
  comprueban cada vez menos, hasta una vez cada 32 fotogramas, como en Vulkan. El cubo dinámico se
  rellena con las caras que resuelve C2.
- El estado fijo usa las tablas de `RellenarEstadoFijo` (factores y operaciones de mezcla,
  comparaciones, operaciones de stencil, cull y cara frontal) y solo se aplica lo que cambia. GL y
  Vulkan numeran igual las filas para el mismo NDC, así que el viewport y la tijera no cambian,
  pero **la cara frontal sí se invierte**: el área de un polígono lleva en Vulkan un signo menos
  (para compensar su Y hacia abajo) y en GL no. Ese fallo fue la primera prueba: todos los dibujos
  con cull perdían sus caras visibles y la pantalla salía vacía. Las alturas de viewport negativas,
  que GL no admite, se convierten volteando la Y del NDC y del medio píxel.
- Lo que no depende de la API (aritmética de mips, desenmosaicado rápido, orden de bytes,
  ubicaciones de vértices, remapeos) lo usan ahora también los dibujos de Vulkan desde esa
  cabecera, con los mismos cuerpos.

La biblioteca GLES ya va en el APK (`assets/nativo/nfsmw_shaders_gles.nfsp`, si está en `out/`) y
`MotorNativo` la instala al lado de la de Vulkan; con `nfsmw_nativo_api = gles` el sistema carga
esa.

Al principio no tenía ninguna de las optimizaciones de Vulkan; las que se han portado después están en «Optimizaciones y escalado», más abajo.

**Probado en el A22 el 2026-10-19:** el vídeo de introducción y el menú 3D del modo carrera se ven
bien a 30 fps. En el menú, unos 310 dibujos por fotograma, 0 rechazos, 19 programas, 163 texturas y
ningún error de GL; el hilo del anillo va al 99 % (14 µs por dibujo), así que el límite es la CPU.

### Las sombras en carrera

La primera carrera (A22, 2026-10-19) iba a 22 fps con la escena completa, el retrovisor y el HUD,
pero las sombras temblaban al moverse el coche. No eran las copias (cero errores de GL y cero
avisos del driver con `--nfsmw_nativo_gles_depurar=true`, una cvar nueva que enciende
`KHR_debug`) ni la resolución o el muestreo, que son los mismos que en Vulkan. Faltaba el
desplazamiento de profundidad por pendiente que Vulkan añade a los dibujos del mapa de sombras
cuando el juego no pone el suyo (`nfsmw_nativo_sombras_sesgo_pendiente`, 2,0 por defecto): sin
él, las superficies se hacen sombra a sí mismas (acné). Portado, con las mismas cvars.

Lo que quedaba era de los ajustes, no de GLES: con «Sombras: cada 2 fotogramas» el mapa va un
fotograma por detrás del coche, y con la calidad «Rápida» (PCF barato, un solo texel) los bordes
saltan de texel en texel. Con «cada fotograma» y calidad «Xbox 360» se ve bien.

### La memoria

Tras un rato de carrera el juego se cerraba solo: el sistema lo mataba por memoria (PSS de 2147 MB
en el A22, que tiene 4 GB). Las texturas que el juego había pedido sumaban unos 380 MB, pero el
driver de Mali lleva la cuenta de unos 1,25 GB de memoria de GL (`dumpsys meminfo`, «GL mtrack»):
cada textura le cuesta unas tres veces su tamaño. Vulkan no tiene ese problema porque expulsa las
texturas que no se usan; GLES no lo hacía. Ahora sí, con la misma política
(`nfsmw_nativo_texturas_mb_max`: desde el 75 % del límite, unas pocas de las más viejas sin usar en
120 fotogramas; por encima, un lote hasta el 75 %), y el límite baja según la RAM del móvil: 256 MB
con 4,5 GB o menos, 384 MB hasta 6,5 GB, y la cvar por encima. Con eso el PSS se quedaba en unos
1,97 GB; el límite por RAM está **sin probar**.

Al expulsar, GL puede reutilizar el nombre de una textura borrada: el swizzle que se aplicó a una
textura resuelta va ahora en su `ImagenGles` y no en un mapa por nombre, que se lo daba a otra.

## Fase 5: oclusión, FXAA, medidas y overlays

- **Oclusión.** Las consultas del juego (el destello del sol) van con `GL_ANY_SAMPLES_PASSED`,
  que solo dice si pasó algún fragmento: una visible cuenta 1000 muestras (lo que el sistema
  inventa para «visible del todo») y una tapada, 0, así que el destello se enciende y apaga sin
  fundido. Como en Vulkan, el resultado es el último medido de esa estructura, de un fotograma
  anterior, leído sin esperar a la GPU (`GL_QUERY_RESULT_AVAILABLE`).
- **FXAA** en el shader de salida, con `nfsmw_antialiasing = fxaa` (calidad normal, umbral 0,166,
  mínimo 0,0833), antes de la rampa de gamma, como en Vulkan. El paso es el texel del origen.
- **Tiempo de GPU por fotograma** con `GL_EXT_disjoint_timer_query`: una consulta `TIME_ELAPSED`
  de Swap a Swap, hasta cuatro en vuelo, y se descarta la que coincide con una disyunción.
- **Overlays de ImGui** (el teclado para el nombre del perfil y los mensajes de XAM, los overlays
  de depuración y de ajustes, los logros): `GlesImmediateDrawer` en el SDK
  ([`gles_immediate_drawer.cpp`](../nativo/sdk/src/ui/gles/gles_immediate_drawer.cpp)) dibuja sobre
  la salida dentro del pintado del presentador. Las texturas se crean y se destruyen en el hilo de
  la interfaz, que no tiene contexto de GL: guardan sus píxeles y se suben la primera vez que se
  dibujan, y al destruirse dejan su nombre en una papelera que vacía el siguiente `Begin`. El
  vértice base va en los desplazamientos de los atributos (`glDrawElementsBaseVertex` es de 3.2).

## Fase 6: lanzador, manifiesto y APK

- El APK nativo ya no exige Vulkan en el manifiesto (`vulkanRequerido`, falso con el motor nativo).
- La opción «API gráfica» de los ajustes sale por defecto en OpenGL ES si el móvil no tiene
  Vulkan 1.1 (`FEATURE_VULKAN_HARDWARE_VERSION`), y en Vulkan si lo tiene.
- Si se elige GLES y la edición no trae su biblioteca GLES, se juega con Vulkan y se avisa
  (`gles_no_disponible`, en los 10 idiomas).
- `apk_ediciones.py` mete la biblioteca GLES de cada edición si está en `out/<edición>`.
- Las demás ediciones: su biblioteca GLES se genera con `nativo/tools/biblioteca_shaders_gles.mjs`
  sobre `assets/game_root_<edición>`, y su `app_<edición>` se pone al día con
  `tools/editions/crear_arbol.py` (con las huellas de `huellas_shaders` y sus parejas corregidas),
  sin codegen. **Después hay que rehacer sus copias literales** (`tools/copia_literal.py` sobre su
  código generado, con `--tabla out/<edición>/tabla.tsv`): `crear_arbol.py` copia las de la PAL.

## Optimizaciones y escalado

Lo que faltaba respecto a Vulkan y las limitaciones de la 0.5.3, hecho el 2026-10-19. Compila, y
los shaders nuevos pasan glslang en ES 3.10 y 3.20, pero **nada de esto se ha probado todavía en un
móvil**. Cada parte tiene su cvar para apagarla.

### En la CPU del anillo (el límite en el A22)

- **Vértices sin duplicar** (`nfsmw_nativo_dedupe_vertices`, la de Vulkan): el mismo rango de
  vértices dos veces en un fotograma se copia una vez. La lista se olvida en cada `WAIT_REG_MEM`
  (`g_sincronizaciones_anillo`), como en Vulkan.
- **Cambio de orden de bytes con NEON** al copiar los vértices al búfer de subida, del juego al
  búfer directamente (solo escrituras: puede ser memoria sin caché).
- **Vértice base** (`nfsmw_nativo_gles_base_vertice`, GLES 3.2): los índices de 16 bits sin
  reinicio, el caso normal, van del juego al búfer con NEON (con el guardián de
  `nfsmw_nativo_indices_neon`) y sin reescribirlos, y `glDrawElementsBaseVertex` quita el mínimo.
  Con un solo stream de vértices, su copia va a un múltiplo de su zancada y los atributos se quedan
  apuntando a 0: ya no se vuelven a apuntar en cada dibujo. `Reservar` acepta alineaciones que no
  son potencia de dos y vuelve a alinear al saltar de segmento.
- **Bloques uniformes** y atributos: solo se vuelven a enlazar si cambia su sitio.
- **Texturas por registro** (`nfsmw_nativo_gles_memo_texturas`): si un registro tiene la misma fetch
  constant que en su dibujo anterior del mismo fotograma, y C2 no ha copiado nada entre medias, se
  reutiliza su textura y su sampler sin buscarlos. Se recuerda dónde está el swizzle de la textura,
  por si otro registro le puso otro en medio.
- **Sombras sin vegetación** (`nfsmw_sombras_sin_vegetacion`, que la calidad de sombras «Xbox 360»
  apaga) y **dibujos invisibles** (`nfsmw_nativo_saltar_invisibles`), con las condiciones de Vulkan:
  en Vulkan la vegetación era el 31 % de los dibujos que llegaban y 3-5 ms de CPU por fotograma.

### En la GPU

- **Z temprana** (`nfsmw_nativo_z_temprana`, con las condiciones de Vulkan): los shaders que pueden
  descartar en dibujos que prueban la profundidad y no la escriben llevan
  `layout(early_fragment_tests)`. Es otra variante del programa.

### Los tirones: programas en disco

`nfsmw_nativo_gles_cache_programas`: cada programa enlazado se guarda con `glGetProgramBinary` en
`cache/nfsmw_nativo_programas_gles.bin` (al lado del fichero de pipelines de Vulkan), con su lista de
samplers, y en las partidas siguientes se carga con `glProgramBinary` sin convertir, compilar ni
enlazar. La clave es el SPIR-V de los dos shaders, la especialización y las variantes, así que una
biblioteca nueva da claves nuevas sola; la cabecera lleva el driver (`GL_VENDOR`, `GL_RENDERER`,
`GL_VERSION`), y otro driver empieza de cero. En memoria solo está el índice. Hay que subir
`kVersionProgramas` si cambia el GLSL que sale de `nfsmw_gles_shaders.cpp`. La primera partida
sigue teniendo los tirones.

### El destello del sol con fundido

Cada consulta de oclusión lleva además un contador de muestras: un búfer de contador atómico de
4 bytes. Los dibujos de la consulta usan una variante del programa con la prueba de profundidad
antes del shader y un `atomicCounterIncrement` al final de `main` (el `main` del shader pasa a
llamarse `nfsmw_main`): un fragmento que descarta no llega, y uno tapado ni se sombrea. Sale la
cuenta de la 360 y no solo «sí o no». Un dibujo cuya prueba temprana daría otro resultado (descarta
y escribe profundidad o estencil, o el shader escribe la profundidad) deja la consulta en sí o no.
Sin contadores atómicos en los pixel shaders (`GL_MAX_FRAGMENT_ATOMIC_COUNTERS` = 0), o si el driver
da 0 con la consulta visible ocho veces, todo vuelve al sí o no.

### Escalado: FSR 1.0 y CAS

El presentador hace los efectos de Vulkan: bilineal, AMD CAS (nitidez o reescalado) y FSR 1.0
(EASU + RCAS), con y sin tramado, encadenados en texturas intermedias `GL_RGB10_A2`. Los shaders son
los mismos de Vulkan (`sdk/src/ui/shaders/vulkan_spirv`) pasados a GLSL ES 3.10 una vez en el PC con
[`nativo/tools/gles_efectos_presentador.cpp`](../nativo/tools/gles_efectos_presentador.cpp) (SPIRV-Cross),
que escribe `sdk/src/ui/gles/gles_presenter_efectos.h`. Cada uno sale en dos variantes: a una
textura, tal cual (GL y Vulkan numeran igual las filas de una textura), y a la ventana, con la y de
`gl_FragCoord` volteada con el uniforme `nfsmw_alto`, porque la fila 0 de la ventana es la de
abajo. Las constantes son las estructuras de `Presenter` (`FsrEasuConstants`...). Si un efecto no
compila, se pinta en bilineal el resto de la sesión.

### El reflejo de la carretera bajo demanda

`nfsmw_reflejo_bajo_demanda` (en `nfsmw_recortes_carrera.cpp`) solo dibuja el reflejo de la
carretera si algo lo ha leído hace poco, y `nfsmw_reflejo_visibilidad`, solo si el agua que lo lee
deja muestras en pantalla. En Vulkan ahorra unos 210 dibujos por fotograma (hasta 338 en Heritage
& Omega) donde no se ve agua. Necesita que el renderizador le diga cuándo se resuelve el reflejo a
`0x07C5A000`, cuándo se lee y cuándo hay Swap, y **GLES no se lo decía**: su guardián no veía
ninguna copia en los primeros fotogramas, se apagaba («APAGADO… se dibuja siempre») y con GLES el
reflejo se dibujaba en todos los fotogramas.

Ahora `DestinosGles` avisa del Swap, de la copia y de cada lectura (en `TexturaResuelta`, que los
dibujos consultan al menos una vez por fotograma), y los dibujos miden la visibilidad como Vulkan:
una consulta `GL_ANY_SAMPLES_PASSED_CONSERVATIVE` propia alrededor de cada dibujo que muestrea el
reflejo (la conservadora puede decir «alguna» sin haberla, que deja el reflejo dibujado, pero nunca
«ninguna» habiéndola), y cada 32 fotogramas el testigo: la composición final, que cubre la pantalla
y tiene que dejar muestras. Con una consulta del juego abierta no se mide y cuenta como visible.
Los resultados se leen sin esperar al final de cada fotograma. El informe `C6 GLES ahorros` dice
cuántas consultas hubo y cuántas salieron sin muestras. Sin probar en un móvil.

### El cielo aplazado

`nfsmw_nativo_cielo_aplazado`, con el criterio y el guardián de Vulkan: el pixel shader del cielo
(por la huella de su contenedor, que `crear_arbol.py` traduce en USA y Japón), los 480 índices de la
cúpula, primer dibujo del pase de la escena, opaco, que prueba la profundidad sin escribirla, sin
estencil y sin consulta de oclusión del juego abierta; y solo después de 90 fotogramas de carrera con
exactamente una cúpula por fotograma, nunca dos. En Vulkan son 3,4–4,3 ms de GPU por fotograma.

GL dibuja en el momento, así que el final de `Dibujar` se partió en dos: el dibujo ya calculado
(`DibujoGles`: programa, framebuffer, estado fijo, texturas, bloques uniformes, atributos y la
llamada) y `Emitir`, que lo aplica y lo dibuja. El cielo se guarda y se emite más tarde por el mismo
camino:

- al llegar un dibujo con mezcla, uno que no escribe Z o uno sin color (más estricto que Vulkan en
  este último: no se le deja pasar por delante);
- al cambiar de framebuffer, y antes de cada copia o borrado de C2, de cada Swap y de cada consulta
  de oclusión del juego (`DibujosGles::TerminarPase`, que llama `DestinosGles`);
- antes de que el búfer de subida pueda volver al segmento con sus datos (se apunta el segmento más
  viejo que lee, contando los vértices reutilizados y las constantes);
- con los swizzles de sus texturas comprobados otra vez, por si otro registro los cambió.

El informe `C6 GLES cielo aplazado` sale cada 1800 fotogramas.

### El informe de fases

`nfsmw_nativo_gles_etapas` (apagado) cronometra cada dibujo por fases (entrada, destinos, índices,
vértices, texturas, encuadre, programa, subidas, estado y la llamada de dibujo) y las copias y los
Swaps de C2, y lo escribe cada 600 fotogramas como `C6 GLES etapas` y `C2 GLES etapas`. Es lo que
falta para decidir el siguiente ahorro con números del A22.

### BC1 a 16 bits

Sin BC en la GPU (Mali y PowerVR, en GLES y en Vulkan) las texturas BC se descomprimen en la CPU, y
antes iban todas a RGBA8. Las BC1, las más comunes, van ahora a 16 bits por texel
(`nfsmw_nativo_bc1_16_bits`, encendido): la mitad de memoria y de ancho de banda. BC1 guarda sus
extremos en 5:6:5 y su alfa es de un bit, así que solo se pierde el redondeo de los colores
intermedios.

- **GLES:** `GL_RGB565`. Si al subirla resulta que usa su alfa, se rehace como `GL_RGB5_A1` (el
  almacenamiento es inmutable).
- **Vulkan:** `VK_FORMAT_A1R5G5B5_UNORM_PACK16` (de soporte obligatorio, pero se comprueba), sin
  saber de antemano si tiene alfa. No con el hilo de huellas, que da los datos en RGBA8.

`EmpaquetarA16` está en `nfsmw_nativo_texturas_comun.h` y se ha comprobado en el PC con valores
calculados a mano.

### Media precisión

`nfsmw_nativo_media_precision` (apagado; Vulkan y GLES): al cargar la biblioteca, cada pixel shader
lleva `RelaxedPrecision` en la matemática de color (`nfsmw_spirv_precision.cpp`): los resultados de
muestrear una textura y toda la aritmética que sale de ellos. Lo que acaba en una coordenada de
textura, una referencia de profundidad, un operando de imagen o `gl_FragDepth` vuelve a 32 bits con
todo lo que lo alimenta, y lo que no viene de una textura (posiciones, niebla, constantes) no se
toca. Los drivers de Vulkan lo calculan a 16 bits y SPIRV-Cross lo escribe como `mediump`. Mali y
Adreno hacen el doble de cuentas así. Comprobado en el PC: los 89 pixel shaders de las bibliotecas
de Vulkan y de GLES pasan `spirv-val` (Vulkan 1.2) antes y después, y en GLES las 89 parejas
compilan y enlazan con glslang en ES 3.10 y 3.20 (unos 4.000 resultados marcados). Puede dar bandas
en algún efecto: hay que verlo en el móvil antes de encenderlo de fábrica.

### Lo que sigue sin portar

El pixel shader sin escrituras de color en los pases sin color (`nfsmw_nativo_ps_solo_alfa`) y los
hilos de huellas de texturas y de copias.
