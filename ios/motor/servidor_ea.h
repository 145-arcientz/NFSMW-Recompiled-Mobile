// El servidor de salas de EA de la LAN dentro de la app de iOS, para que un telefono haga de
// anfitrion sin PC: el ServidorEa.java de la app de Android (y el ea_server.py de nfsmw-server-lan)
// en C++. Corre en sus hilos, en el proceso del juego, mientras la app esta delante: iOS no deja
// que una app siga sirviendo en segundo plano.

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
// Los que estan en Xbox Live del juego (con sesion en el servidor de salas).
int Jugadores();

}  // namespace nfsmw::servidor
