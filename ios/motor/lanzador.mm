// El lanzador de iOS: la pantalla de inicio de la app de Android (SetupActivity, con lo que hacen
// Ajustes, MotorNativo, IsoXex e Idioma) en UIKit. Sale antes que el juego, en el mismo proceso:
// el SDL_main del SDK en iOS (windowed_app_main_sdl.cpp) llama a NfsmwLanzadorIos, que pinta la
// pantalla en su propia ventana, espera a Jugar y devuelve la linea de comandos con lo elegido.
// Despues el juego arranca como siempre, con la ventana de SDL.
//
// Lo que cambia respecto a Android:
//   - El juego: la ISO elegida en Archivos (se lee donde esta, con un marcador de seguridad), una
//     ISO en la carpeta de la app (Documents, que ven Archivos y el Finder) o el juego extraido en
//     Documents/game_root. La ISO no se mapea entera: ver disc_image_device.cpp del SDK.
//   - No estan las opciones que en iOS no existen: la resolucion de la pantalla, la API grafica
//     (solo Vulkan, por MoltenVK), el sonido por AAudio, los hilos fijados, el driver de la GPU, la
//     sonda de Vulkan y el modo de compatibilidad (sin Xenos en iOS). El servidor LAN, de momento,
//     tampoco.
//   - El editor del mando tactil es el de la partida (el engranaje junto a OCULTAR).
//   - Volver aqui es cerrar la app y abrirla: iOS no deja que una app se cierre sola.
//
// Las preferencias van en NSUserDefaults con las claves de Android ("ajustes.<clave>"), las mismas
// que lee el mando tactil. Los textos son los de la app de Android en sus 10 idiomas (textos.py).

#import <CommonCrypto/CommonDigest.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <os/log.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "ios_app.h"

namespace {

// ---------------------------------------------------------------------------------------------
//  Textos (Idioma.java): textos.json, que compilar.sh mete en la app
// ---------------------------------------------------------------------------------------------

// "" = el del telefono. Despues, como en Android, por el nombre de cada idioma en su lengua.
NSString* const CODIGOS_IDIOMA[] = {@"", @"de", @"en", @"es", @"fr", @"it", @"pl", @"pt", @"ja", @"ko", @"zh-TW"};
NSString* const NOMBRES_IDIOMA[] = {@"idioma_auto", @"idioma_de", @"idioma_en", @"idioma_es", @"idioma_fr",
                                   @"idioma_it",   @"idioma_pl", @"idioma_pt", @"idioma_ja", @"idioma_ko",
                                   @"idioma_zh_tw"};

NSDictionary<NSString*, NSDictionary<NSString*, NSString*>*>* g_textos = nil;
NSDictionary<NSString*, NSString*>* g_tabla = nil;
NSDictionary<NSString*, NSString*>* g_ingles = nil;

NSString* IdiomaElegido() {
  NSString* v = [NSUserDefaults.standardUserDefaults stringForKey:@"ajustes.idioma"];
  return v ?: @"";
}

// El codigo de la tabla para un idioma de iOS: "es-ES" es "es", "zh-Hant-TW" es "zh-TW".
NSString* TablaPara(NSString* idioma) {
  if ([idioma hasPrefix:@"zh-Hant"] || [idioma hasPrefix:@"zh-TW"] || [idioma hasPrefix:@"zh-HK"]) {
    return g_textos[@"zh-TW"] ? @"zh-TW" : nil;
  }
  NSString* base = [idioma componentsSeparatedByString:@"-"].firstObject;
  return g_textos[base] ? base : nil;
}

void CargarTextos() {
  if (!g_textos) {
    NSString* ruta = [NSBundle.mainBundle pathForResource:@"textos" ofType:@"json"];
    NSData* datos = ruta ? [NSData dataWithContentsOfFile:ruta] : nil;
    id json = datos ? [NSJSONSerialization JSONObjectWithData:datos options:0 error:nil] : nil;
    g_textos = [json isKindOfClass:NSDictionary.class] ? json : @{};
    g_ingles = g_textos[@"en"] ?: @{};
  }
  NSString* codigo = nil;
  NSString* elegido = IdiomaElegido();
  if (elegido.length) {
    codigo = TablaPara(elegido);
  } else {
    for (NSString* idioma in NSLocale.preferredLanguages) {
      if ((codigo = TablaPara(idioma))) break;
    }
  }
  g_tabla = g_textos[codigo ?: @"en"] ?: g_ingles;
}

// ---------------------------------------------------------------------------------------------
//  Colores y preferencias (SetupActivity y Ajustes.java)
// ---------------------------------------------------------------------------------------------

UIColor* Color(uint32_t argb) {
  return [UIColor colorWithRed:((argb >> 16) & 0xFF) / 255.0
                         green:((argb >> 8) & 0xFF) / 255.0
                          blue:(argb & 0xFF) / 255.0
                         alpha:((argb >> 24) & 0xFF) / 255.0];
}
constexpr uint32_t ACENTO = 0xFF6F7432;
// El acento sobre fondo oscuro, para iconos y textos: el de siempre se pierde.
constexpr uint32_t ACENTO_CLARO = 0xFFB9BF6A;
constexpr uint32_t GRIS = 0xFFA0A0A0, AVISO = 0xFFFFB74D, FONDO = 0xFF121212, FONDO_MENU = 0xFF181818;
constexpr uint32_t ELEGIDO = 0xFF2E301E, TARJETA = 0xFF1E1E1E, BOTON = 0xFF2C2C2C, BLANCO = 0xFFFFFFFF;

NSString* Clave(NSString* k) { return [@"ajustes." stringByAppendingString:k]; }
id Leer(NSString* k) { return [NSUserDefaults.standardUserDefaults objectForKey:Clave(k)]; }
bool Hay(NSString* k) { return Leer(k) != nil; }
bool LeerBool(NSString* k, bool def) {
  id v = Leer(k);
  return [v isKindOfClass:NSNumber.class] ? [v boolValue] : def;
}
int LeerInt(NSString* k, int def) {
  id v = Leer(k);
  return [v isKindOfClass:NSNumber.class] ? [v intValue] : def;
}
NSString* LeerTexto(NSString* k, NSString* def) {
  id v = Leer(k);
  return [v isKindOfClass:NSString.class] ? v : def;
}
void Poner(NSString* k, id v) { [NSUserDefaults.standardUserDefaults setObject:v forKey:Clave(k)]; }
void Quitar(NSString* k) { [NSUserDefaults.standardUserDefaults removeObjectForKey:Clave(k)]; }

// Mando tactil, en la disposicion de conducir: con que se gira (Ajustes.modoGiro). Inclinar
// sigue en su clave de antes (girar_inclinando).
NSString* const GIRO_STICK = @"stick";
NSString* const GIRO_FLECHAS = @"flechas";
NSString* const GIRO_INCLINAR = @"inclinar";
NSString* ModoGiro() {
  if (LeerBool(@"girar_inclinando", false)) return GIRO_INCLINAR;
  return [LeerTexto(@"giro_carrera", GIRO_STICK) isEqualToString:GIRO_FLECHAS] ? GIRO_FLECHAS : GIRO_STICK;
}
void ModoGiro(NSString* v) {
  Poner(@"girar_inclinando", @([v isEqualToString:GIRO_INCLINAR]));
  Poner(@"giro_carrera", v);
}
constexpr int SENSIBILIDAD_MIN = 1, SENSIBILIDAD_MAX = 20;
// Los grados que hay que inclinar el movil para girar del todo: de 45 (1) a 6 (20).
float AnguloGiro(int sensibilidad) {
  const int s = std::max(SENSIBILIDAD_MIN, std::min(SENSIBILIDAD_MAX, sensibilidad));
  return float(45.0 * std::pow(6.0 / 45.0, (s - SENSIBILIDAD_MIN) / double(SENSIBILIDAD_MAX - SENSIBILIDAD_MIN)));
}

NSString* const AA_NO = @"no";
NSString* const AA_FXAA = @"fxaa";
// El motor nativo solo tiene FXAA: lo que viniera de antes (MSAA, "sin MSAA") es sin suavizado.
NSString* Antialiasing() { return [LeerTexto(@"antialiasing", AA_NO) isEqualToString:AA_FXAA] ? AA_FXAA : AA_NO; }

NSString* const SOMBRAS_RAPIDA = @"rapida";
NSString* const SOMBRAS_SUAVE = @"suave";
NSString* const SOMBRAS_XBOX = @"xbox360";
constexpr int FPS_SIN_LIMITE = 0;

bool GamertagValido(NSString* g) {
  return g.length == 0 ||
         [[NSPredicate predicateWithFormat:@"SELF MATCHES %@", @"[A-Za-z][A-Za-z0-9 ]{0,14}"] evaluateWithObject:g];
}

// --- Preajustes (Ajustes.aplicarAhorro, aplicarFabrica, aplicarMisAjustes)

// Los ajustes graficos que cambian los preajustes (y "sombras_cada", que ya no se usa).
NSString* const CLAVES_GRAFICOS[] = {@"limite_fps",   @"resolucion_interna", @"escalado",     @"antialiasing",
                                     @"sin_msaa",     @"anisotropico",       @"calidad_sombras", @"sombras_escala",
                                     @"sombras_cada", @"sombras_corte",      @"reflejos_coche", @"reflejo_asfalto",
                                     @"perfil_gpu_debil"};

// Los valores guardados de CLAVES_GRAFICOS con su tipo, en el JSON de Android:
// {"clave": ["b", true], "otra": ["i", 30], ...}.
NSString* FotoGraficos() {
  NSMutableDictionary* foto = [NSMutableDictionary dictionary];
  for (NSString* clave : CLAVES_GRAFICOS) {
    id v = Leer(clave);
    if ([v isKindOfClass:NSString.class]) {
      foto[clave] = @[@"s", v];
    } else if ([v isKindOfClass:NSNumber.class]) {
      const bool booleano = CFGetTypeID((__bridge CFTypeRef)v) == CFBooleanGetTypeID();
      foto[clave] = booleano ? @[@"b", @([v boolValue])] : @[@"i", @([v intValue])];
    }
  }
  NSData* datos = [NSJSONSerialization dataWithJSONObject:foto options:NSJSONWritingSortedKeys error:nil];
  return datos ? [[NSString alloc] initWithData:datos encoding:NSUTF8StringEncoding] : @"{}";
}

// Antes de aplicar otro preajuste se guardan los ajustes del usuario, si los ha tocado desde el
// ultimo preajuste: asi "Ahorro" y luego "Fabrica" no pisan los suyos.
void GuardarMisAjustes() {
  NSString* ahora = FotoGraficos();
  if (![ahora isEqualToString:LeerTexto(@"huella_preajuste", @"")]) {
    Poner(@"mis_ajustes", ahora);
  }
}

// 30 fps, 1024x576 con FSR, sin suavizado ni anisotropico, sombras rapidas y de cerca y un solo
// reflejo del coche.
void AplicarAhorro() {
  GuardarMisAjustes();
  Poner(@"limite_fps", @30);
  Poner(@"resolucion_interna", @"1024x576");
  Poner(@"escalado", @"fsr");
  Poner(@"antialiasing", AA_NO);
  Poner(@"anisotropico", @0);
  Poner(@"calidad_sombras", SOMBRAS_RAPIDA);
  Poner(@"sombras_escala", @100);
  Poner(@"sombras_corte", @200);
  Poner(@"reflejos_coche", @1);
  Poner(@"huella_preajuste", FotoGraficos());
}

// Los de una instalacion nueva. En iOS no hay perfil de GPU debil: todos los iPhone que llegan a
// iOS 16 tienen GPU de sobra para los de fabrica.
void AplicarFabrica() {
  GuardarMisAjustes();
  for (NSString* clave : CLAVES_GRAFICOS) Quitar(clave);
  Poner(@"huella_preajuste", FotoGraficos());
}

void AplicarMisAjustes() {
  NSData* datos = [LeerTexto(@"mis_ajustes", @"") dataUsingEncoding:NSUTF8StringEncoding];
  NSDictionary* foto = datos.length ? [NSJSONSerialization JSONObjectWithData:datos options:0 error:nil] : nil;
  if (![foto isKindOfClass:NSDictionary.class]) return;  // una foto rota no cambia nada
  for (NSString* clave : CLAVES_GRAFICOS) {
    NSArray* v = foto[clave];
    if (![v isKindOfClass:NSArray.class] || v.count != 2) {
      Quitar(clave);
    } else if ([v[0] isEqual:@"b"]) {
      Poner(clave, @([v[1] boolValue]));
    } else if ([v[0] isEqual:@"i"]) {
      Poner(clave, @([v[1] intValue]));
    } else {
      Poner(clave, [v[1] description]);
    }
  }
}

// ---------------------------------------------------------------------------------------------
//  La edicion del juego de esta app (edicion.json, de la compilacion) y donde esta el juego
// ---------------------------------------------------------------------------------------------

struct Edicion {
  NSString* nombre = @"?";
  NSString* sha = @"";
  int idioma = -1;
  int pais = -1;
};

Edicion LeerEdicion() {
  Edicion e;
  NSString* ruta = [NSBundle.mainBundle pathForResource:@"edicion" ofType:@"json"];
  NSData* datos = ruta ? [NSData dataWithContentsOfFile:ruta] : nil;
  NSDictionary* j = datos ? [NSJSONSerialization JSONObjectWithData:datos options:0 error:nil] : nil;
  if (![j isKindOfClass:NSDictionary.class]) return e;
  if ([j[@"nombre"] isKindOfClass:NSString.class]) e.nombre = j[@"nombre"];
  if ([j[@"sha256"] isKindOfClass:NSString.class]) e.sha = [j[@"sha256"] lowercaseString];
  if ([j[@"idioma"] isKindOfClass:NSNumber.class]) e.idioma = [j[@"idioma"] intValue];
  if ([j[@"pais"] isKindOfClass:NSNumber.class]) e.pais = [j[@"pais"] intValue];
  return e;
}

NSString* Documentos() { return [NSHomeDirectory() stringByAppendingPathComponent:@"Documents"]; }

enum class Fuente { NINGUNA, ISO_ELEGIDA, ISO_DOCUMENTOS, CARPETA };

struct Juego {
  Fuente fuente = Fuente::NINGUNA;
  NSString* ruta = nil;
  NSString* nombre = nil;
  // Hay una ISO elegida, pero iOS ya no da acceso a ella (se movio o se borro).
  bool elegidaSinAcceso = false;
};

// La ISO elegida, con su acceso de seguridad abierto hasta que se cierre la app: el juego la lee
// mientras dura la partida.
NSURL* g_iso_accedida = nil;
bool g_iso_con_acceso = false;

NSURL* ResolverIsoElegida(bool* sinAcceso) {
  NSData* marcador = [NSUserDefaults.standardUserDefaults dataForKey:Clave(@"iso_marcador")];
  if (!marcador) return nil;
  BOOL viejo = NO;
  NSURL* url = [NSURL URLByResolvingBookmarkData:marcador options:0 relativeToURL:nil bookmarkDataIsStale:&viejo error:nil];
  if (!url) {
    *sinAcceso = true;
    return nil;
  }
  if (g_iso_accedida && ![g_iso_accedida isEqual:url]) {
    if (g_iso_con_acceso) [g_iso_accedida stopAccessingSecurityScopedResource];
    g_iso_accedida = nil;
  }
  if (!g_iso_accedida) {
    // NO tambien con las de la propia carpeta de la app, que no lo necesitan.
    g_iso_con_acceso = [url startAccessingSecurityScopedResource];
    g_iso_accedida = url;
  }
  if (access(url.fileSystemRepresentation, R_OK) != 0) {
    *sinAcceso = true;
    return nil;
  }
  if (viejo) {
    NSData* nuevo = [url bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
    if (nuevo) Poner(@"iso_marcador", nuevo);
  }
  return url;
}

// La elegida con el boton; si no, el juego extraido de Documents/game_root; si no, la primera ISO
// de Documents.
Juego BuscarJuego() {
  Juego j;
  bool sinAcceso = false;
  if (NSURL* url = ResolverIsoElegida(&sinAcceso)) {
    j.fuente = Fuente::ISO_ELEGIDA;
    j.ruta = url.path;
    j.nombre = LeerTexto(@"iso_nombre", url.lastPathComponent);
    return j;
  }
  j.elegidaSinAcceso = sinAcceso;
  NSFileManager* fm = NSFileManager.defaultManager;
  NSString* docs = Documentos();
  NSString* carpeta = [docs stringByAppendingPathComponent:@"game_root"];
  if ([fm fileExistsAtPath:[carpeta stringByAppendingPathComponent:@"default.xex"]]) {
    j.fuente = Fuente::CARPETA;
    j.ruta = carpeta;
    j.nombre = @"game_root";
    return j;
  }
  NSArray* ficheros = [[fm contentsOfDirectoryAtPath:docs error:nil] sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
  for (NSString* f in ficheros) {
    if ([f.pathExtension.lowercaseString isEqualToString:@"iso"]) {
      j.fuente = Fuente::ISO_DOCUMENTOS;
      j.ruta = [docs stringByAppendingPathComponent:f];
      j.nombre = f;
      return j;
    }
  }
  return j;
}

// La biblioteca de shaders del renderizador nativo: la de la app (compilar.sh la mete) o una
// copiada a Documents o junto al juego extraido (nfsmw_nativo_sistema.cpp mira ahi).
bool HayBiblioteca(const Juego& j) {
  NSFileManager* fm = NSFileManager.defaultManager;
  NSString* nombre = @"nfsmw_shaders.nfsp";
  if ([fm fileExistsAtPath:[NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:nombre]] ||
      [fm fileExistsAtPath:[Documentos() stringByAppendingPathComponent:nombre]]) {
    return true;
  }
  return j.fuente == Fuente::CARPETA && [fm fileExistsAtPath:[j.ruta stringByAppendingPathComponent:nombre]];
}

// ---------------------------------------------------------------------------------------------
//  IsoXex: de que edicion es la ISO (el SHA-256 de su default.xex) y si esta entera
// ---------------------------------------------------------------------------------------------

// xex de una ISO que se abrio bien pero no tiene el XDVDFS de Xbox 360: otro juego, la version de
// PC o de PS2, un .zip/.7z o un fichero roto.
const std::string NO_ES_ISO = "-";

struct Mirada {
  bool hecha = false;
  // SHA-256 del default.xex, "" si no se pudo leer o NO_ES_ISO.
  std::string xex;
  // Bytes que le faltan al fichero para contener todos sus archivos; 0 si esta entero.
  uint64_t faltan = 0;
};

constexpr uint64_t SECTOR = 2048;
constexpr uint8_t DIRECTORIO = 0x10;
// Donde empieza la particion de juego; los mismos que mira el SDK (DiscImageDevice::Verify).
constexpr uint64_t BASES[] = {0, 0x0FD90000, 0x02080000, 0x18300000, 0x0000FB20, 0x00020600};
constexpr uint64_t MAX_DIRECTORIO = 1 << 20, MAX_XEX = 64 << 20;
constexpr int MAX_PROFUNDIDAD = 8;

bool LeerEn(int fd, uint64_t desde, void* destino, size_t cuantos) {
  auto* p = static_cast<uint8_t*>(destino);
  while (cuantos) {
    const ssize_t n = pread(fd, p, cuantos, off_t(desde));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    p += n;
    desde += uint64_t(n);
    cuantos -= size_t(n);
  }
  return true;
}

uint32_t Le32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;  // arm64: little endian, como el XDVDFS
}
uint16_t Le16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return v;
}

std::string Sha256(const std::vector<uint8_t>& datos) {
  uint8_t resumen[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(datos.data(), CC_LONG(datos.size()), resumen);
  static const char* const hex = "0123456789abcdef";
  std::string s;
  for (uint8_t b : resumen) {
    s += hex[b >> 4];
    s += hex[b & 15];
  }
  return s;
}

// Recorre el arbol de directorios apuntando el default.xex de la raiz y donde acaba el ultimo
// archivo. Cada entrada lleva los desplazamientos (en palabras de 4 bytes) de sus dos hijas en el
// arbol binario, el sector, el tamano, los atributos y el nombre.
struct Recorrido {
  int fd;
  uint64_t base;
  uint64_t sectorXex = 0, tamXex = 0, fin = 0;

  void directorio(uint64_t sector, uint64_t tam, int profundidad) {
    fin = std::max(fin, base + sector * SECTOR + tam);
    if (tam == 0 || tam > MAX_DIRECTORIO || profundidad > MAX_PROFUNDIDAD) return;
    std::vector<uint8_t> tabla(tam);
    if (!LeerEn(fd, base + sector * SECTOR, tabla.data(), tabla.size())) {
      return;  // el directorio cae en lo que falta: ya cuenta en `fin`
    }
    std::vector<size_t> pila{0};
    int vueltas = 0;
    while (!pila.empty() && vueltas++ < 8192) {
      const size_t off = pila.back();
      pila.pop_back();
      if (off + 14 > tabla.size()) continue;
      const uint16_t izq = Le16(&tabla[off]), der = Le16(&tabla[off + 2]);
      const uint64_t sectorHijo = Le32(&tabla[off + 4]), tamHijo = Le32(&tabla[off + 8]);
      const uint8_t atributos = tabla[off + 12], largo = tabla[off + 13];
      if (izq != 0 && izq != 0xFFFF && pila.size() < 256) pila.push_back(size_t(izq) * 4);
      if (der != 0 && der != 0xFFFF && pila.size() < 256) pila.push_back(size_t(der) * 4);
      if (largo == 0 || off + 14 + largo > tabla.size()) continue;
      if (atributos & DIRECTORIO) {
        directorio(sectorHijo, tamHijo, profundidad + 1);
        continue;
      }
      fin = std::max(fin, base + sectorHijo * SECTOR + tamHijo);
      std::string nombre(reinterpret_cast<const char*>(&tabla[off + 14]), largo);
      std::transform(nombre.begin(), nombre.end(), nombre.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      if (profundidad == 0 && nombre == "default.xex") {
        sectorXex = sectorHijo;
        tamXex = tamHijo;
      }
    }
  }
};

Mirada MirarIso(const char* ruta) {
  Mirada m;
  const int fd = open(ruta, O_RDONLY);
  if (fd < 0) return m;
  struct stat datos {};
  const uint64_t tam = fstat(fd, &datos) == 0 ? uint64_t(datos.st_size) : 0;
  m.hecha = true;
  m.xex = NO_ES_ISO;
  for (uint64_t base : BASES) {
    uint8_t vd[28];
    if (!LeerEn(fd, base + 32 * SECTOR, vd, sizeof(vd)) || std::memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20) != 0) continue;
    Recorrido r{fd, base};
    r.directorio(Le32(vd + 20), Le32(vd + 24), 0);
    m.xex.clear();
    if (r.tamXex > 0 && r.tamXex <= MAX_XEX) {
      std::vector<uint8_t> xex(r.tamXex);
      if (LeerEn(fd, base + r.sectorXex * SECTOR, xex.data(), xex.size())) m.xex = Sha256(xex);
    }
    m.faltan = tam > 0 && r.fin > tam ? r.fin - tam : 0;
    break;
  }
  close(fd);
  return m;
}

Mirada MirarJuego(const Juego& j) {
  if (j.fuente == Fuente::CARPETA) {
    Mirada m;
    NSData* xex = [NSData dataWithContentsOfFile:[j.ruta stringByAppendingPathComponent:@"default.xex"]];
    m.hecha = true;
    if (xex.length > 0 && xex.length <= MAX_XEX) {
      const auto* p = static_cast<const uint8_t*>(xex.bytes);
      m.xex = Sha256(std::vector<uint8_t>(p, p + xex.length));
    }
    return m;
  }
  return MirarIso(j.ruta.fileSystemRepresentation);
}

// ---------------------------------------------------------------------------------------------
//  La linea de comandos del juego (MotorNativo.argumentos)
// ---------------------------------------------------------------------------------------------

// anisotropic_override del SDK (0, 2 = 2x, 3 = 4x, 5 = 16x) a nfsmw_nativo_anisotropico.
int Anisotropico(int sdk) {
  switch (sdk) {
    case 2: return 2;
    case 3: return 4;
    case 4: return 8;
    case 5: return 16;
    default: return 0;
  }
}

// Ancho entre alto de la pantalla entera, en horizontal.
double ProporcionPantalla() {
  const CGSize s = UIScreen.mainScreen.nativeBounds.size;
  return std::max(s.width, s.height) / std::max(1.0, double(std::min(s.width, s.height)));
}

std::vector<std::string> Argumentos(const Juego& j, const Edicion& edicion, const std::string& argv0) {
  std::vector<std::string> a{argv0};
  auto poner = [&a](NSString* s) { a.emplace_back(s.UTF8String); };
  auto bandera = [](bool v) { return v ? @"true" : @"false"; };
  poner([@"--game_data_root=" stringByAppendingString:j.ruta]);

  // La pantalla: con el 16:9 del centro, las bandas las pone el presentador; estirando o con la
  // pantalla ancha, la imagen llena la pantalla entera (el mando tactil lo sabe por sus ajustes).
  const bool panoramico = LeerBool(@"panoramico", false);
  const bool estirar = LeerBool(@"estirar", false) && !panoramico;
  poner([@"--present_letterbox=" stringByAppendingString:bandera(!estirar && !panoramico)]);
  poner(@"--present_allow_overscan_cutoff=false");
  poner(@"--present_safe_area_x=100");
  poner(@"--present_safe_area_y=100");
  poner([@"--present_effect=" stringByAppendingString:LeerTexto(@"escalado", @"bilinear")]);
  if (panoramico) {
    poner([NSString stringWithFormat:@"--nfsmw_proporcion_pantalla=%.4f", ProporcionPantalla()]);
  }

  // Graficos y sonido
  poner([@"--nfsmw_resolucion_interna=" stringByAppendingString:LeerTexto(@"resolucion_interna", @"1280x720")]);
  const int fps = LeerInt(@"limite_fps", 60);
  poner(fps == FPS_SIN_LIMITE ? @"--nfsmw_limite_fps=sin_limite" : [NSString stringWithFormat:@"--nfsmw_limite_fps=%d", fps]);
  // MSAA no hay: la escena se pinta en una pasada. Solo FXAA.
  poner([Antialiasing() isEqualToString:AA_FXAA] ? @"--nfsmw_antialiasing=fxaa" : @"--nfsmw_antialiasing=apagado");
  poner([NSString stringWithFormat:@"--nfsmw_nativo_anisotropico=%d", Anisotropico(LeerInt(@"anisotropico", 3))]);
  poner([@"--nfsmw_tratamiento_visual=" stringByAppendingString:LeerTexto(@"filtro_color", @"original")]);
  poner([@"--nfsmw_nativo_sin_desenfoque=" stringByAppendingString:bandera(!LeerBool(@"desenfoque", false))]);
  poner([@"--nfsmw_posproceso=" stringByAppendingString:LeerTexto(@"filtro_imagen", @"apagado")]);
  NSString* sombras = LeerTexto(@"calidad_sombras", SOMBRAS_RAPIDA);
  poner([@"--nfsmw_nativo_pcf_barato=" stringByAppendingString:bandera([sombras isEqualToString:SOMBRAS_RAPIDA])]);
  poner([@"--nfsmw_sombras_sin_vegetacion=" stringByAppendingString:bandera(![sombras isEqualToString:SOMBRAS_XBOX])]);
  poner([NSString stringWithFormat:@"--nfsmw_nativo_sombras_escala=%d", LeerInt(@"sombras_escala", 100)]);
  // Siempre cada fotograma: cada 2 deja el mapa un fotograma por detras del coche.
  poner(@"--nfsmw_sombras_cada=1");
  poner([NSString stringWithFormat:@"--nfsmw_sombras_corte=%d", LeerInt(@"sombras_corte", 150)]);
  poner([NSString stringWithFormat:@"--nfsmw_cubemap_caras_max=%d", LeerInt(@"reflejos_coche", 6)]);
  poner([@"--nfsmw_reflejo_carretera=" stringByAppendingString:bandera(LeerBool(@"reflejo_asfalto", true))]);
  poner([@"--nfsmw_resplandor_cielo=" stringByAppendingString:LeerTexto(@"resplandor_cielo", @"natural")]);
  // El volumen del juego, siempre el original: por encima de 100 su limitador lo comprime.
  poner(@"--audio_ganancia_pct=100");
  // La vibracion del juego, a los mandos fisicos y, con el mando tactil, al movil.
  poner([@"--input_vibracion=" stringByAppendingString:bandera(LeerBool(@"vibracion", true))]);
  poner(@"--nfsmw_renderizador=nativo");

  // El idioma y el pais de la consola, los de la edicion de esta app.
  if (edicion.idioma >= 0) poner([NSString stringWithFormat:@"--user_language=%d", edicion.idioma]);
  if (edicion.pais >= 0) poner([NSString stringWithFormat:@"--user_country=%d", edicion.pais]);
  NSString* gamertag = LeerTexto(@"gamertag", @"");
  if (gamertag.length && GamertagValido(gamertag)) {
    // Un solo argumento aunque lleve espacios: va en el vector, sin shell.
    poner([@"--user_gamertag=" stringByAppendingString:gamertag]);
  }
  poner(LeerBool(@"registro", false) ? @"--log_level=debug" : @"--log_level=info");
  return a;
}

// ---------------------------------------------------------------------------------------------
//  Piezas de la pantalla (los equivalentes de las de SetupActivity)
// ---------------------------------------------------------------------------------------------

UILabel* Etiqueta(NSString* texto, CGFloat tam, UIFontWeight peso, uint32_t color) {
  UILabel* l = [UILabel new];
  l.text = texto;
  l.font = [UIFont systemFontOfSize:tam weight:peso];
  l.textColor = Color(color);
  l.numberOfLines = 0;
  return l;
}

UILabel* Resumen(NSString* texto) { return Etiqueta(texto, 13, UIFontWeightRegular, GRIS); }

// Una columna con margen arriba y abajo, como el padding de un LinearLayout.
UIStackView* Columna(NSArray<UIView*>* vistas, CGFloat arriba = 0, CGFloat abajo = 0, CGFloat entre = 0) {
  UIStackView* s = [[UIStackView alloc] initWithArrangedSubviews:vistas];
  s.axis = UILayoutConstraintAxisVertical;
  s.alignment = UIStackViewAlignmentFill;
  s.spacing = entre;
  s.layoutMarginsRelativeArrangement = YES;
  s.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(arriba, 0, abajo, 0);
  return s;
}

UIView* Titulo(NSString* texto) { return Columna(@[Etiqueta(texto, 16, UIFontWeightBold, BLANCO)], 12); }

UIView* Espacio(CGFloat alto) {
  UIView* v = [UIView new];
  [v.heightAnchor constraintEqualToConstant:alto].active = YES;
  return v;
}

UIView* Seccion(NSString* texto) {
  UIView* raya = [UIView new];
  raya.backgroundColor = Color(ACENTO);
  [raya.heightAnchor constraintEqualToConstant:2].active = YES;
  return Columna(@[Etiqueta(texto, 22, UIFontWeightBold, BLANCO), raya], 8, 16, 8);
}

UIView* Tarjeta(NSArray<UIView*>* vistas) {
  UIView* t = [UIView new];
  t.backgroundColor = Color(TARJETA);
  t.layer.cornerRadius = 16;
  UIStackView* s = Columna(vistas);
  s.translatesAutoresizingMaskIntoConstraints = NO;
  [t addSubview:s];
  [NSLayoutConstraint activateConstraints:@[
    [s.leadingAnchor constraintEqualToAnchor:t.leadingAnchor constant:16],
    [s.trailingAnchor constraintEqualToAnchor:t.trailingAnchor constant:-16],
    [s.topAnchor constraintEqualToAnchor:t.topAnchor constant:16],
    [s.bottomAnchor constraintEqualToAnchor:t.bottomAnchor constant:-16],
  ]];
  return t;
}

UIConfigurationTextAttributesTransformer Letra(UIFont* letra) {
  return ^NSDictionary<NSAttributedStringKey, id>*(NSDictionary<NSAttributedStringKey, id>* entrada) {
    NSMutableDictionary* d = [entrada mutableCopy];
    d[NSFontAttributeName] = letra;
    return d;
  };
}

UIButton* Boton(NSString* texto, uint32_t fondo, uint32_t letra, void (^accion)(void)) {
  UIButtonConfiguration* c = [UIButtonConfiguration filledButtonConfiguration];
  c.title = texto;
  c.baseBackgroundColor = Color(fondo);
  c.baseForegroundColor = Color(letra);
  c.background.cornerRadius = 10;
  c.cornerStyle = UIButtonConfigurationCornerStyleFixed;
  c.contentInsets = NSDirectionalEdgeInsetsMake(12, 16, 12, 16);
  c.titleTextAttributesTransformer = Letra([UIFont systemFontOfSize:15 weight:UIFontWeightMedium]);
  return [UIButton buttonWithConfiguration:c primaryAction:[UIAction actionWithHandler:^(UIAction*) { accion(); }]];
}

UIView* Interruptor(NSString* titulo, NSString* resumen, bool valor, void (^guardar)(bool),
                    UISwitch* __strong* salida = nullptr) {
  UILabel* l = Etiqueta(titulo, 16, UIFontWeightRegular, BLANCO);
  UISwitch* s = [UISwitch new];
  s.on = valor;
  s.onTintColor = Color(ACENTO);
  [s addAction:[UIAction actionWithHandler:^(UIAction* a) { guardar(((UISwitch*)a.sender).isOn); }]
      forControlEvents:UIControlEventValueChanged];
  [s setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
  [s setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
  UIStackView* fila = [[UIStackView alloc] initWithArrangedSubviews:@[l, s]];
  fila.axis = UILayoutConstraintAxisHorizontal;
  fila.alignment = UIStackViewAlignmentCenter;
  fila.spacing = 12;
  NSMutableArray* vistas = [NSMutableArray arrayWithObject:fila];
  if (resumen.length) [vistas addObject:Resumen(resumen)];
  if (salida) *salida = s;
  return Columna(vistas, 10, 6, 2);
}

UIImage* Circulo(bool elegido) {
  UIImage* i = [UIImage systemImageNamed:elegido ? @"largecircle.fill.circle" : @"circle"];
  return [i imageWithTintColor:Color(elegido ? ACENTO_CLARO : 0xFF9E9E9E) renderingMode:UIImageRenderingModeAlwaysOriginal];
}

// Opciones a la vista: en fila (UISegmentedControl) si caben, como en Android; si no, en columna.
UIView* Selector(NSString* titulo, NSString* resumen, NSArray<NSString*>* textos, NSArray* valores, id actual,
                 bool horizontal, void (^guardar)(id)) {
  NSMutableArray* vistas = [NSMutableArray array];
  if (titulo.length) [vistas addObject:Titulo(titulo)];
  if (resumen.length) [vistas addObject:Resumen(resumen)];
  NSUInteger total = 0, mayor = 0;
  for (NSString* t in textos) {
    total += t.length;
    mayor = std::max(mayor, t.length);
  }
  const NSUInteger elegido = [valores indexOfObject:actual];
  if (horizontal && total <= 30 && mayor <= 14) {
    UISegmentedControl* sc = [[UISegmentedControl alloc] initWithItems:textos];
    sc.selectedSegmentIndex = elegido == NSNotFound ? UISegmentedControlNoSegment : NSInteger(elegido);
    sc.selectedSegmentTintColor = Color(ACENTO);
    sc.backgroundColor = Color(BOTON);
    [sc setTitleTextAttributes:@{NSForegroundColorAttributeName : UIColor.whiteColor} forState:UIControlStateNormal];
    [sc addAction:[UIAction actionWithHandler:^(UIAction* a) {
          const NSInteger i = ((UISegmentedControl*)a.sender).selectedSegmentIndex;
          if (i >= 0 && NSUInteger(i) < valores.count) guardar(valores[i]);
        }]
        forControlEvents:UIControlEventValueChanged];
    [vistas addObject:Columna(@[sc], 8)];
  } else {
    UIStackView* opciones = Columna(@[], 4);
    __weak UIStackView* columna = opciones;
    for (NSUInteger i = 0; i < textos.count; ++i) {
      UIButtonConfiguration* c = [UIButtonConfiguration plainButtonConfiguration];
      c.title = textos[i];
      c.image = Circulo(i == elegido);
      c.imagePadding = 10;
      c.baseForegroundColor = UIColor.whiteColor;
      c.contentInsets = NSDirectionalEdgeInsetsMake(7, 0, 7, 0);
      c.titleTextAttributesTransformer = Letra([UIFont systemFontOfSize:15]);
      UIButton* b = [UIButton buttonWithConfiguration:c
                                        primaryAction:[UIAction actionWithHandler:^(UIAction*) {
                                          NSArray* botones = columna.arrangedSubviews;
                                          for (NSUInteger k = 0; k < botones.count; ++k) {
                                            UIButton* o = botones[k];
                                            UIButtonConfiguration* oc = o.configuration;
                                            oc.image = Circulo(k == i);
                                            o.configuration = oc;
                                          }
                                          guardar(valores[i]);
                                        }]];
      b.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
      [opciones addArrangedSubview:b];
    }
    [vistas addObject:opciones];
  }
  return Columna(vistas, 12, 12);
}

// Como Selector, pero en un menu desplegable: para las listas largas.
UIView* Desplegable(NSString* titulo, NSString* resumen, NSArray<NSString*>* textos, NSArray* valores, id actual,
                    void (^guardar)(id)) {
  NSMutableArray* vistas = [NSMutableArray array];
  if (titulo.length) [vistas addObject:Titulo(titulo)];
  if (resumen.length) [vistas addObject:Resumen(resumen)];
  NSUInteger elegido = [valores indexOfObject:actual];
  if (elegido == NSNotFound) elegido = 0;
  NSMutableArray<UIMenuElement*>* acciones = [NSMutableArray array];
  for (NSUInteger i = 0; i < textos.count; ++i) {
    UIAction* a = [UIAction actionWithTitle:textos[i] image:nil identifier:nil handler:^(UIAction*) { guardar(valores[i]); }];
    a.state = i == elegido ? UIMenuElementStateOn : UIMenuElementStateOff;
    [acciones addObject:a];
  }
  UIButtonConfiguration* c = [UIButtonConfiguration filledButtonConfiguration];
  c.baseBackgroundColor = Color(BOTON);
  c.baseForegroundColor = UIColor.whiteColor;
  c.indicator = UIButtonConfigurationIndicatorPopup;
  c.contentInsets = NSDirectionalEdgeInsetsMake(12, 16, 12, 16);
  c.titleTextAttributesTransformer = Letra([UIFont systemFontOfSize:15]);
  UIButton* b = [UIButton buttonWithConfiguration:c primaryAction:nil];
  b.menu = [UIMenu menuWithChildren:acciones];
  b.showsMenuAsPrimaryAction = YES;
  b.changesSelectionAsPrimaryAction = YES;
  b.contentHorizontalAlignment = UIControlContentHorizontalAlignmentFill;
  [vistas addObject:Columna(@[b], 8)];
  return Columna(vistas, 12, 12);
}

// Un deslizador de enteros, con su valor escrito debajo del titulo. Se guarda en cada paso.
UIView* Deslizador(NSString* titulo, NSString* resumen, int min, int max, int actual, NSString* (^texto)(int),
                   void (^guardar)(int)) {
  UILabel* valor = Etiqueta(texto(actual), 13, UIFontWeightRegular, BLANCO);
  UISlider* s = [UISlider new];
  s.minimumValue = min;
  s.maximumValue = max;
  s.value = std::max(min, std::min(max, actual));
  s.minimumTrackTintColor = Color(ACENTO);
  s.maximumTrackTintColor = Color(0xFF303030);
  __block int ultimo = int(s.value);
  [s addAction:[UIAction actionWithHandler:^(UIAction* a) {
       UISlider* d = (UISlider*)a.sender;
       const int n = int(std::lround(d.value));
       d.value = n;
       if (n == ultimo) return;
       ultimo = n;
       valor.text = texto(n);
       guardar(n);
     }]
      forControlEvents:UIControlEventValueChanged];
  NSMutableArray* vistas = [NSMutableArray arrayWithArray:@[Titulo(titulo), valor, Columna(@[s], 4, 4)]];
  if (resumen.length) [vistas addObject:Resumen(resumen)];
  return Columna(vistas, 6, 6);
}

UIScrollView* Panel(NSArray<UIView*>* tarjetas) {
  UIScrollView* sv = [UIScrollView new];
  sv.translatesAutoresizingMaskIntoConstraints = NO;
  sv.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive;
  sv.alwaysBounceVertical = YES;
  UIStackView* col = Columna(tarjetas, 8, 12, 8);
  col.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(8, 12, 12, 12);
  col.translatesAutoresizingMaskIntoConstraints = NO;
  [sv addSubview:col];
  [NSLayoutConstraint activateConstraints:@[
    [col.leadingAnchor constraintEqualToAnchor:sv.contentLayoutGuide.leadingAnchor],
    [col.trailingAnchor constraintEqualToAnchor:sv.contentLayoutGuide.trailingAnchor],
    [col.topAnchor constraintEqualToAnchor:sv.contentLayoutGuide.topAnchor],
    [col.bottomAnchor constraintEqualToAnchor:sv.contentLayoutGuide.bottomAnchor],
    [col.widthAnchor constraintEqualToAnchor:sv.frameLayoutGuide.widthAnchor],
  ]];
  return sv;
}

NSString* T(NSString* clave) { return NfsmwTexto(clave); }

// Las secciones del menu lateral, en su orden.
enum { S_JUEGO, S_PANTALLA, S_GRAFICOS, S_MULTIJUGADOR, S_CONTROLES, S_IDIOMA, S_AVANZADO, SECCIONES };
NSString* const NOMBRES_SECCION[] = {@"seccion_juego",     @"seccion_pantalla",  @"seccion_graficos",
                                     @"seccion_multijugador", @"seccion_controles", @"seccion_idioma",
                                     @"seccion_avanzado"};
NSString* const ICONOS_SECCION[] = {@"gamecontroller.fill", @"rectangle.inset.filled", @"paintpalette.fill",
                                    @"person.2.fill",       @"hand.tap.fill",          @"globe",
                                    @"wrench.and.screwdriver.fill"};

}  // namespace

NSString* NfsmwTexto(NSString* clave) {
  if (!g_tabla) CargarTextos();
  return g_tabla[clave] ?: g_ingles[clave] ?: clave;
}

// =============================================================================================
//  La pantalla
// =============================================================================================

@interface NfsmwLanzador : UIViewController <UIDocumentPickerDelegate, UITextFieldDelegate>
@property(nonatomic, readonly) BOOL terminado;
- (instancetype)initConArgv0:(std::string)argv0;
- (std::vector<std::string>)argumentos;
@end

@implementation NfsmwLanzador {
  std::string argv0_;
  std::vector<std::string> argumentos_;
  int seccion_;
  NSMutableArray<UIView*>* paneles_;
  NSMutableArray<UIButton*>* entradas_;
  UIButton* jugar_;
  UILabel* textoIso_;
  UILabel* textoEdicion_;
  UILabel* avisoJugar_;
  UILabel* avisoAcceso_;
  UILabel* avisoBiblioteca_;
  UILabel* errorGamertag_;
  UILabel* vibracionProbada_;
  Edicion edicion_;
  Juego juego_;
  Mirada mirada_;
  int miradas_;
}

- (instancetype)initConArgv0:(std::string)argv0 {
  self = [super initWithNibName:nil bundle:nil];
  if (self) {
    argv0_ = argv0;
    seccion_ = S_JUEGO;
    edicion_ = LeerEdicion();
    miradas_ = 0;
  }
  return self;
}

- (std::vector<std::string>)argumentos {
  return argumentos_;
}

- (BOOL)prefersStatusBarHidden {
  return YES;
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
  return UIInterfaceOrientationMaskLandscape;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  [self construir];
}

// En horizontal: a la izquierda el menu de secciones con Jugar debajo, que se ve desde todas; a
// la derecha, la seccion elegida. Se rehace entera al cambiar de idioma o aplicar un preajuste.
- (void)construir {
  CargarTextos();
  for (UIView* v in self.view.subviews) [v removeFromSuperview];
  UIView* raiz = self.view;
  raiz.backgroundColor = Color(FONDO);
  UILayoutGuide* segura = raiz.safeAreaLayoutGuide;

  // --- Menu lateral
  UIView* menu = [UIView new];
  menu.backgroundColor = Color(FONDO_MENU);
  menu.translatesAutoresizingMaskIntoConstraints = NO;
  [raiz addSubview:menu];

  UILabel* titulo = Etiqueta(T(@"app_name"), 18, UIFontWeightBold, BLANCO);
  UIStackView* lista = Columna(@[], 0, 0, 2);
  entradas_ = [NSMutableArray array];
  for (int i = 0; i < SECCIONES; ++i) {
    UIButtonConfiguration* c = [UIButtonConfiguration plainButtonConfiguration];
    c.title = T(NOMBRES_SECCION[i]);
    c.image = [UIImage systemImageNamed:ICONOS_SECCION[i]
                      withConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:17]];
    c.imagePadding = 14;
    c.contentInsets = NSDirectionalEdgeInsetsMake(12, 12, 12, 12);
    c.background.cornerRadius = 12;
    c.titleLineBreakMode = NSLineBreakByWordWrapping;
    __weak NfsmwLanzador* yo = self;
    UIButton* b = [UIButton buttonWithConfiguration:c
                                      primaryAction:[UIAction actionWithHandler:^(UIAction*) {
                                        [yo mostrarSeccion:i];
                                      }]];
    b.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
    [entradas_ addObject:b];
    [lista addArrangedSubview:b];
  }
  UIScrollView* desplazarMenu = [UIScrollView new];
  desplazarMenu.showsVerticalScrollIndicator = NO;
  lista.translatesAutoresizingMaskIntoConstraints = NO;
  [desplazarMenu addSubview:lista];
  [NSLayoutConstraint activateConstraints:@[
    [lista.leadingAnchor constraintEqualToAnchor:desplazarMenu.contentLayoutGuide.leadingAnchor],
    [lista.trailingAnchor constraintEqualToAnchor:desplazarMenu.contentLayoutGuide.trailingAnchor],
    [lista.topAnchor constraintEqualToAnchor:desplazarMenu.contentLayoutGuide.topAnchor],
    [lista.bottomAnchor constraintEqualToAnchor:desplazarMenu.contentLayoutGuide.bottomAnchor],
    [lista.widthAnchor constraintEqualToAnchor:desplazarMenu.frameLayoutGuide.widthAnchor],
  ]];

  __weak NfsmwLanzador* yo = self;
  UIButtonConfiguration* cj = [UIButtonConfiguration filledButtonConfiguration];
  cj.title = T(@"jugar");
  cj.image = [UIImage systemImageNamed:@"play.fill"];
  cj.imagePadding = 8;
  cj.cornerStyle = UIButtonConfigurationCornerStyleFixed;
  cj.background.cornerRadius = 12;
  cj.contentInsets = NSDirectionalEdgeInsetsMake(16, 12, 16, 12);
  cj.titleTextAttributesTransformer = Letra([UIFont systemFontOfSize:18 weight:UIFontWeightBold]);
  jugar_ = [UIButton buttonWithConfiguration:cj primaryAction:[UIAction actionWithHandler:^(UIAction*) { [yo jugar]; }]];
  jugar_.configurationUpdateHandler = ^(UIButton* b) {
    UIButtonConfiguration* c = b.configuration;
    c.baseBackgroundColor = Color(b.enabled ? ACENTO : 0xFF3A3A3A);
    c.baseForegroundColor = Color(b.enabled ? 0xFF121212 : 0xFF808080);
    b.configuration = c;
  };

  UIStackView* columna = [[UIStackView alloc] initWithArrangedSubviews:@[Columna(@[titulo], 0, 12), desplazarMenu,
                                                                        Columna(@[jugar_], 8)]];
  columna.axis = UILayoutConstraintAxisVertical;
  columna.translatesAutoresizingMaskIntoConstraints = NO;
  [menu addSubview:columna];

  // --- La seccion elegida. Todas se construyen al empezar y solo se ve una.
  UIView* contenido = [UIView new];
  contenido.translatesAutoresizingMaskIntoConstraints = NO;
  [raiz addSubview:contenido];
  paneles_ = [NSMutableArray arrayWithArray:@[
    Panel(@[[self tarjetaJuego]]), Panel(@[[self tarjetaPantalla]]), Panel(@[[self tarjetaGraficos]]),
    Panel(@[[self tarjetaPerfil]]), Panel(@[[self tarjetaControles]]), Panel(@[[self tarjetaIdioma]]),
    Panel(@[[self tarjetaAvanzado]])
  ]];
  for (UIView* p in paneles_) {
    [contenido addSubview:p];
    [NSLayoutConstraint activateConstraints:@[
      [p.leadingAnchor constraintEqualToAnchor:contenido.leadingAnchor],
      [p.trailingAnchor constraintEqualToAnchor:contenido.trailingAnchor],
      [p.topAnchor constraintEqualToAnchor:contenido.topAnchor],
      [p.bottomAnchor constraintEqualToAnchor:contenido.bottomAnchor],
    ]];
  }

  [NSLayoutConstraint activateConstraints:@[
    [menu.leadingAnchor constraintEqualToAnchor:raiz.leadingAnchor],
    [menu.topAnchor constraintEqualToAnchor:raiz.topAnchor],
    [menu.bottomAnchor constraintEqualToAnchor:raiz.bottomAnchor],
    [columna.leadingAnchor constraintEqualToAnchor:segura.leadingAnchor constant:12],
    [columna.widthAnchor constraintEqualToConstant:196],
    [menu.trailingAnchor constraintEqualToAnchor:columna.trailingAnchor constant:12],
    [columna.topAnchor constraintEqualToAnchor:segura.topAnchor constant:16],
    [columna.bottomAnchor constraintEqualToAnchor:segura.bottomAnchor constant:-12],
    [contenido.leadingAnchor constraintEqualToAnchor:menu.trailingAnchor],
    [contenido.trailingAnchor constraintEqualToAnchor:segura.trailingAnchor],
    [contenido.topAnchor constraintEqualToAnchor:segura.topAnchor],
    // El teclado (el del gamertag) solo encoge la seccion: el menu se queda debajo de el.
    [contenido.bottomAnchor constraintEqualToAnchor:raiz.keyboardLayoutGuide.topAnchor],
  ]];
  [self mostrarSeccion:seccion_];
  [self refrescar];
}

- (void)mostrarSeccion:(int)i {
  [self.view endEditing:YES];
  seccion_ = i;
  for (int j = 0; j < SECCIONES; ++j) {
    const bool esta = j == i;
    paneles_[j].hidden = !esta;
    UIButton* b = entradas_[j];
    UIButtonConfiguration* c = b.configuration;
    c.background.backgroundColor = esta ? Color(ELEGIDO) : UIColor.clearColor;
    c.baseForegroundColor = Color(esta ? BLANCO : 0xFFC8C8C8);
    c.image = [c.image imageWithTintColor:Color(esta ? ACENTO_CLARO : GRIS) renderingMode:UIImageRenderingModeAlwaysOriginal];
    c.titleTextAttributesTransformer = Letra([UIFont systemFontOfSize:15 weight:esta ? UIFontWeightBold : UIFontWeightRegular]);
    b.configuration = c;
  }
}

// ---- Juego

- (UIView*)tarjetaJuego {
  textoIso_ = Etiqueta(@"", 16, UIFontWeightRegular, BLANCO);
  textoEdicion_ = Resumen(@"");
  __weak NfsmwLanzador* yo = self;
  UIButton* elegir = Boton(T(@"elegir_iso"), BOTON, BLANCO, ^{ [yo elegirIso]; });
  elegir.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
  avisoAcceso_ = Etiqueta(T(@"ios_iso_sin_acceso"), 13, UIFontWeightRegular, AVISO);
  // Por que Jugar, en el menu, no se puede pulsar.
  avisoJugar_ = Etiqueta(T(@"jugar_sin_iso"), 13, UIFontWeightRegular, AVISO);
  avisoBiblioteca_ = Etiqueta(T(@"ios_sin_biblioteca"), 13, UIFontWeightRegular, AVISO);
  UIStackView* boton = [[UIStackView alloc] initWithArrangedSubviews:@[elegir, [UIView new]]];
  boton.axis = UILayoutConstraintAxisHorizontal;
  return Tarjeta(@[
    Seccion(T(@"seccion_juego")), Columna(@[Resumen(T(@"ios_app_con_juego"))], 0, 12), textoIso_,
    Columna(@[textoEdicion_], 4), Espacio(12), boton, Columna(@[Resumen(T(@"ios_ayuda_iso"))], 8),
    Columna(@[avisoAcceso_, avisoJugar_, avisoBiblioteca_], 8, 0, 6),
    Columna(@[Resumen(T(@"ios_cerrar_para_ajustes"))], 12)
  ]);
}

- (void)elegirIso {
  UIDocumentPickerViewController* p = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeData]
                                                                                                asCopy:NO];
  p.delegate = self;
  p.allowsMultipleSelection = NO;
  p.directoryURL = [NSURL fileURLWithPath:Documentos() isDirectory:YES];
  [self presentViewController:p animated:YES completion:nil];
}

// La ISO no se copia: se guarda un marcador para volver a abrirla donde esta en cada arranque.
- (void)documentPicker:(UIDocumentPickerViewController*)picker didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  NSURL* url = urls.firstObject;
  if (!url) return;
  const BOOL acceso = [url startAccessingSecurityScopedResource];
  NSData* marcador = [url bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
  if (acceso) [url stopAccessingSecurityScopedResource];
  if (!marcador) {
    UIAlertController* a = [UIAlertController alertControllerWithTitle:nil
                                                               message:T(@"ios_iso_sin_acceso")
                                                        preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:a animated:YES completion:nil];
    return;
  }
  Poner(@"iso_marcador", marcador);
  Poner(@"iso_nombre", url.lastPathComponent);
  [self refrescar];
}

- (void)refrescar {
  juego_ = BuscarJuego();
  switch (juego_.fuente) {
    case Fuente::NINGUNA: textoIso_.text = NfsmwTextoF(@"sin_iso", edicion_.nombre); break;
    case Fuente::CARPETA: textoIso_.text = NfsmwTextoF(@"ios_juego_extraido", juego_.nombre); break;
    default: textoIso_.text = NfsmwTextoF(@"iso_elegida", juego_.nombre); break;
  }
  avisoAcceso_.hidden = !juego_.elegidaSinAcceso;
  avisoBiblioteca_.hidden = HayBiblioteca(juego_);
  const bool puedeJugar = juego_.fuente != Fuente::NINGUNA;
  jugar_.enabled = puedeJugar;
  avisoJugar_.hidden = puedeJugar;

  // De que edicion es y si esta entera: son unos megas de lectura, en otro hilo. `miradas_` tira
  // las respuestas que llegan tarde (se eligio otra ISO entretanto).
  mirada_ = Mirada();
  const int n = ++miradas_;
  if (puedeJugar) {
    const Juego juego = juego_;
    __weak NfsmwLanzador* yo = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      const Mirada m = MirarJuego(juego);
      dispatch_async(dispatch_get_main_queue(), ^{
        [yo mirada:m numero:n];
      });
    });
  }
  [self pintarEdicion];
}

- (void)mirada:(Mirada)m numero:(int)n {
  if (n != miradas_) return;
  mirada_ = m;
  [self pintarEdicion];
}

// La app lleva el codigo de una edicion del juego: se dice cual y, si el default.xex de la ISO es
// de otra o a la ISO le falta el final, se avisa. No se impide jugar: lo que no se ha podido leer
// no demuestra nada.
- (void)pintarEdicion {
  const bool mirada = juego_.fuente != Fuente::NINGUNA && mirada_.hecha;
  const bool incompleta = mirada && mirada_.faltan > 0;
  const bool otraEdicion = mirada && !mirada_.xex.empty() && mirada_.xex != NO_ES_ISO && edicion_.sha.length &&
                           ![@(mirada_.xex.c_str()) isEqualToString:edicion_.sha];
  if (mirada && mirada_.xex == NO_ES_ISO) {
    textoEdicion_.text = T(@"iso_no_es_xbox");
  } else if (incompleta) {
    textoEdicion_.text = NfsmwTextoF(@"iso_incompleta", [NSByteCountFormatter stringFromByteCount:(long long)mirada_.faltan
                                                                                       countStyle:NSByteCountFormatterCountStyleFile]);
  } else if (otraEdicion) {
    textoEdicion_.text = NfsmwTextoF(@"ios_iso_otra_edicion", edicion_.nombre);
  } else {
    textoEdicion_.text = [NSString stringWithFormat:@"%@ %@", NfsmwTextoF(@"ios_edicion_app", edicion_.nombre), T(@"motor_nativo")];
  }
  textoEdicion_.textColor = Color(incompleta || otraEdicion || (mirada && mirada_.xex == NO_ES_ISO) ? AVISO : GRIS);
}

// ---- Pantalla

- (UIView*)tarjetaPantalla {
  // Estirar y pantalla ancha se excluyen: las dos llenan la pantalla, una deformando y la otra
  // no. Encender una apaga la otra; si venian las dos encendidas, se queda la pantalla ancha.
  if (LeerBool(@"estirar", false) && LeerBool(@"panoramico", false)) Poner(@"estirar", @NO);
  __block __weak UISwitch* estirar = nil;
  __block __weak UISwitch* panoramico = nil;
  UISwitch* s1 = nil;
  UISwitch* s2 = nil;
  UIView* filaEstirar = Interruptor(T(@"estirar"), T(@"estirar_resumen"), LeerBool(@"estirar", false), ^(bool v) {
    Poner(@"estirar", @(v));
    if (v && panoramico.on) {
      [panoramico setOn:NO animated:YES];
      Poner(@"panoramico", @NO);
    }
  }, &s1);
  UIView* filaPanoramico = Interruptor(T(@"panoramico"), T(@"panoramico_resumen"), LeerBool(@"panoramico", false), ^(bool v) {
    Poner(@"panoramico", @(v));
    if (v && estirar.on) {
      [estirar setOn:NO animated:YES];
      Poner(@"estirar", @NO);
    }
  }, &s2);
  estirar = s1;
  panoramico = s2;
  return Tarjeta(@[
    Seccion(T(@"seccion_pantalla")), filaEstirar, filaPanoramico,
    Interruptor(T(@"mostrar_fps"), T(@"mostrar_fps_resumen"), LeerBool(@"mostrar_fps", true), ^(bool v) {
      Poner(@"mostrar_fps", @(v));
    })
  ]);
}

// ---- Graficos

- (UIView*)tarjetaGraficos {
  auto guardar = ^(NSString* clave) {
    return ^(id v) { Poner(clave, v); };
  };
  return Tarjeta(@[
    Seccion(T(@"seccion_graficos")), [self filaPreajustes],
    Selector(T(@"resolucion_interna"), T(@"resolucion_interna_resumen"),
             @[T(@"res_interna_576"), T(@"res_interna_720"), T(@"res_interna_1080")],
             @[@"1024x576", @"1280x720", @"1920x1080"], LeerTexto(@"resolucion_interna", @"1280x720"), false,
             guardar(@"resolucion_interna")),
    Desplegable(T(@"limite_fps"), T(@"limite_fps_resumen"),
                @[NfsmwTextoF(@"fps_valor", 30), NfsmwTextoF(@"fps_valor", 60), NfsmwTextoF(@"fps_experimental", 90),
                  NfsmwTextoF(@"fps_experimental", 120), T(@"fps_sin_limite")],
                @[@30, @60, @90, @120, @(FPS_SIN_LIMITE)], @(LeerInt(@"limite_fps", 60)), guardar(@"limite_fps")),
    // El motor nativo pinta la escena en una pasada, sin MSAA: solo hay FXAA, y sin niveles.
    Selector(T(@"suavizado"), T(@"suavizado_resumen"), @[T(@"aa_no"), T(@"aa_fxaa")], @[AA_NO, AA_FXAA],
             Antialiasing(), true, guardar(@"antialiasing")),
    Selector(T(@"escalado"), T(@"escalado_resumen"),
             @[T(@"escalado_bilineal"), T(@"escalado_fsr"), T(@"escalado_cas")], @[@"bilinear", @"fsr", @"cas"],
             LeerTexto(@"escalado", @"bilinear"), false, guardar(@"escalado")),
    Selector(T(@"anisotropico"), T(@"anisotropico_resumen"), @[T(@"aniso_no"), T(@"aniso_2"), T(@"aniso_4"), T(@"aniso_16")],
             @[@0, @2, @3, @5], @(LeerInt(@"anisotropico", 3)), true, guardar(@"anisotropico")),
    Selector(T(@"calidad_sombras"), T(@"calidad_sombras_resumen"),
             @[T(@"sombras_rapida"), T(@"sombras_suave"), T(@"sombras_xbox360")],
             @[SOMBRAS_RAPIDA, SOMBRAS_SUAVE, SOMBRAS_XBOX], LeerTexto(@"calidad_sombras", SOMBRAS_RAPIDA), true,
             guardar(@"calidad_sombras")),
    Desplegable(T(@"resolucion_sombras"), nil,
                @[T(@"sombras_nativa"), T(@"sombras_alta"), T(@"sombras_muy_alta"), T(@"sombras_ultra")],
                @[@100, @150, @200, @250], @(LeerInt(@"sombras_escala", 100)), guardar(@"sombras_escala")),
    Selector(T(@"sombras_distancia"), T(@"sombras_distancia_resumen"),
             @[T(@"sombras_lejos"), T(@"sombras_media"), T(@"sombras_cerca")], @[@100, @150, @200],
             @(LeerInt(@"sombras_corte", 150)), true, guardar(@"sombras_corte")),
    Selector(T(@"reflejos_coche"), T(@"reflejos_coche_resumen"),
             @[T(@"calidad_alta"), T(@"calidad_media"), T(@"calidad_baja")], @[@6, @2, @1],
             @(LeerInt(@"reflejos_coche", 6)), true, guardar(@"reflejos_coche")),
    Interruptor(T(@"reflejo_asfalto"), T(@"reflejo_asfalto_resumen"), LeerBool(@"reflejo_asfalto", true),
                ^(bool v) { Poner(@"reflejo_asfalto", @(v)); }),
    Selector(T(@"resplandor_cielo"), T(@"resplandor_cielo_resumen"),
             @[T(@"cielo_natural"), T(@"cielo_original"), T(@"cielo_suave")], @[@"natural", @"original", @"suave"],
             LeerTexto(@"resplandor_cielo", @"natural"), true, guardar(@"resplandor_cielo")),
    Selector(T(@"filtro_color"), T(@"filtro_color_resumen"),
             @[T(@"filtro_color_original"), T(@"filtro_color_suave"), T(@"filtro_color_apagado")],
             @[@"original", @"suave", @"apagado"], LeerTexto(@"filtro_color", @"original"), true,
             guardar(@"filtro_color")),
    Interruptor(T(@"desenfoque"), T(@"desenfoque_resumen"), LeerBool(@"desenfoque", false),
                ^(bool v) { Poner(@"desenfoque", @(v)); }),
    Desplegable(T(@"filtro_imagen"), T(@"filtro_imagen_resumen"),
                @[T(@"filtro_imagen_no"), T(@"filtro_imagen_cine"), T(@"filtro_imagen_vivo"), T(@"filtro_imagen_calido"),
                  T(@"filtro_imagen_frio"), T(@"filtro_imagen_sepia"), T(@"filtro_imagen_noir"), T(@"filtro_imagen_crt")],
                @[@"apagado", @"cine", @"vivo", @"calido", @"frio", @"sepia", @"noir", @"crt"],
                LeerTexto(@"filtro_imagen", @"apagado"), guardar(@"filtro_imagen"))
  ]);
}

// Preajustes: cambian de golpe los ajustes graficos de abajo.
- (UIView*)filaPreajustes {
  __weak NfsmwLanzador* yo = self;
  UIButton* ahorro = Boton(T(@"preajuste_ahorro"), BOTON, BLANCO, ^{
    [yo confirmarPreajuste:@"preajuste_ahorro" detalle:@"preajuste_ahorro_detalle" aplicar:^{ AplicarAhorro(); }];
  });
  UIButton* fabrica = Boton(T(@"preajuste_fabrica"), BOTON, BLANCO, ^{
    [yo confirmarPreajuste:@"preajuste_fabrica" detalle:@"preajuste_fabrica_detalle" aplicar:^{ AplicarFabrica(); }];
  });
  UIStackView* fila = [[UIStackView alloc] initWithArrangedSubviews:@[ahorro, fabrica]];
  fila.axis = UILayoutConstraintAxisHorizontal;
  fila.distribution = UIStackViewDistributionFillEqually;
  fila.spacing = 8;
  NSMutableArray* vistas = [NSMutableArray arrayWithArray:@[Titulo(T(@"preajustes")), Resumen(T(@"preajustes_resumen")),
                                                            Columna(@[fila], 6)]];
  // Los del usuario, guardados al aplicar uno de los otros dos: solo si los hay.
  if (Hay(@"mis_ajustes")) {
    [vistas addObject:Columna(@[Boton(T(@"preajuste_mios"), BOTON, BLANCO, ^{
                                [yo confirmarPreajuste:@"preajuste_mios"
                                               detalle:@"preajuste_mios_detalle"
                                               aplicar:^{ AplicarMisAjustes(); }];
                              })],
                              4)];
  }
  return Columna(vistas, 0, 6);
}

// Pregunta antes de cambiar varios ajustes a la vez; al aplicar, la pantalla se rehace con ellos.
- (void)confirmarPreajuste:(NSString*)titulo detalle:(NSString*)detalle aplicar:(void (^)(void))aplicar {
  UIAlertController* a = [UIAlertController alertControllerWithTitle:T(titulo)
                                                             message:T(detalle)
                                                      preferredStyle:UIAlertControllerStyleAlert];
  __weak NfsmwLanzador* yo = self;
  [a addAction:[UIAlertAction actionWithTitle:T(@"editor_cancelar").localizedCapitalizedString
                                        style:UIAlertActionStyleCancel
                                      handler:nil]];
  [a addAction:[UIAlertAction actionWithTitle:T(@"preajuste_aplicar")
                                        style:UIAlertActionStyleDefault
                                      handler:^(UIAlertAction*) {
                                        aplicar();
                                        [yo construir];
                                      }]];
  [self presentViewController:a animated:YES completion:nil];
}

// ---- Multijugador: el perfil

// El gamertag se guarda segun se escribe, pero solo si es valido para Xbox 360; si no, se avisa y
// se queda el ultimo bueno. Vacio = el de fabrica, "User".
- (UIView*)tarjetaPerfil {
  UITextField* campo = [UITextField new];
  campo.text = LeerTexto(@"gamertag", @"");
  campo.placeholder = T(@"gamertag");
  campo.borderStyle = UITextBorderStyleRoundedRect;
  campo.font = [UIFont systemFontOfSize:16];
  campo.textColor = UIColor.whiteColor;
  campo.autocorrectionType = UITextAutocorrectionTypeNo;
  campo.spellCheckingType = UITextSpellCheckingTypeNo;
  campo.returnKeyType = UIReturnKeyDone;
  campo.delegate = self;
  [campo.heightAnchor constraintGreaterThanOrEqualToConstant:44].active = YES;
  errorGamertag_ = Etiqueta(T(@"gamertag_error"), 13, UIFontWeightRegular, AVISO);
  errorGamertag_.hidden = GamertagValido(campo.text);
  __weak UILabel* error = errorGamertag_;
  [campo addAction:[UIAction actionWithHandler:^(UIAction* a) {
           NSString* texto = ((UITextField*)a.sender).text ?: @"";
           const bool valido = GamertagValido(texto);
           error.hidden = valido;
           if (valido) Poner(@"gamertag", texto);
         }]
      forControlEvents:UIControlEventEditingChanged];
  return Tarjeta(@[
    Seccion(T(@"seccion_perfil")), Resumen(T(@"gamertag_resumen")), Espacio(8), campo, Columna(@[errorGamertag_], 6)
  ]);
}

- (BOOL)textFieldShouldReturn:(UITextField*)campo {
  [campo resignFirstResponder];
  return YES;
}

// ---- Controles tactiles

- (UIView*)tarjetaControles {
  vibracionProbada_ = Resumen(T(@"ios_vibracion_probada"));
  vibracionProbada_.hidden = YES;
  __weak UILabel* probada = vibracionProbada_;
  UIButton* probar = Boton(T(@"probar_vibracion"), BOTON, BLANCO, ^{
    NfsmwVibrarUnRato();
    probada.hidden = NO;
  });
  UIStackView* filaProbar = [[UIStackView alloc] initWithArrangedSubviews:@[probar, [UIView new]]];
  filaProbar.axis = UILayoutConstraintAxisHorizontal;
  return Tarjeta(@[
    Seccion(T(@"seccion_tactil")), Resumen(T(@"ios_editar_tactil_resumen")), Espacio(8),
    Interruptor(T(@"vibracion"), T(@"vibracion_resumen"), LeerBool(@"vibracion", true),
                ^(bool v) { Poner(@"vibracion", @(v)); }),
    Columna(@[filaProbar, vibracionProbada_], 2, 0, 6),
    Interruptor(T(@"menus_tactiles"), T(@"menus_tactiles_resumen"), LeerBool(@"menus_tactiles", true),
                ^(bool v) { Poner(@"menus_tactiles", @(v)); }),
    [self selectorGiro]
  ]);
}

// Con que se gira en las carreras. La sensibilidad solo cuenta inclinando el movil.
- (UIView*)selectorGiro {
  UIView* sensibilidad = Deslizador(
      T(@"sensibilidad_giro"), T(@"sensibilidad_giro_resumen"), SENSIBILIDAD_MIN, SENSIBILIDAD_MAX,
      LeerInt(@"sensibilidad_giro", 10),
      ^NSString*(int s) { return NfsmwTextoF(@"sensibilidad_valor", s, int(std::lround(AnguloGiro(s)))); },
      ^(int s) { Poner(@"sensibilidad_giro", @(s)); });
  UIStackView* conMargen = Columna(@[sensibilidad]);
  conMargen.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(0, 32, 0, 0);
  conMargen.hidden = ![ModoGiro() isEqualToString:GIRO_INCLINAR];
  __weak UIStackView* mostrar = conMargen;
  UIView* selector = Selector(T(@"giro_carrera"), T(@"giro_carrera_resumen"),
                              @[T(@"giro_stick"), T(@"giro_flechas"), T(@"giro_inclinar")],
                              @[GIRO_STICK, GIRO_FLECHAS, GIRO_INCLINAR], ModoGiro(), true, ^(id modo) {
                                ModoGiro(modo);
                                mostrar.hidden = ![modo isEqualToString:GIRO_INCLINAR];
                              });
  return Columna(@[selector, conMargen]);
}

// ---- Idioma: todos a la vista; al elegir uno, la pantalla se rehace en el.

- (UIView*)tarjetaIdioma {
  NSMutableArray* nombres = [NSMutableArray array];
  NSMutableArray* codigos = [NSMutableArray array];
  for (size_t i = 0; i < std::size(CODIGOS_IDIOMA); ++i) {
    [nombres addObject:T(NOMBRES_IDIOMA[i])];
    [codigos addObject:CODIGOS_IDIOMA[i]];
  }
  __weak NfsmwLanzador* yo = self;
  return Tarjeta(@[
    Seccion(T(@"seccion_idioma")), Selector(nil, nil, nombres, codigos, IdiomaElegido(), false, ^(id codigo) {
      if ([codigo isEqualToString:IdiomaElegido()]) return;
      [NSUserDefaults.standardUserDefaults setObject:codigo forKey:@"ajustes.idioma"];
      // Despues del toque: el boton que lo recibe es de la pantalla que se va a rehacer.
      dispatch_async(dispatch_get_main_queue(), ^{
        [yo construir];
      });
    })
  ]);
}

// ---- Avanzado

- (UIView*)tarjetaAvanzado {
  return Tarjeta(@[
    Seccion(T(@"seccion_avanzado")), Resumen(T(@"avanzado_resumen")),
    Interruptor(T(@"registro_detallado"), T(@"registro_detallado_resumen"), LeerBool(@"registro", false),
                ^(bool v) { Poner(@"registro", @(v)); })
  ]);
}

// ---- Jugar

- (void)jugar {
  [self.view endEditing:YES];
  juego_ = BuscarJuego();
  if (juego_.fuente == Fuente::NINGUNA) {
    [self refrescar];
    return;
  }
  argumentos_ = Argumentos(juego_, edicion_, argv0_);
  std::string linea;
  for (const auto& a : argumentos_) linea += " " + a;
  os_log(OS_LOG_DEFAULT, "[lanzador] jugar:%{public}s", linea.c_str());
  _terminado = YES;
}

@end

// =============================================================================================
//  La entrada: el SDL_main del SDK en iOS (windowed_app_main_sdl.cpp)
// =============================================================================================

// La pantalla en su propia ventana, encima de la de arranque de SDL, hasta que se pulse Jugar. La
// llama SDL_main en el hilo principal, dentro del bucle de UIKit: aqui se le da vueltas a ese
// bucle, como hace SDL con sus mensajes. Devuelve la linea de comandos del juego; vacia = la de
// siempre (sin escena a la que poner la ventana, que no deberia pasar).
std::vector<std::string> NfsmwLanzadorIos(int argc, char** argv) {
  UIWindowScene* escena = nil;
  for (UIScene* s in UIApplication.sharedApplication.connectedScenes) {
    if ([s isKindOfClass:UIWindowScene.class]) {
      escena = (UIWindowScene*)s;
      break;
    }
  }
  if (!escena) {
    os_log(OS_LOG_DEFAULT, "[lanzador] sin escena: el juego arranca sin lanzador");
    return {};
  }
  NfsmwLanzador* lanzador = [[NfsmwLanzador alloc] initConArgv0:argc > 0 ? argv[0] : "nfsmw"];
  UIWindow* ventana = [[UIWindow alloc] initWithWindowScene:escena];
  ventana.windowLevel = UIWindowLevelNormal + 2;
  ventana.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  ventana.rootViewController = lanzador;
  [ventana makeKeyAndVisible];
  while (!lanzador.terminado) {
    @autoreleasepool {
      [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
    }
  }
  std::vector<std::string> argumentos = [lanzador argumentos];
  ventana.hidden = YES;
  ventana.rootViewController = nil;
  return argumentos;
}
