# NFSMW Recompiled para iOS

**Estado: fase 0 superada** (2026-10-22, iPhone 13 Pro con iOS 26.6.2 y Apple ID gratuita). El
motor todavía no se ha compilado para iOS.

El port reutiliza el motor nativo de Android tal cual (`../nativo/`: el SDK, el código del juego
recompilado y el renderizador). Solo cambia lo que rodea al motor: el lanzador, que en Android es
Java y aquí será Swift, la compilación y la firma.

## Por qué es posible

- **No hace falta JIT.** El juego está recompilado a C++ antes de compilar, así que en iOS es una app
  normal. Es lo que impide a la mayoría de emuladores.
- **El SDK ya conoce Apple.** `REX_PLATFORM_MAC` (en iOS también vale, porque `TARGET_OS_MAC` es 1),
  la capa POSIX, la memoria con `shm_open` (`nativo/sdk/src/core/memory_posix.cpp`) y MoltenVK
  (`nativo/sdk/src/ui/vulkan/vulkan_moltenvk.cpp`) para Vulkan sobre Metal.
- **SDL3 funciona en iOS** (ventana, mandos y audio).

## Lo que no se puede

- **El backend GLES no sirve.** iOS se quedó en OpenGL ES 3.0 y el nuestro pide 3.1/3.2: en iOS el
  renderizador es el de Vulkan, a través de MoltenVK.
- **No puede ir a la App Store**, porque lleva código del juego. Cada persona compila e instala la
  suya, con su ISO.
- **Con Apple ID gratuita**, la app caduca a los 7 días y hay que volver a instalarla. Los permisos
  de memoria (`increased-memory-limit`, `extended-virtual-addressing`) firman pero no se aplican:
  iOS deja unos 3 GB y la reserva del juego cabe sin ellos (ver la fase 0).

## Herramientas: xtool en WSL

Se compila y se firma desde Windows con [xtool](https://github.com/xtool-org/xtool), sin Mac. xtool
corre en WSL, no en Windows directamente. Hay que instalar, una vez:

1. **WSL** (PowerShell como administrador): `wsl --install`, reiniciar, y crear el usuario de Ubuntu.
2. **El iPhone dentro de WSL**: [usbipd-win](https://learn.microsoft.com/windows/wsl/connect-usb)
   (`winget install usbipd`, y luego `usbipd bind` y `usbipd attach --wsl` con el iPhone).
3. En Ubuntu: **Swift 6.4** (de swift.org/install/linux) y `sudo apt-get install usbmuxd
   libimobiledevice-utils`.
4. **El `Xcode.xip`** (Xcode 27), bajado con el navegador desde
   https://developer.apple.com/download/all/?q=Xcode con tu Apple ID. Con `curl` no vale.
5. **xtool**: el AppImage de sus releases en `/usr/local/bin/xtool`, y `xtool setup`. Con la Apple ID
   gratuita se elige el modo **1: Password**, que usa APIs privadas de Apple. Su guía recomienda una
   Apple ID de usar y tirar. Después se le da la ruta del `Xcode.xip`.

Para comprobarlo: `swift sdk list` tiene que decir `darwin`, e `ideviceinfo` tiene que ver el iPhone.

### Instalar se cuelga en `Connecting`

Por usbip, `xtool dev` firma pero se queda en `Connecting`, y después todo da `Mux error (-8)`. El
log (`sudo journalctl -u usbmuxd`) dice `message was too large` o `expected 49152 bytes, received
58008`: usbip pierde el paquete vacío (ZLP) que cierra una transferencia de tamaño múltiplo de 512
bytes, y el iPhone junta dos mensajes. No lo arregla actualizar usbmuxd; hay que compilarlo de
[git](https://github.com/libimobiledevice/usbmuxd) con un parche en `device_client_process`
(`src/device.c`), que manda en dos segmentos TCP (n−1 y 1 byte) los datos cuyo mensaje mux daría
múltiplo de 512:

```c
int mux_header_size = ((conn->dev->version < 2) ? 8 : sizeof(struct mux_header));
int first = size;
if(size > 1 && (mux_header_size + (int)sizeof(struct tcphdr) + size) % USB_PACKET_SIZE == 0)
	first = size - 1;
res = send_tcp(conn, TH_ACK, conn->ob_buf, first);
/* ... y si first < size, otro send_tcp con el último byte; tx_seq avanza cada vez */
```

Partir la transferencia USB en vez del mensaje no vale: el iPhone exige un mensaje entero por
transferencia. Si el iPhone se cuelga, usbipd lo suelta (`Shared`) y hay que volver a hacer
`usbipd attach --wsl`. Un `apt upgrade` de usbmuxd quita el parche.

## Fases

0. **[PruebaMemoria](PruebaMemoria/)**: una app sin nada del juego que mide en el iPhone lo que el
   SDK necesita:
   - una zona compartida de 4,5 GB (`shm_open` o un fichero);
   - la reserva de 4,5 GB de espacio de direcciones, con las vistas en espejo de `MapViewsMac` en
     `nativo/sdk/src/system/xmemory.cpp` (en iOS entra por esa rama);
   - los fallos de página capturados del `mmio_handler`;
   - cuánta memoria deja iOS antes de cerrar la app.

   Se compila e instala con `cd ios/PruebaMemoria && xtool dev`; se pulsa «Probar» y el informe sale
   en pantalla y en Archivos (`prueba_memoria.txt`). Si la firma falla por los permisos
   (`PruebaMemoria.entitlements`), se quita `entitlementsPath` de `xtool.yml` y se prueba sin ellos:
   el informe dirá entonces si la reserva cabe o no. xtool cambia el identificador de la app
   (`XTL-<equipo>.io.github...`, se ve con `ideviceinstaller list --user`); con él, el informe se
   saca por USB: `afcclient --container <id> get Documents/prueba_memoria.txt`.

   **Resultado** (iPhone 13 Pro, 6 GB, páginas de 16 KB, Apple ID gratuita): la zona compartida y
   la reserva de 4,5 GB caben, el espejo funciona y los fallos de página se capturan. iOS deja
   unos 3063 MB. Para la fase 1:
   - `shm_open` da `EPERM`: la zona compartida tiene que ser un fichero en tmp.
   - Una reserva de 8,5 GB (la de Linux) no cabe; la de 4,5 GB, sí.
   - Páginas de 16 KB: revisar la vista de `0xE0000000`, que apunta a `0x100001000`.
1. **El motor nativo compilado para iOS** con CMake y el kit de iOS que instala xtool, como
   bibliotecas estáticas, enlazadas en una app SwiftPM mínima: elegir la ISO y jugar con mando.
   MoltenVK como Vulkan. Lo que haya que tocar del SDK y del renderizador va en `nativo/`, con
   `#if` de iOS, para que Android no cambie.
2. **Controles táctiles y lanzador** en SwiftUI: lo que hoy hacen `SetupActivity`,
   `TouchControllerView` y `EditorTactilActivity` en Android.
3. **El resto**: idiomas, servidor LAN (`ServidorEa`), preajustes.
