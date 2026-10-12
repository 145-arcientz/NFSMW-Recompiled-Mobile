<div align="center">

# NFSMW Android Evolved

**Need for Speed: Most Wanted (2005) para Android ARM64**

APK nativo · Vulkan · controles táctiles · generación local de shaders

[Descargar la última versión](https://github.com/codepdbh/nfsmw-android/releases/latest)

</div>

Este proyecto adapta a Android el trabajo de recompilación de [nfsmw-nx](https://github.com/StevensND/nfsmw-nx) y el SDK [ReXGlue](https://github.com/rexglue/rexglue-sdk). La aplicación usa código C++ recompilado, SDL3 y el renderizador nativo sobre Vulkan. No emula una Xbox 360 ni incluye los archivos del juego.

## Requisitos

- Android 8 o posterior y procesador ARM64 compatible con ARMv8.0 o posterior (desde v0.3.5).
- GPU con Vulkan y un controlador compatible con alguno de los renderizadores del port. Se han probado un **Samsung Galaxy S25 Ultra** (audio y juego en v0.3.3), un **Samsung Galaxy A55 con Xclipse 530** (corrección gráfica en v0.3.4) y un **Redmi Note 8 con Adreno 610** (modo de compatibilidad en v0.3.5, con mucha lentitud). El soporte de otros teléfonos, incluidos Helio G99/G200, sigue pendiente de pruebas.
- Una copia propia de **Need for Speed: Most Wanted (2005), Xbox 360, edición PAL España**, extraída y con `default.xex`, `NFS/` y `Movies/`. Este APK se compila para esa edición. Los archivos de la versión de PC, PS2 o una ISO sin extraer no sirven para estos pasos.
- Espacio en la memoria interna para el APK, la carpeta completa del juego y los archivos generados. Si importas una carpeta que ya está en el teléfono, necesitas espacio para una copia adicional durante la importación.

## Instalación y primer inicio

1. Descarga el archivo APK de la última publicación en [Releases](https://github.com/codepdbh/nfsmw-android/releases/latest) y ábrelo en el teléfono. Si Android lo solicita, permite **Instalar aplicaciones desconocidas** al navegador o gestor de archivos que estés usando.
2. Instala el APK y abre **Need for Speed Most Wanted**. Autoriza el acceso a archivos que solicita la app; en Android 11 o posterior aparece el ajuste de **acceso a todos los archivos**. Después vuelve al launcher.
3. Copia `default.xex`, `NFS/` y `Movies/` directamente dentro de `Memoria interna/nsfmw-androidevolved/`. También puedes pulsar **Elegir carpeta del juego** y seleccionar la carpeta extraída que contiene esos tres elementos; la app la copia a ese destino.
4. Comprueba que el launcher marque los archivos como disponibles y pulsa **Jugar**. En modo **Nativo**, la primera vez genera `nfsmw_shaders.nfsp` a partir de tu copia y muestra el avance; espera a que termine. Puede tardar varios minutos. Si la GPU carece de las funciones necesarias, el launcher ofrece **Probar compatibilidad**. También puedes seleccionar **Renderizador → Compatibilidad · experimental**; este modo no necesita esa biblioteca.
5. Se abre el juego en horizontal. Los siguientes inicios en modo nativo usan la biblioteca generada. En teléfonos compatibles con ese modo puedes empezar con 1280×720 y 60 FPS. El modo de compatibilidad puede tener errores, pausas o bloqueos y no garantiza un rendimiento jugable.

La carpeta debe quedar así:

```text
Memoria interna/nsfmw-androidevolved/
├── default.xex
├── NFS/
├── Movies/
└── nfsmw_shaders.nfsp   (generado por la app para el modo nativo)
```

No descargues ni compartas archivos del juego. La biblioteca de shaders se genera en el dispositivo desde los archivos locales de tu copia.

### Actualizar desde una versión anterior

Instala el nuevo APK encima del anterior, **sin desinstalar ni borrar los datos de la app**. Las versiones publicadas en este repositorio usan la misma firma y la actualización conserva las partidas y los ajustes. Si ya tienes los archivos del juego y los shaders, no hace falta importarlos ni generarlos otra vez.

### Si no aparece Jugar

- Revisa el permiso de archivos y vuelve a abrir la app.
- Comprueba que `default.xex` esté directamente en `nsfmw-androidevolved/`, junto a `NFS/` y `Movies/`. Evita una carpeta adicional como `nsfmw-androidevolved/Need for Speed Most Wanted/default.xex`.
- Si la importación falla, revisa el espacio libre y selecciona la carpeta que contiene los tres elementos, no la carpeta `NFS` por separado.
- Si Android rechaza la actualización por una firma diferente, la instalación anterior procede de otra compilación. Conserva tus partidas antes de cambiar de instalación.

## Launcher, ajustes y controles

En el launcher puedes cambiar resolución interna, límite de FPS, antialiasing, sombras, reflejos del coche y del asfalto, resplandor del cielo y filtro de imagen. La app también guarda ajustes de controles y formato de pantalla.

El juego se abre en horizontal. La superposición táctil incluye dirección, botones de acción, START, freno y acelerador. Desde el editor de controles puedes mover y redimensionar botones, ocultarlos y ajustar su opacidad. También se admiten mandos Bluetooth y USB.

Desde v0.3.5, el joystick también responde al dedo si está activado **Inclinar**: al soltarlo vuelve el control por inclinación. El modo de compatibilidad conserva el camino gráfico original del juego; algunos ajustes específicos del renderizador nativo no se aplican en él.

## Compatibilidad en prueba: v0.3.6-experimental

La nueva compilación de prueba adapta el renderer **Nativo** a controladores sin texturas BC1–BC5:
convierte los formatos que falten en CPU y conserva sus niveles de detalle, transparencia y cubos.
También separa los requisitos nativos de los del backend Xenos: la ausencia de
`vertexPipelineStoresAndAtomics` ya no bloquea el inicio nativo en Android.
Los requisitos de los shaders nativos, incluidos Vulkan 1.2, `shaderInt64` y los descriptores,
siguen siendo necesarios; esta adaptación no habilita el renderer nativo en todos los teléfonos.

En **Estabilidad gráfica → Automática · protección Mali**, Mali evita las consultas de oclusión
del sol y de los reflejos. Conserva los reflejos mediante lecturas y omite el destello solar.
**Máxima compatibilidad** aplica esta alternativa en cualquier GPU; **Efectos completos**
permite probar las consultas originales y puede reintroducir cierres en Mali.
Estos ajustes se aplican al renderer **Nativo**. Xenos conserva sus requisitos propios.

Las pruebas de conversión pasan en PC y ARMv8.0. En el Redmi Note 8 con Vulkan 1.1.128 se
verificó la subida y lectura de los cinco formatos convertidos, con seis caras y cuatro mips.
Esto valida la ruta de texturas, no el juego completo ni su rendimiento.
La ejecución completa en Mali-G615 y Mali-G715 sigue pendiente de testers.
El APK está disponible como [versión experimental en Releases](https://github.com/codepdbh/nfsmw-android/releases/tag/v0.3.6-experimental).
Instálalo encima del anterior y selecciona **Renderizador → Nativo** y
**Estabilidad gráfica → Automática · protección Mali** para probar esta adaptación.

## Compatibilidad e informes en v0.3.5

Se añade **Renderizador → Compatibilidad · experimental**, que consiguió reproducir las intros y llegar a conducir en el Redmi Note 8 probado. Se corrige una reserva de memoria que causaba cierres en su kernel antiguo. Persisten lentitud y esperas de la GPU; todavía no se ha comprobado una carrera completa ni el soporte de Helio G99/G200.

El botón **Enviar crash o log** permite compartir un ZIP por correo a `daniebatuani@gmail.com`, guardarlo o abrir un issue en GitHub. En GitHub debes adjuntar el ZIP al formulario. Consulta [la guía para testers](docs/android-testers.md).

## Gráficos en v0.3.4

Se corrigieron bloques, manchas y reflejos incorrectos en la carrocería que aparecían tanto en el menú como durante las carreras del Galaxy A55. El renderizador ahora sincroniza las copias de texturas y sus lecturas entre pases de Vulkan. La mejora se comprobó en el teléfono y fue confirmada por su usuario. La corrección se activa automáticamente y no requiere cambiar los archivos del juego ni regenerar los shaders. Consulta [el diagnóstico de Xclipse](docs/android-xclipse-diagnostic.md) para los detalles de la prueba.

## Audio desde v0.3.3

Se corrigió el ruido de los logos y los videos iniciales, incluida la voz de la chica, y el audio doble de las cinemáticas de historia. También se ajustó la salida del juego para reducir cortes y distorsión. La reproducción de intros, cinemáticas de historia y gameplay se comprobó en un Galaxy S25 Ultra con la edición PAL española. Las pruebas técnicas están descritas en [Audio en Android](docs/android-audio.md).

## Compilar

El código para Android admite ARMv8.0 desde v0.3.5; el APK v0.3.4 requiere ARMv8.2-A.

Requisitos: Android SDK, NDK `28.2.13676358`, JDK 17 o posterior y PowerShell.

```powershell
.\build_android.ps1
```

El APK Release se genera en `android/app/build/outputs/apk/release/app-release.apk`. La compilación usa optimización nativa Release y está configurada para `arm64-v8a`.

## Proyecto y licencias

- `android/`: launcher, integración SDL y build Android.
- `app/`, `sdk/`: aplicación recompilada y ReXGlue.
- `docs/`: notas del port y compilación.

Los archivos del juego y el código generado desde `default.xex` se mantienen fuera de Git. Consulta [`LICENSE`](LICENSE) y [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) para las licencias. Proyecto de aficionados, sin afiliación con Electronic Arts; “Need for Speed” es una marca de Electronic Arts Inc.
