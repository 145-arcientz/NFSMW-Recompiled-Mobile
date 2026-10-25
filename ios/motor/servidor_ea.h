// El servidor de salas de EA de la LAN dentro de la app de iOS, para que un telefono haga de
// anfitrion sin PC: el ServidorEa.java de la app de Android (y el ea_server.py de nfsmw-server-lan)
// en C++. Corre en sus hilos, en el proceso del juego. En segundo plano iOS congelaria la app y le
// anularia los sockets; segundo_plano.mm la mantiene viva mientras el servidor esta encendido, y si
// aun asi se cae (una llamada, por ejemplo), Revivir() lo vuelve a abrir al volver a la app.

#pragma once

#include <functional>
#include <string>

namespace nfsmw::servidor {

// datos: donde van stats.json y races.jsonl; logs: la carpeta de servidor_lan.log. alCambiar: se
// llama (desde cualquier hilo) cuando entra o sale un jugador. nombre: el que ve quien busca
// servidores, pedido en cada busqueda. false, con el motivo en `error`, si no pudo abrir los puertos.
bool Arrancar(const std::string& datos, const std::string& logs, std::function<void()> alCambiar,
              std::function<std::string()> nombre, std::string* error);
void Parar();
bool EnMarcha();
// Si se arranco y se cayo solo (sin Parar()), lo vuelve a abrir con lo mismo que se paso a Arrancar.
// true si queda en marcha. Puede tardar unos segundos (espera a que se suelten los puertos): llamarlo
// fuera del hilo principal.
bool Revivir(std::string* error);
// Los que estan en Xbox Live del juego (con sesion en el servidor de salas).
int Jugadores();

}  // namespace nfsmw::servidor
