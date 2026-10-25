// Las partidas en un .zip: ver partidas.h.
//
// El .zip se escribe y se lee aqui con zlib (iOS no tiene una API publica para .zip). Sin ZIP64: las
// partidas son unos pocos megas. Se escribe con deflate (o guardado tal cual si no gana nada) y
// nombres en UTF-8. Se lee por el directorio central, no por las cabeceras locales: el ZipOutputStream
// de Java (la app de Android) escribe los tamanos despues de los datos (descriptor de datos) y deja a
// cero los de la cabecera local.

#include "partidas.h"

#include <sys/stat.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace nfsmw::partidas {
namespace {

constexpr int kProfundidadMax = 16;  // como PROFUNDIDAD_MAX de Partidas.java

bool EsPartida(const std::string& nombre) {
  std::string n = nombre;
  std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  auto acaba = [&](const char* fin) {
    const size_t l = std::strlen(fin);
    return n.size() >= l && n.compare(n.size() - l, l, fin) == 0;
  };
  return n != "nfsmw.toml" && !acaba(".nfsp") && n.rfind("cache", 0) != 0 && n != kMarca && n.rfind(".", 0) != 0 &&
         n != "servidor_lan";
}

void Poner16(std::string& b, uint32_t v) {
  b.push_back(char(v & 0xFF));
  b.push_back(char((v >> 8) & 0xFF));
}
void Poner32(std::string& b, uint32_t v) {
  Poner16(b, v & 0xFFFF);
  Poner16(b, v >> 16);
}
uint32_t Leer16(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8); }
uint32_t Leer32(const uint8_t* p) { return Leer16(p) | (Leer16(p + 2) << 16); }

void HoraDos(std::time_t t, uint16_t& hora, uint16_t& fecha) {
  std::tm l{};
  localtime_r(&t, &l);
  if (l.tm_year < 80) {  // antes de 1980 no cabe
    l = std::tm{};
    l.tm_year = 80;
    l.tm_mday = 1;
  }
  hora = uint16_t((l.tm_hour << 11) | (l.tm_min << 5) | (l.tm_sec / 2));
  fecha = uint16_t(((l.tm_year - 80) << 9) | ((l.tm_mon + 1) << 5) | l.tm_mday);
}

std::time_t TiempoDos(uint32_t hora, uint32_t fecha) {
  std::tm l{};
  l.tm_year = int(fecha >> 9) + 80;
  l.tm_mon = int((fecha >> 5) & 0xF) - 1;
  l.tm_mday = int(fecha & 0x1F);
  l.tm_hour = int(hora >> 11);
  l.tm_min = int((hora >> 5) & 0x3F);
  l.tm_sec = int(hora & 0x1F) * 2;
  l.tm_isdst = -1;
  return std::mktime(&l);
}

std::string FechaTexto(const char* formato) {
  const std::time_t t = std::time(nullptr);
  std::tm l{};
  localtime_r(&t, &l);
  char b[64];
  std::strftime(b, sizeof(b), formato, &l);
  return b;
}

bool LeerFichero(const fs::path& ruta, std::string& datos) {
  std::ifstream f(ruta, std::ios::binary);
  if (!f) return false;
  datos.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return !f.bad();
}

struct Entrada {
  std::string nombre;
  uint32_t crc = 0, comprimido = 0, original = 0, desplazamiento = 0;
  uint16_t hora = 0, fecha = 0, metodo = 0;
};

class Escritor {
 public:
  explicit Escritor(const fs::path& ruta) : f_(ruta, std::ios::binary | std::ios::trunc) {}
  bool Bien() const { return bool(f_); }

  bool Anadir(const std::string& nombre, const std::string& datos, std::time_t tiempo) {
    if (datos.size() > 0xFFFFFFF0u || entradas_.size() >= 0xFFFF || nombre.size() > 0xFFFF) return false;
    Entrada e;
    e.nombre = nombre;
    e.original = uint32_t(datos.size());
    e.crc = uint32_t(crc32(0, reinterpret_cast<const Bytef*>(datos.data()), uInt(datos.size())));
    HoraDos(tiempo, e.hora, e.fecha);
    std::string comprimido;
    z_stream z{};
    if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
    comprimido.resize(deflateBound(&z, uLong(datos.size())));
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(datos.data()));
    z.avail_in = uInt(datos.size());
    z.next_out = reinterpret_cast<Bytef*>(comprimido.data());
    z.avail_out = uInt(comprimido.size());
    const int fin = deflate(&z, Z_FINISH);
    comprimido.resize(z.total_out);
    deflateEnd(&z);
    if (fin != Z_STREAM_END) return false;
    e.metodo = 8;
    if (comprimido.size() >= datos.size()) {  // no gana nada: tal cual
      comprimido = datos;
      e.metodo = 0;
    }
    e.comprimido = uint32_t(comprimido.size());
    if (uint64_t(posicion_) + 30 + nombre.size() + comprimido.size() > 0xFFFFFFF0u) return false;
    e.desplazamiento = posicion_;
    std::string cabecera;
    Poner32(cabecera, 0x04034b50);
    Poner16(cabecera, 20);      // version para extraer
    Poner16(cabecera, 0x0800);  // nombres en UTF-8
    Poner16(cabecera, e.metodo);
    Poner16(cabecera, e.hora);
    Poner16(cabecera, e.fecha);
    Poner32(cabecera, e.crc);
    Poner32(cabecera, e.comprimido);
    Poner32(cabecera, e.original);
    Poner16(cabecera, uint32_t(nombre.size()));
    Poner16(cabecera, 0);
    cabecera += nombre;
    Escribir(cabecera);
    Escribir(comprimido);
    entradas_.push_back(std::move(e));
    return bool(f_);
  }

  bool Cerrar() {
    const uint32_t inicio = posicion_;
    for (const Entrada& e : entradas_) {
      std::string c;
      Poner32(c, 0x02014b50);
      Poner16(c, (3 << 8) | 20);  // hecho en Unix, version 2.0
      Poner16(c, 20);
      Poner16(c, 0x0800);
      Poner16(c, e.metodo);
      Poner16(c, e.hora);
      Poner16(c, e.fecha);
      Poner32(c, e.crc);
      Poner32(c, e.comprimido);
      Poner32(c, e.original);
      Poner16(c, uint32_t(e.nombre.size()));
      Poner16(c, 0);  // extra
      Poner16(c, 0);  // comentario
      Poner16(c, 0);  // disco
      Poner16(c, 0);  // atributos internos
      Poner32(c, 0100644u << 16);  // atributos externos: un fichero rw-r--r--
      Poner32(c, e.desplazamiento);
      c += e.nombre;
      Escribir(c);
    }
    std::string fin;
    Poner32(fin, 0x06054b50);
    Poner16(fin, 0);
    Poner16(fin, 0);
    Poner16(fin, uint32_t(entradas_.size()));
    Poner16(fin, uint32_t(entradas_.size()));
    Poner32(fin, posicion_ - inicio);
    Poner32(fin, inicio);
    Poner16(fin, 0);
    Escribir(fin);
    f_.close();
    return !f_.fail();
  }

 private:
  void Escribir(const std::string& s) {
    f_.write(s.data(), std::streamsize(s.size()));
    posicion_ += uint32_t(s.size());
  }

  std::ofstream f_;
  uint32_t posicion_ = 0;
  std::vector<Entrada> entradas_;
};

// Los archivos de `f` (un fichero o una carpeta, recorrida en orden) bajo el nombre `ruta`.
bool AnadirArbol(Escritor& zip, const fs::path& f, const std::string& ruta, int profundidad, Resultado& r,
                 std::string& error) {
  std::error_code ec;
  const fs::file_status estado = fs::symlink_status(f, ec);
  if (ec || fs::is_symlink(estado)) return true;
  if (fs::is_directory(estado)) {
    if (profundidad > kProfundidadMax) return true;
    std::vector<fs::path> hijos;
    for (const auto& h : fs::directory_iterator(f, ec)) hijos.push_back(h.path());
    std::sort(hijos.begin(), hijos.end());
    for (const fs::path& h : hijos) {
      if (!AnadirArbol(zip, h, ruta + "/" + h.filename().string(), profundidad + 1, r, error)) return false;
    }
    return true;
  }
  if (!fs::is_regular_file(estado)) return true;
  std::string datos;
  if (!LeerFichero(f, datos)) {
    error = "no se puede leer " + ruta;
    return false;
  }
  struct stat st{};
  const std::time_t tiempo = ::stat(f.c_str(), &st) == 0 ? st.st_mtime : std::time(nullptr);
  if (!zip.Anadir(ruta, datos, tiempo)) {
    error = "no se puede escribir " + ruta;
    return false;
  }
  ++r.archivos;
  r.bytes += datos.size();
  return true;
}

bool TieneArchivos(const fs::path& f, int profundidad) {
  std::error_code ec;
  if (fs::is_regular_file(f, ec)) return true;
  if (!fs::is_directory(f, ec) || profundidad > kProfundidadMax) return false;
  for (const auto& h : fs::directory_iterator(f, ec)) {
    if (TieneArchivos(h.path(), profundidad + 1)) return true;
  }
  return false;
}

// Un nombre de dentro del .zip, ya con '/', que no sale de la carpeta (ni absoluto ni con "..").
bool NombreSeguro(const std::string& nombre) {
  if (nombre.empty() || nombre[0] == '/') return false;
  size_t inicio = 0;
  while (inicio <= nombre.size()) {
    const size_t barra = nombre.find('/', inicio);
    const std::string trozo = nombre.substr(inicio, barra == std::string::npos ? std::string::npos : barra - inicio);
    if (trozo == "..") return false;
    if (barra == std::string::npos) break;
    inicio = barra + 1;
  }
  return true;
}

}  // namespace

bool Hay(const fs::path& carpeta) {
  std::error_code ec;
  for (const auto& h : fs::directory_iterator(carpeta, ec)) {
    if (EsPartida(h.path().filename().string()) && TieneArchivos(h.path(), 0)) return true;
  }
  return false;
}

std::string NombreSugerido() { return "nfsmw_partidas_" + FechaTexto("%Y-%m-%d_%H%M") + ".zip"; }

bool Exportar(const fs::path& carpeta, const fs::path& destino, const std::string& version, Resultado& r,
              std::string& error) {
  r = Resultado{};
  std::error_code ec;
  fs::create_directories(destino.parent_path(), ec);
  bool bien = false;
  {
    Escritor zip(destino);
    if (!zip.Bien()) {
      error = "no se puede crear " + destino.filename().string();
      return false;
    }
    const std::string marca = "NFS Most Wanted Recompiled: partidas exportadas el " + FechaTexto("%Y-%m-%d %H:%M") +
                              "\nmotor=nativo\nversion=" + version + "\nsistema=ios\n";
    bien = zip.Anadir(kMarca, marca, std::time(nullptr));
    if (!bien) error = "no se puede escribir el .zip";
    std::vector<fs::path> hijos;
    for (const auto& h : fs::directory_iterator(carpeta, ec)) hijos.push_back(h.path());
    std::sort(hijos.begin(), hijos.end());
    for (const fs::path& h : hijos) {
      if (!bien) break;
      const std::string nombre = h.filename().string();
      if (EsPartida(nombre)) bien = AnadirArbol(zip, h, nombre, 0, r, error);
    }
    if (bien && !zip.Cerrar()) {
      bien = false;
      error = "no se puede escribir el .zip";
    }
  }
  if (!bien) fs::remove(destino, ec);
  return bien;
}

Importado Importar(const fs::path& carpeta, const fs::path& origen, const fs::path& copia, const std::string& version,
                   Resultado& r, std::string& error) {
  r = Resultado{};
  std::string zip;
  if (!LeerFichero(origen, zip)) {
    error = "no se puede leer el .zip";
    return Importado::kError;
  }
  const auto* b = reinterpret_cast<const uint8_t*>(zip.data());
  const size_t n = zip.size();
  // El final del directorio central: en los ultimos 22 + 65535 bytes (el comentario).
  size_t fin = std::string::npos;
  for (size_t i = n >= 22 ? n - 22 : 0; n >= 22; --i) {
    if (Leer32(b + i) == 0x06054b50) {
      fin = i;
      break;
    }
    if (i == 0 || n - i > 22 + 0xFFFF) break;
  }
  if (fin == std::string::npos) return Importado::kNoValido;
  const uint32_t total = Leer16(b + fin + 10);
  const uint32_t inicio = Leer32(b + fin + 16);
  if (total == 0 || total == 0xFFFF || inicio == 0xFFFFFFFFu || inicio >= n) return Importado::kNoValido;

  // Primero se descomprime aparte: si algo va mal, las partidas de ahora siguen ahi.
  std::error_code ec;
  const fs::path temporal = carpeta.parent_path() / (carpeta.filename().string() + ".importando");
  fs::remove_all(temporal, ec);
  if (!fs::create_directories(temporal, ec)) {
    error = "no se puede crear " + temporal.filename().string();
    return Importado::kError;
  }
  Importado resultado = Importado::kBien;
  size_t p = inicio;
  for (uint32_t i = 0; i < total && resultado == Importado::kBien; ++i) {
    if (p + 46 > n || Leer32(b + p) != 0x02014b50) {
      resultado = Importado::kNoValido;
      break;
    }
    const uint32_t banderas = Leer16(b + p + 8), metodo = Leer16(b + p + 10);
    const uint32_t hora = Leer16(b + p + 12), fecha = Leer16(b + p + 14), crc = Leer32(b + p + 16);
    const uint32_t comprimido = Leer32(b + p + 20), original = Leer32(b + p + 24);
    const uint32_t largo_nombre = Leer16(b + p + 28), largo_extra = Leer16(b + p + 30);
    const uint32_t largo_comentario = Leer16(b + p + 32), local = Leer32(b + p + 42);
    if (p + 46 + largo_nombre > n) {
      resultado = Importado::kNoValido;
      break;
    }
    std::string nombre(reinterpret_cast<const char*>(b + p + 46), largo_nombre);
    p += 46 + largo_nombre + largo_extra + largo_comentario;
    std::replace(nombre.begin(), nombre.end(), '\\', '/');
    if (i == 0) {  // la marca va siempre la primera
      if (nombre != kMarca) resultado = Importado::kNoValido;
      continue;
    }
    if (nombre.empty() || nombre.back() == '/') continue;  // carpeta
    const std::string primero = nombre.substr(0, nombre.find('/'));
    if (!EsPartida(primero)) continue;
    if (!NombreSeguro(nombre) || (banderas & 0x1) || (metodo != 0 && metodo != 8)) {
      resultado = Importado::kNoValido;  // fuera de la carpeta, cifrado o comprimido de otra forma
      break;
    }
    if (local + 30 > n || Leer32(b + local) != 0x04034b50) {
      resultado = Importado::kNoValido;
      break;
    }
    const size_t datos = local + 30 + Leer16(b + local + 26) + Leer16(b + local + 28);
    if (datos + comprimido > n) {
      resultado = Importado::kNoValido;
      break;
    }
    std::string salida(original, '\0');
    if (metodo == 0) {
      if (comprimido != original) {
        resultado = Importado::kNoValido;
        break;
      }
      std::memcpy(salida.data(), b + datos, original);
    } else {
      z_stream z{};
      if (inflateInit2(&z, -15) != Z_OK) {
        resultado = Importado::kError;
        error = "zlib";
        break;
      }
      z.next_in = const_cast<Bytef*>(b + datos);
      z.avail_in = comprimido;
      z.next_out = reinterpret_cast<Bytef*>(salida.data());
      z.avail_out = original;
      const int r_inflar = inflate(&z, Z_FINISH);
      const uLong salieron = z.total_out;
      inflateEnd(&z);
      if ((r_inflar != Z_STREAM_END && !(r_inflar == Z_BUF_ERROR && original == 0)) || salieron != original) {
        resultado = Importado::kNoValido;
        break;
      }
    }
    if (uint32_t(crc32(0, reinterpret_cast<const Bytef*>(salida.data()), uInt(salida.size()))) != crc) {
      resultado = Importado::kNoValido;
      break;
    }
    const fs::path f = temporal / fs::path(nombre);
    fs::create_directories(f.parent_path(), ec);
    std::ofstream o(f, std::ios::binary | std::ios::trunc);
    o.write(salida.data(), std::streamsize(salida.size()));
    o.close();
    if (o.fail()) {
      resultado = Importado::kError;
      error = "no se puede escribir " + nombre;
      break;
    }
    const std::time_t t = TiempoDos(hora, fecha);
    if (t > 0) {
      const fs::file_time_type ahora_fichero = fs::file_time_type::clock::now();
      const auto diferencia = std::chrono::system_clock::from_time_t(t) - std::chrono::system_clock::now();
      fs::last_write_time(f, ahora_fichero + std::chrono::duration_cast<fs::file_time_type::duration>(diferencia),
                          ec);
    }
    ++r.archivos;
    r.bytes += original;
  }

  if (resultado == Importado::kBien) {
    // Las de ahora, a la copia; si no se puede, no se toca nada.
    Resultado copiadas;
    fs::create_directories(carpeta, ec);
    if (Hay(carpeta) && !Exportar(carpeta, copia, version, copiadas, error)) {
      error = "no se puede guardar la copia de las de ahora (" + error + ")";
      resultado = Importado::kError;
    }
  }
  if (resultado == Importado::kBien) {
    // Las listas antes de tocar nada: borrar o mover mientras se recorre la carpeta no es seguro.
    std::vector<fs::path> viejas, nuevas;
    for (const auto& h : fs::directory_iterator(carpeta, ec)) {
      if (EsPartida(h.path().filename().string())) viejas.push_back(h.path());
    }
    for (const auto& h : fs::directory_iterator(temporal, ec)) nuevas.push_back(h.path());
    for (const fs::path& v : viejas) fs::remove_all(v, ec);
    for (const fs::path& h : nuevas) {
      fs::rename(h, carpeta / h.filename(), ec);
      if (ec) {
        error = "no se puede mover " + h.filename().string();
        resultado = Importado::kError;
        break;
      }
    }
  }
  fs::remove_all(temporal, ec);
  return resultado;
}

}  // namespace nfsmw::partidas
