// Las partidas guardadas en un .zip, para verlas y llevarlas a otro sitio: el Partidas.java de la app
// de Android en C++, con el mismo formato. La primera entrada es nfsmw_partidas.txt y el resto es la
// carpeta de datos del motor (user_data_root: Documents/datos en iOS, files/nativo/usuario en
// Android): el perfil y las partidas (<xuid>/<title id>/...), los logros (achievements/) y las copias
// de saves/. Un .zip exportado en Android se importa en iOS y al reves.
//
// Se deja fuera lo que no son partidas: nfsmw.toml, las bibliotecas de shaders (*.nfsp), las caches,
// los ocultos y, en iOS, servidor_lan/ (las estadisticas del servidor de la LAN, que en Android van en
// otra carpeta).

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace nfsmw::partidas {

// La primera entrada del .zip: dice que es una copia de partidas de esta app.
inline constexpr char kMarca[] = "nfsmw_partidas.txt";

struct Resultado {
  int archivos = 0;
  uint64_t bytes = 0;  // sin comprimir
};

enum class Importado { kBien, kNoValido, kError };

// Si en `carpeta` hay algun archivo que exportar.
bool Hay(const std::filesystem::path& carpeta);

// nfsmw_partidas_2026-10-24_1530.zip
std::string NombreSugerido();

// Mete las partidas de `carpeta` en `destino`. `version` va en la marca. Si falla, no deja el .zip a
// medias y `error` dice por que.
bool Exportar(const std::filesystem::path& carpeta, const std::filesystem::path& destino, const std::string& version,
              Resultado& r, std::string& error);

// Sustituye las partidas de `carpeta` por las del .zip `origen`. Primero lo descomprime aparte (si esta
// roto o no es de esta app, las de ahora no se tocan), luego deja las de ahora en `copia` y al final
// cambia unas por otras.
Importado Importar(const std::filesystem::path& carpeta, const std::filesystem::path& origen,
                   const std::filesystem::path& copia, const std::string& version, Resultado& r,
                   std::string& error);

}  // namespace nfsmw::partidas
