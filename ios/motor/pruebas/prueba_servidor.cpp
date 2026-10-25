// El servidor de salas de iOS (servidor_ea.cpp) en un programa de Linux, para probarlo sin telefono
// (prueba_servidor_ea.py): arranca, dice "listo" y sirve hasta que se cierra su entrada.
//
//   prueba_servidor <carpeta de datos> <carpeta del registro>

#include <csignal>
#include <iostream>
#include <string>

#include "../servidor_ea.h"

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "prueba_servidor <datos> <registro>\n";
    return 2;
  }
  // Como la app (lanzador.mm): un cliente que se va no mata el proceso.
  std::signal(SIGPIPE, SIG_IGN);
  std::string error;
  if (!nfsmw::servidor::Arrancar(argv[1], argv[2], [] {}, [] { return std::string("prueba"); }, &error)) {
    std::cerr << "no arranca: " << error << "\n";
    return 1;
  }
  std::cout << "listo" << std::endl;
  for (std::string linea; std::getline(std::cin, linea);) {
  }
  nfsmw::servidor::Parar();
  return 0;
}
