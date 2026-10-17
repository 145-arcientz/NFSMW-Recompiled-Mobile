# Servidor de EA para la LAN: se ha movido

El servidor de salas de EA (`servidor_ea.py`) y la herramienta de escucha
(`escucha_ea.py`) son ahora un proyecto aparte: **`nfsmw-servidor-lan`**, en la carpeta
hermana de este repositorio (`..\nfsmw-servidor-lan`).

```
cd ..\nfsmw-servidor-lan
python servidor_ea.py --ip <IP del PC>
```

Allí están el uso, los puertos, los ajustes del móvil y el protocolo (su `README.md`).
El registro va a `registros/servidor.log` dentro de ese proyecto, no a `out/red/`.

Lo que hay que tocar en el juego para jugar en LAN (sockets, XNet, ganchos del motor
nativo) sigue en este repositorio: `docs/diario/red-y-privilegios.md`.
