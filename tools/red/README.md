# Servidor de EA para la LAN: se ha movido

El servidor de salas de EA (`ea_server.py`) y la herramienta de escucha
(`ea_listener.py`) son ahora un proyecto aparte: **nfsmw-server-lan**,
https://github.com/victorgbd/nfsmw-server-lan (aquí, en la carpeta hermana de este
repositorio: `..\nfsmw-server-lan`).

```
cd ..\nfsmw-server-lan
python ea_server.py --ip <IP del PC>
```

Allí están el uso, los puertos, los ajustes del móvil y el protocolo (su `README.md`).
El registro va a `logs/server.log` dentro de ese proyecto, no a `out/red/`.

La app comprueba con ese servidor (TCP 10902) que el gamertag no lo esté usando otro
aparato conectado antes de dejar jugar (`ComprobacionGamertag.java`).

Lo que hay que tocar en el juego para jugar en LAN (sockets, XNet, ganchos del motor
nativo) sigue en este repositorio: `docs/diario/red-y-privilegios.md`.

## Sin PC: un móvil de anfitrión (2026-10-17)

La app lleva el mismo servidor en Java (`ServidorEa.java`, port 1:1 de `ea_server.py`
del commit 9ebeb11) para que un móvil haga de servidor y juegue a la vez: ajuste
**"Anfitrión en este móvil"** en la tarjeta "Servidor LAN". Corre en un servicio en
primer plano (`ServidorLanService.java`) del proceso principal, así que sigue vivo
mientras el juego va en `:juego`; la notificación dice la IP y cuántos hay en Xbox
Live, y tiene un botón para pararlo. El juego del anfitrión va contra la IP de su
propia Wi-Fi o punto de acceso (`Ajustes.servidorEfectivo`), nunca 127.0.0.1: esa IP
es la que el servidor pone en el registro de la partida para los demás. Los demás
ponen esa IP como siempre. Estadísticas en `files/servidor_lan/` (mismo
`stats.json`), registro en `files/logs/servidor_lan.log`.

**Un cambio de protocolo va a los dos** (`ea_server.py` y `ServidorEa.java`). Se
comprobó con dos clientes falsos que las dos versiones contestan exactamente lo mismo.
Única diferencia: `@dir ADDR` y `news BUDDY_SERVER` son la IP local de cada conexión,
no una `--ip` fija.
