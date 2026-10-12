# Diagnostico grafico: Galaxy A55

Prueba local del 30 de septiembre de 2026. Correccion incluida en v0.3.4.

## Dispositivo y sintomas

- Samsung SM-A556E, Android 16, GPU Samsung Xclipse530.
- Vulkan 1.3.279, controlador SamsungProprietary 24.0.539.
- El usuario informa buen rendimiento, pero texturas y colores incorrectos.
- Capturas por ADB muestran bloques y manchas en los reflejos de la carroceria
  tanto en el menu como en una carrera. La geometria y el texto son legibles.
- La copia de los archivos del juego se verifico por rutas y tamanos: 50
  archivos, 7.009.137.083 bytes. Se comprobo tambien el SHA256 de default.xex,
  nfsmw_shaders.nfsp y el video PSA usado en las pruebas de sonido.

## Capacidades comprobadas en el telefono

Una herramienta Vulkan independiente consulta los formatos y crea imagenes
con los usos SAMPLED y TRANSFER_DST, 512x512 y seis niveles mip.

| Formato | Imagen compatible | Alineacion de memoria |
| --- | --- | --- |
| RGBA8, RGB10A2 | Si | 65536 bytes |
| BC1, BC2, BC3 | Si | 65536 bytes |
| BC4, BC5 | No: VK_ERROR_FORMAT_NOT_SUPPORTED | N/A |
| RG16F, RGBA16F | Si | 65536 bytes |

`textureCompressionBC` es falso, pero BC1/2/3 estan disponibles individualmente.
No se debe desactivar toda la compresion BC basandose solo en ese indicador.
La ausencia de BC4/5 no demuestra que sea la causa de esta escena: falta
confirmar que el juego los usa en los dibujos afectados.

Las imagenes probadas no requieren ni prefieren asignacion dedicada. No se
encontro una diferencia entre su alineacion y la unidad de 64 KB del pool.

## Primera prueba: dependencias de memoria

El renderizador nativo mantiene las imagenes en GENERAL y los pases no tenian
dependencias explicitas con las copias y lecturas siguientes. Los comandos de
subida y trabajo se envian juntos, pero el orden de envio no sustituye las
dependencias de memoria.

Se agregan dependencias externas de entrada y salida al pase, una dependencia
antes de las subidas para las lecturas/escrituras de envios anteriores, y otra
al terminar las subidas para publicar las texturas y caras de los reflejos.
El ajuste `nfsmw_nativo_sincronizacion_gpu` permite comparar con la ruta previa;
es de inicio y requiere reiniciar el juego. No modifica los archivos del juego.

Referencia: [ejemplos de sincronizacion de Khronos](https://docs.vulkan.org/guide/latest/synchronization_examples.html).

## Resultado

El APK compilo, se instalo encima de v0.3.3 y arranco en el A55 con el ajuste
de sincronizacion activado. La captura posterior mostro la carroceria sin las
manchas de la prueba anterior. El usuario confirmo: "ahora si funciona bien"
y pidio publicar la correccion.

La comprobacion de enlaces internos de FFmpeg sigue pasando. No se modifico
el audio ni los shaders del juego. La prueba funcional de v0.3.4 corresponde
al Galaxy A55; el Galaxy S25 Ultra se habia comprobado con v0.3.3. No se ha
medido el coste de estas dependencias en todos los controladores ni se ha
validado la compatibilidad de otros modelos de GPU.
