# El multijugador: hasta dónde llega y dónde se para

Estado corto (2026-10-15, noche): **dos móviles llegan a la misma sala de juego** con un
servidor de EA propio en el PC; **la carrera todavía no**: falta la conexión directa
entre los dos juegos. Lo de abajo de "Muro 1" en adelante es la historia de cómo se
llegó; el estado y el protocolo están en esta primera sección.

## Estado: servidor de EA propio en la LAN (motor nativo, APK PAL)

### Cómo se prueba

1. En el PC: `python servidor_ea.py --ip <IP del PC>` desde el proyecto aparte
   `..\nfsmw-server-lan` (github.com/victorgbd/nfsmw-server-lan; directorio 30960, salas 10901; todo al log
   `registros/servidor.log` de ese proyecto; ver `tools/red/README.md`).
2. En cada móvil, ajustes de la app → "Servidor LAN (multijugador)": la IP del PC. Con eso
   el motor nativo arranca con `--grant_user_privileges --red_lan --red_live
   --red_dns=<IP>` (`MotorNativo.argumentos`). Gamertag distinto en cada uno.
3. Uno: Xbox Live → Partida rápida → Crear partida. El otro: Xbox Live → Partida rápida.

Diagnóstico: lanzar con el banco y `--red_registro=true` (cada llamada de red al log,
`[red]`), `--rastro_sistema=true` (llamadas a exportaciones, `[rastro]`) y
`--nfsmw_red_sonda=true` (gancho de `NetConnStatus` y volcados de cadenas/tablas del
juego, `[red_sonda]`; solo PAL).

### Los muros que había, en orden, y qué los quita

| Síntoma | Causa | Arreglo |
|---|---|---|
| Ningún socket; IP 127.0.0.1 | la app no tenía permiso `INTERNET` | manifiesto |
| Sockets que nunca dejan de bloquear, `select` ciego, errores 0 | xsocket/xam_net venían de Windows: `FIONBIO`, `setsockopt`, `nfds`, errno→WSA, orden de bytes de `sendto`/`recvfrom`, SIGPIPE | `xam_net.cpp`, `xsocket.cpp` |
| Se queda en "Conectando a los servidores de EA" sin tocar la red | espera a que el jugador esté en Live tras `XamShowSigninUI` (signin_state siempre 1) | `red_live` (estado 2) |
| "Has perdido tu conexión a Xbox Live" tras iniciar sesión en EA | `XOnlineGetServiceInfo(0x45410004)` (mensaje xlivebase 0x58007) daba error → NetConn en `-svc` | contesta la IP de `red_dns` (`xlivebase_app.cpp`) |
| Vuelve al menú a los 60 s | el servidor no mandaba `~png` | latido cada 20 s |
| Crear partida no sigue | `ucre` sin dueño (`O`) | ficha de sala completa |
| "Esperando a que se conecten…" repitiendo `XNetQosLookup` | stub | QoS completa al momento |
| "Comenzando sesión de juego" | xlivebase `0x5000D` desconocido → error | con `red_lan`, lo desconocido va bien y se apunta |

### El protocolo (salas de EA 2005, "Aries"; `PROD=nfs-xbox-2006`)

Cabecera de 12 bytes (orden de 4 letras, subcódigo de 4, largo total big-endian) y texto
`CLAVE=valor\n`, acabado en 0. Lo que pide el cliente, en orden: `@tic` (cifrado, sin
respuesta), `@dir` → `ADDR PORT SESS MASK`; en el servidor de salas `addr`, `skey`,
`news`, `auth`, `pers`, `sele` (y el servidor manda `+who`), `user`, `usea`, `auxi`.

Partida rápida: `usea` → `COUNT=n` y luego `n` mensajes `+uss` con la ficha de sala. Crear:
`ucre` → ficha de sala `I T SF CF O S C N D P` (`sub_82825B28`), luego `gcre` → ficha de
partida (`sub_82825C90`): `IDENT NAME SELF HOST PARAMS PLATPARAMS ROOM CUSTFLAGS SYSFLAGS
COUNT PRIV MINSIZE MAXSIZE NUMPART SEED WHEN AUTH SESS` y por jugador `OPID%d`, un nombre
(clave en `0x820A1B30`, sin leer aún; se manda `OPPO%d`), `ADDR%d LADDR%d` (IP con
puntos), `MADDR%d` (la XNADDR que el cliente manda en `auth`), `OPPART%d OPPARAM%d
OPFLAG%d PRES%d`, más `PARTSIZE%d PARTPARAMS%d`. Unirse: `ujoi`, `gjoi`. Otros: `gset`,
`uadm`, `mesg`, `glea`, `gdel`, `gsta`.

Mensajes que el servidor manda sin que se los pidan: la tabla del juego en `0x828F2264`
es `+ses +mgm +msg +who +rom +pop +usr +uss +ust +usm +snp +agm +gam +sst +bud +mlg +koh`
(cada uno con su función). Las claves se leyeron de la memoria del juego en marcha con la
sonda (`.rdata` no sale en el código traducido).

### Dónde está (2026-10-16): carrera completa entre dos móviles

El 2026-10-16 un RedMagic (SM8750) y un Galaxy A22 (Mali) corrieron una carrera de circuito
entera por la LAN, con los dos resultados (`rank`) mandados al acabar, y cambiaron de
anfitrión sin reiniciar. Los dos últimos muros:

- **El enlace de juego solo traía el primer paquete.** El ConnApi de DirtySock usa CommUDP
  en el puerto 3658 y recibe en un hilo suyo (`sub_8282A078`) con `WSARecvFrom` solapada:
  espera a "cualquiera" de sus eventos con `WSAWaitForMultipleEvents` y vuelve a pedir
  con `sub_82829FA0`. Nuestro `NetDll_WSAWaitForMultipleEvents` pasaba `fWaitAll` tal
  cual como tipo de espera del kernel (allí 0 es "todos"), el plazo en ms como instante
  absoluto, y devolvía siempre 0 en vez del índice. Arreglado en `xam_net.cpp`.
- **La sala de juego salía vacía** ("Espera a que haya un número mínimo de jugadores").
  Las "salas" `ucre/ujoi/ulea/usea/uadm` son *user sets*. La lista de miembros de la sala
  de juego (`LobbyGameSessions`, objeto +360) es la del set de la ranura 0 (lista 24 del
  lobby) y solo se llena con `+usm` (`sub_8282EB78`): `S` ranura, `I`, `N`, `F`, `G` y `X`
  (registro de 160 bytes de `sub_82826530`: +4 nombre, +28 texto `auxi`, de donde se leen
  `SCF`, `LT`, `CD` y `CE`). El miembro 0 es el que se llama como uno mismo
  (`sub_82544C28`). El que se une se suscribe con `sele USERSET0=<sala>` antes de `ujoi`;
  el anfitrión no manda nada y se le da la ranura 0 al crear. `+usr` (`sub_8282ED40`) es
  otra cosa: la lista de usuarios del lobby (ahí `X` va a +172).

La tabla de mensajes del lobby está en `0x828F2228` (pares código/función) y se puede
leer sin el móvil: `tools/ediciones/imagen.py` saca la imagen del XEX
(`cargar(ruta, cache)` → `(img, huella)`).

Pendiente: si un móvil pasa a segundo plano en carrera, Android lo cierra y el otro
sigue solo; XGI `0xB0013` (`XSessionLeave`) ya contesta bien, sin probar a fondo.

**Y la versión de 360 no tiene System Link (comprobado el 2026-10-15).** El carrusel del
menú principal solo trae "Xbox Live"; recorrido opción por opción en el móvil, no hay
LAN ni enlace de sistema. Con `grant_user_privileges` se pasa el cartel y el juego se
queda en "Conectando a los servidores de EA…" sin llamar a ningún stub de XNet ni a
ningún mensaje de sesión (log en debug, motor nativo). Lo que este documento dice abajo
de que la única vía es el System Link queda corregido así: **para jugar en LAN habría que
sustituir el servidor de EA**, no basta con completar XNet.

Esta entrada documenta qué se probó y qué se midió, para que quien quiera intentarlo no
repita el camino.

## Muro 1: los privilegios — resuelto

**Síntoma:** al entrar al multijugador, el juego saca un cartel y no deja pasar.

> ATENCIÓN
> Los privilegios que tienes en Xbox Live no te permiten acceder a esta función.

No es un fallo ni un cuelgue: es un **no** limpio, y llega mucho antes de que se toque la
red.

Está en `src/kernel/xam/xam_user.cpp`, y el comentario original no deja dudas:

```cpp
u32 XamUserCheckPrivilege_entry(u32 user_index, u32 mask, mapped_u32 out_value) {
  ...
  // If we deny everything, games should hopefully not try to do stuff.
  *out_value = 0;
  return X_ERROR_SUCCESS;
}
```

Deniega **todos** los privilegios, siempre, sea cual sea el que pregunten. Viene de Xenia
y para un emulador sin Xbox Live tiene su lógica: si el juego se cree sin permisos, ni
lo intenta, y no se cuelga contra servidores apagados.

Lo llamativo es que el resto del SDK dice lo contrario:

| Función | Devuelve |
|---|---|
| `XamUserIsOnlineEnabled` | 1 — hay conexión |
| `XamUserGetMembershipTier` | 6 — que es Gold |
| `user_profile.signin_state` | 1 — sesión iniciada |
| `user_profile.type` | 1 \| 2 — perfil local y online |

O sea: perfil montado, sesión iniciada, membresía Gold, y cero permisos. La única pieza
que decía que no era esa.

`parche_privilegios.py` añade `grant_user_privileges`, apagado por defecto. Encendido, se
pasa el cartel. **Confirmado funcionando.**

## Muro 2: los servidores de EA — insalvable

Pasado el cartel, el juego se pone a buscar las partidas en los servidores oficiales de
EA y se queda en bucle.

Esto no tiene arreglo y no merece esfuerzo: esos servidores llevan años apagados. No hay
parche que los devuelva. **La única vía posible es System Link**, que no usa
infraestructura de EA para nada — dos máquinas hablando directamente.

Un descarte útil: **no se atasca resolviendo el nombre.** `XNetDnsLookup` en este SDK ya
falla rápido a propósito:

```cpp
dns->status = 1;  // non-zero = error
if (event_handle) ev->Set(0, false);
```

Devuelve error y despierta el evento en el acto. El bucle está más arriba, probablemente
en que el juego reintenta la búsqueda de sesiones indefinidamente (ver abajo).

## Muro 3: la capa XNet — el trabajo de verdad

Aquí está el fondo del asunto. Cruzando la tabla de ordinales del SDK
(`src/kernel/xam/export_table.inc`) con lo que `xam_net.cpp` implementa de verdad:

- **158** funciones de red declaradas
- **44** implementadas
- **114** sin implementar

Lo que hay funciona y no es poco: sockets de verdad (`socket`, `bind`, `connect`,
`send`/`recv`, `sendto`/`recvfrom`, `select`) y `XNetSetSystemLinkPort`.
`XNetGetTitleXnAddr` está, pero devuelve 127.0.0.1, no la IP local (ver abajo).

Lo que falta es justo el System Link:

| Ordinal | Función | Para qué |
|---|---|---|
| 0x36 | `XNetCreateKey` | crear la XNKID/XNKEY de la partida |
| 0x37 | `XNetRegisterKey` | asociarla en el otro extremo |
| 0x38 | `XNetUnregisterKey` | |
| 0x3F | `XNetUnregisterInAddr` | |
| 0x41 | `XNetConnect` | levantar el enlace con el par |
| 0x42 | `XNetGetConnectStatus` | saber si se levantó |
| 0x53 | `XNetGetSystemLinkPort` | solo existe el *setter* |
| 0x09 | `getsockname` | básico, y tampoco está |

Ese trío `CreateKey`/`RegisterKey`/`UnregisterKey` es el que asocia la clave de sesión.
Sin él el juego no llega ni a intentar hablar.

## Muro 4: las sesiones son de adorno

Y hay una segunda capa igual de vacía. Los manejadores de sesión de
`src/kernel/xam/apps/xgi_app.cpp` leen los parámetros, los escriben en el log y
devuelven `X_E_SUCCESS` **sin hacer nada**. Por ejemplo `XSessionSearch` (mensaje
`0x000B0016`):

```cpp
REXKRNL_DEBUG("XSessionSearch({}, {}, {}, ...)", ...);
return X_E_SUCCESS;
```

Ni siquiera toca el buffer de resultados. Un cliente buscando partidas siempre encontrará
cero — y como le contestan "correcto", no tiene motivo para rendirse. **Un stub que
miente es peor que uno que falla.** Es la explicación más probable del bucle del muro 2.

Igual con `XSessionCreate`, `XSessionJoinRemote`, `XSessionModify` y el resto.

## Lo que llama Most Wanted de verdad (2026-10-15)

De las 158 no hacen falta todas. El código traducido trae la tabla de importaciones
entera, pero cada importación pasa por una función puente (`sub_827C67xx` para XNet,
`sub_82810xxx` para los sockets), y contar quién llama a cada puente dice qué usa el
juego. Sale del código generado, sin arrancarlo; la cuenta es de sitios de llamada, no
de veces en marcha.

**XNet: 15 funciones con llamadas.** Del trío de claves, el juego llama él mismo a
`XNetCreateKey` (1 sitio). Eso **no** prueba que haya System Link: los juegos de EA
usaban su propio servidor de salas en vez de las sesiones de Xbox Live, y el anfitrión
creaba la clave a mano para repartirla por ese servidor.

| Función | Sitios | SDK Xenos | Qué hace hoy |
|---|---|---|---|
| `XNetStartup` / `XNetCleanup` | 2 / 1 | sí | |
| `XNetGetTitleXnAddr` | 7 | sí | **127.0.0.1** y MAC `CC:CC:…` |
| `XNetGetEthernetLinkStatus` | 3 | sí | **0: "no hay cable"** |
| `XNetXnAddrToInAddr` | 2 | sí | **devuelve 1 (error) sin escribir nada** |
| `XNetCreateKey` | 1 | stub | |
| `XNetRegisterKey` | 2 | stub | |
| `XNetUnregisterKey` | 1 | stub | |
| `XNetConnect` | 1 | stub | |
| `XNetGetConnectStatus` | 1 | stub | |
| `XNetQosLookup` | 2 | stub | sondea a los anfitriones |
| `XNetQosRelease` / `XNetQosServiceLookup` | 3 / 2 | sí | |
| `XNetDnsLookup` / `XNetDnsRelease` | 1 / 2 | sí | falla rápido (lo de EA) |
| `XnpLogonGetStatus` | 1 | stub | |

`XNetQosListen`, `XNetInAddrToXnAddr` y `XNetGetBroadcastVersionStatus` se importan
pero nadie las llama.

**Sockets:** TCP (`listen`/`accept`/`connect`) y UDP (`sendto`/`recvfrom`), más
E/S solapada (`WSARecv`, `WSARecvFrom`, `WSAGetOverlappedResult`, eventos). Faltan
`WSARecv`, `WSAGetOverlappedResult`, `getsockname`, `getpeername` y `getsockopt`.

Las tres "implementadas" que mienten (dirección de bucle, sin cable y una conversión
que falla) bastan por sí solas para que el juego no vea a nadie en la red. Una 360
devuelve en `XNADDR.ina` su IP de la LAN; las funciones de abajo traducen esa XNADDR a
una dirección con la que abrir sockets.

**Sesiones:** mensajes XGI (app 0xFB) que manda el juego, sacados de las llamadas a
`XMsgStartIORequest`:

```
0xB0006 0xB0007 0xB0008                   contexto, propiedades, logros
0xB0010 0xB0011 0xB0012 0xB0013           crear, borrar, unirse, salir
0xB0014 0xB0015 0xB0016 0xB0018           empezar, acabar, buscar, modificar
0xB001A 0xB001B 0xB001C                   arbitraje, buscar por ID, buscar (Ex)
0xB0019 0xB0020 0xB0021 0xB0025 0xB0026   invitación y estadísticas
```

Todos menos `0xB0013` tienen manejador, y todos son de adorno. `0xB0013` (salir de la
sesión) ni eso: devuelve `X_E_FAIL` con "Unimplemented XGI message".
`XamSessionCreateHandle` da siempre el mismo asa (`0xCAFEDEAD`), y el objeto de sesión
es `0xDEADF00D`.

El motor nativo (`nativo/sdk`) es el mismo SDK v0.10.0 y está igual en todo esto.
Hasta el 2026-10-15 le faltaba además `grant_user_privileges`, porque
`parche_privilegios.py` solo parchea el SDK de Xenos: ya lo tiene, a mano.

## Qué haría falta

Por orden, y sin engañarse con el tamaño:

1. **Los stubs de XNet que faltan**, mapeando XNADDR ↔ IP directamente en vez de
   emular la asociación segura real. Es lo que hacen los forks de red de Xenia.
2. **Descubrimiento de partidas de verdad**: que `XSessionCreate` registre una sesión
   local y que `XSessionSearch` la anuncie y la encuentre por broadcast UDP.
3. Solo entonces, la interfaz.

## Un aviso de diseño sobre "pásame tu IP"

La idea natural —un panel con tu IP y tu puerto para que un amigo los pegue como
"Host"— **no es como funciona el System Link.** La 360 descubre partidas por *broadcast*
en la red local, y un broadcast no cruza internet.

Para jugar con alguien de fuera harían falta, o bien una VPN que os ponga en la misma LAN
virtual (ZeroTier, Radmin, Hamachi), o bien una capa de conexión directa que sustituya el
broadcast por una IP concreta. Eso último es lo que hacen XLink Kai y las builds de red de
Xenia, y es diseño nuevo, no un ajuste.

## El siguiente paso, si alguien lo retoma

`LOG_DETALLADO.bat` arranca con `--log_level=debug --log_noisy=true`, y los manejadores de
sesión ya escriben cada llamada. Así que entrar al multijugador con eso puesto y dejarlo
dar vueltas 30 segundos da la secuencia exacta —qué pide el juego, en qué orden y dónde se
repite— sin escribir una línea de código.

Ese log es lo primero que hay que mirar. Puede que Most Wanted use bastante menos de lo
que falta en total.
