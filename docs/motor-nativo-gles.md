# El motor nativo sobre OpenGL ES

Un segundo backend del [motor nativo](motor-nativo.md) para los móviles que no tienen Vulkan, o
cuyo driver de Vulkan falla. **Estado: en obras.** Hecha la fase 1 (los shaders); el renderizador
todavía no existe.

## Decisiones

- **El mismo APK.** GLES es el respaldo: se usa si la sonda de Vulkan falla o si se elige en el
  lanzador. El manifiesto tendrá que dejar de exigir Vulkan (`android.hardware.vulkan.version`
  con `required="true"`).
- **GLES 3.2 como mínimo, y 3.1 donde se pueda.** Con `minSdk 29` y solo arm64, los móviles que
  se quedan fuera de Vulkan no son los que no lo tienen, sino los de Vulkan 1.0 o con drivers
  rotos: Mali-T8xx/G71/G72 y Adreno 5xx. Todos ellos tienen GLES 3.2. Los shaders valen también
  para 3.1.
- **Un backend paralelo.** El renderizador ya está detrás de interfaces: `DestinosNativos` es lo
  único que ve `nfsmw_nativo_sistema.cpp`, y `DibujosVulkan`/`ContextoDestinos` es la frontera
  entre C2 (destinos y copias) y C3–C6 (dibujos). El backend de GLES las implementa con otras
  clases, y lo que no toca la API (registros, texturas, vértices, cachés) se saca a ficheros
  comunes. El código de Vulkan, que está muy afinado, no se toca.

## Fases

1. **Shaders.** Hecha (ver abajo).
2. **Presentador.** Una subclase de `rex::ui::Presenter` con EGL sobre el `ANativeWindow` y un
   contexto compartido con el hilo del anillo. Los overlays de imgui pueden quedarse fuera al
   principio.
3. **C2.** Render targets como texturas con FBO, resolves con `glCopyImageSubData` o
   `glBlitFramebuffer`, el intercambio de imágenes al resolver, los borrados con tijera, y tres
   ranuras con `glFenceSync`.
4. **C3–C6.** Dibujos, VAOs, UBOs por ranura, sampler objects y el estado de blend, profundidad y
   stencil. Las texturas BC se descomprimen en la CPU con el código que ya existe
   (`nfsmw_texturas_bc.h`).
5. **Vídeo, oclusión y medidas.** GLES solo tiene `ANY_SAMPLES_PASSED`, que dice sí o no pero no
   cuántas muestras pasaron: las cuentas que recibe el juego serán aproximadas. Los timestamps
   irán con `EXT_disjoint_timer_query` si el driver lo tiene, y sin estadísticas de pipeline.
6. **Lanzador, manifiesto, APK y pruebas** en el A22 (Mali-G57) y forzando GLES en el
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
