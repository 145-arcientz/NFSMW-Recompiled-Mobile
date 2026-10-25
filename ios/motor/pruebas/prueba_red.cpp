// La red del lanzador de iOS (red_lan.cpp) en Linux, contra un servidor de la LAN en este mismo
// equipo (prueba_servidor o el ea_server.py de nfsmw-server-lan): las direcciones, la comprobacion
// del gamertag y la busqueda de servidores. Sale con 0 si todo da lo esperado.
//
//   prueba_red

#include <csignal>
#include <iostream>
#include <string>

#include "../red_lan.h"

namespace {

int fallos = 0;

void Comprobar(bool bien, const std::string& que) {
  std::cout << (bien ? "bien  " : "MAL   ") << que << "\n";
  if (!bien) ++fallos;
}

const char* Texto(nfsmw::red::Comprobacion c) {
  switch (c) {
    case nfsmw::red::Comprobacion::kLibre: return "FREE";
    case nfsmw::red::Comprobacion::kEnUso: return "IN_USE";
    default: return "sin respuesta";
  }
}

}  // namespace

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  using namespace nfsmw::red;
  Comprobar(IpValida("10.0.0.4") && IpValida("255.255.255.255") && !IpValida("10.0.0") && !IpValida("10.0.0.256") &&
                !IpValida("1.2.3.4.5") && !IpValida("a.b.c.d") && !IpValida("10..0.1"),
            "IpValida");
  Comprobar(DireccionValida("10.0.0.4") && DireccionValida("mi-pc") && DireccionValida("servidor.lan") &&
                !DireccionValida("10.0.0") && !DireccionValida("-pc") && !DireccionValida("mi_pc") && !DireccionValida(""),
            "DireccionValida");
  Comprobar(ResolverIpv4("10.0.0.4") == "10.0.0.4", "ResolverIpv4 de una IP");
  Comprobar(ResolverIpv4("localhost") == "127.0.0.1", "ResolverIpv4 de un nombre: " + ResolverIpv4("localhost"));
  Comprobar(ResolverIpv4("no-existe.invalid").empty(), "ResolverIpv4 de un nombre que no existe");
  Comprobar(GamertagEfectivo("  ") == "User" && GamertagEfectivo(" Victor ") == "Victor", "GamertagEfectivo");

  const auto libre = ComprobarGamertag("127.0.0.1", "Alguien");
  Comprobar(libre == Comprobacion::kLibre, std::string("ComprobarGamertag de uno libre: ") + Texto(libre));
  // El mismo, desde la misma IP: sigue siendo suyo.
  const auto otraVez = ComprobarGamertag("127.0.0.1", "Alguien");
  Comprobar(otraVez == Comprobacion::kLibre, std::string("ComprobarGamertag otra vez: ") + Texto(otraVez));
  const auto sinServidor = ComprobarGamertag("192.0.2.1", "Alguien");
  Comprobar(sinServidor == Comprobacion::kSinRespuesta, std::string("ComprobarGamertag sin servidor: ") + Texto(sinServidor));

  const auto encontrados = BuscarServidores();
  std::cout << "      busqueda: " << (encontrados ? std::to_string(encontrados->size()) + " servidores" : "sin red local") << "\n";
  if (encontrados) {
    for (const auto& e : *encontrados) std::cout << "      " << e.ip << " '" << e.nombre << "' " << e.jugadores << "\n";
  }
  std::cout << "      IpLocal: '" << IpLocal() << "'\n";
  Comprobar(PuntosInterfaz("en0") == 3 && PuntosInterfaz("bridge100") == 2 && PuntosInterfaz("en2") == 1 &&
                PuntosInterfaz("pdp_ip0") < 0 && PuntosInterfaz("utun3") < 0 && PuntosInterfaz("awdl0") < 0,
            "PuntosInterfaz");
  std::cout << (fallos ? std::to_string(fallos) + " fallos\n" : "todo bien\n");
  return fallos ? 1 : 0;
}
