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
