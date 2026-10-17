// nfsmw - sonda de la conexion de red de EA (DirtySock NetConn), para el juego en LAN.
//
// Solo mide. sub_82819F48 es NetConnStatus(codigo, ...): el juego le pregunta por
// codigos de cuatro letras ('onln', 'conn', 'plug', 'slnk'...) y decide con eso si
// sigue conectado. Al entrar en Xbox Live con el servidor de EA de la LAN (ver
// docs/diario/red-y-privilegios.md), el juego inicia sesion en el servidor y luego
// dice "Has perdido tu conexion a Xbox Live" sin que ninguna llamada al sistema lo
// explique: la razon esta en el estado interno de NetConn, y esto lo apunta.
//
// Se apunta cada codigo la primera vez y cada vez que cambia su respuesta, con el
// estado de NetConn (la estructura a la que apunta 0x82911768):
//   +272 estado de la maquina (0..4)   +280 ultimo estado en texto ('+onl', '-svc')
//   +328 slnk (System Link)  +329 clave de slnk registrada  +330 secu (pasarela
//   segura)  +331 hay ISP  +332 en Xbox Live  +333 pasarela conectada

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "nfsmw_audio_nativo.h"

REXCVAR_DEFINE_BOOL(nfsmw_red_sonda, false, "NFSMW",
                    "Apunta en el log las preguntas del juego a NetConnStatus (la conexion de red de EA) "
                    "y su estado interno. Solo mide");

REX_EXTERN(__imp__sub_82819F48);
REX_EXTERN(__imp__sub_82546E18);
REX_EXTERN(__imp__sub_8284D030);
REX_EXTERN(__imp__sub_82547130);
REX_EXTERN(__imp__sub_8284C6D0);
REX_EXTERN(__imp__sub_8284CBA8);
REX_EXTERN(__imp__sub_8284C768);
REX_EXTERN(__imp__sub_8284C770);
REX_EXTERN(__imp__sub_8284C810);
REX_EXTERN(__imp__sub_8284C898);
REX_EXTERN(__imp__sub_8284C8D0);
REX_EXTERN(__imp__sub_8284C990);
REX_EXTERN(__imp__sub_82829AD0);

REXCVAR_DECLARE(bool, red_lan);

namespace {

using nfsmw::audio_nativo::Leer32;

constexpr uint32_t kPunteroNetConn = 0x82911768;

std::string Codigo(uint32_t v) {
  std::string s;
  for (int i = 3; i >= 0; --i) {
    char c = static_cast<char>((v >> (i * 8)) & 0xFF);
    s += (c >= 0x20 && c < 0x7F) ? c : '.';
  }
  return s;
}

std::string Estado(uint8_t* base) {
  uint32_t nc = Leer32(base, kPunteroNetConn);
  if (!nc) return "NetConn sin crear";
  auto b = [&](uint32_t off) { return static_cast<int>(*(base + nc + off)); };
  return fmt::format("estado={} ultimo='{}' slnk={} slnk_clave={} secu={} isp={} xbl={} pasarela={}",
                     Leer32(base, nc + 272), Codigo(Leer32(base, nc + 280)), b(328), b(329), b(330),
                     b(331), b(332), b(333));
}

// Las claves de texto del protocolo de salas que el juego lee o escribe, para no
// adivinarlas: las de la respuesta a 'ucre' (sub_82825B28, en orden) y las que
// pone 'gcre' (sub_82541ED0). Estan en .rdata, que no sale en el codigo generado.
void ApuntarClaves(uint8_t* base) {
  static const uint32_t kClaves[] = {0x820A22B0, 0x820A1530, 0x8202A97C, 0x8202A978, 0x8202A974,
                                     0x82094DD4, 0x82083268, 0x820A1528, 0x8202A970, 0x820A2380,
                                     0x8209A594, 0x820A18F8, 0x820A1754, 0x820A17E0,
                                     // las de la respuesta a 'gcre' (sub_82825C90, en orden)
                                     0x820A220C, 0x8209A594, 0x8202AA00, 0x8202A418, 0x820A1C8C,
                                     0x820A2FBC, 0x8202A9F8, 0x820A1C80, 0x820A17E0, 0x820A1FB8,
                                     0x820786F8, 0x820A1754, 0x820A18F8, 0x8202A9F0, 0x8202A9E8,
                                     // (sin la de 'WHEN': nada la referencia y no se
                                     // traduce a las otras ediciones)
                                     0x820A09C4, 0x820A260C,
                                     // la de la respuesta a 'usea' y la de la version (en P)
                                     0x820A1FB8, 0x820A233C};
  for (uint32_t d : kClaves) {
    const char* s = reinterpret_cast<const char*>(base + d);
    std::string t;
    for (int i = 0; i < 48 && s[i]; ++i) t += (s[i] >= 0x20 && s[i] < 0x7F) ? s[i] : '.';
    REXLOG_INFO("[red_sonda] clave {:08X} = '{}'", d, t);
  }
  // Los mensajes que el servidor manda sin que se los pidan ('+usr', '+who'...)
  // no salen como constantes en el codigo: se buscan como cadenas de 4 letras
  // en los datos del juego.
  std::string lista;
  for (uint32_t d = 0x82000000; d < 0x82100000; d += 4) {
    const uint8_t* p = base + d;
    if ((p[0] == '+' || p[0] == '@' || p[0] == '~') && p[1] >= 'a' && p[1] <= 'z' &&
        p[2] >= 'a' && p[2] <= 'z' && p[3] >= 'a' && p[3] <= 'z' && p[4] == 0) {
      lista += fmt::format(" {:08X}={}", d, std::string(reinterpret_cast<const char*>(p), 4));
    }
  }
  REXLOG_INFO("[red_sonda] mensajes en datos:{}", lista);

  // Las salas de la lista (sub_8282EF18 anade, sub_8282EFF8 actualiza) se llaman
  // por puntero desde una tabla {mensaje, funcion}: se busca la tabla en memoria y
  // se apunta lo que tiene alrededor, que es el codigo del mensaje.
  for (uint32_t d = 0x82000000; d < 0x82A00000; d += 4) {
    uint32_t v = Leer32(base, d);
    if (v == 0x8282EF18 || v == 0x8282EFF8) {
      std::string vecinos;
      for (int k = -6; k <= 6; ++k) {
        uint32_t w = Leer32(base, d + k * 4);
        vecinos += fmt::format(" {:08X}'{}'", w, Codigo(w));
      }
      REXLOG_INFO("[red_sonda] tabla {:08X} -> {:08X}:{}", d, v, vecinos);
    }
  }

  // La tabla entera {mensaje, funcion} de 0x828F2264, hacia los dos lados hasta
  // que deja de parecer una tabla.
  {
    std::string t;
    uint32_t ini = 0x828F2264 - 4;
    while (ini > 0x828F2000 && (Leer32(base, ini - 8) >> 24) == '+') ini -= 8;
    for (uint32_t d = ini; d < 0x828F2600; d += 8) {
      uint32_t codigo = Leer32(base, d), fn = Leer32(base, d + 4);
      if ((codigo >> 24) != '+' && (codigo >> 24) != '~' && (codigo >> 24) != '@') break;
      t += fmt::format(" {}={:08X}", Codigo(codigo), fn);
    }
    REXLOG_INFO("[red_sonda] tabla de mensajes:{}", t);
  }

  // Las cadenas de la zona de las claves del protocolo (NAME, IDENT, HOST...),
  // para sacar las de cada jugador de una partida (OPPO0, ADDR0...).
  auto cadenas = [&](uint32_t desde, uint32_t hasta) {
    std::string t;
    for (uint32_t d = desde; d < hasta;) {
      const char* s = reinterpret_cast<const char*>(base + d);
      size_t n = 0;
      while (n < 40 && s[n] >= 0x20 && s[n] < 0x7F) ++n;
      if (n >= 2 && s[n] == 0) {
        t += fmt::format(" {:08X}={}", d, std::string(s, n));
        d += static_cast<uint32_t>((n + 4) & ~3u);
      } else {
        d += 4;
      }
    }
    REXLOG_INFO("[red_sonda] cadenas {:08X}-{:08X}:{}", desde, hasta, t);
  };
  cadenas(0x820A1A00, 0x820A1C00);  // la del nombre por jugador (0x820A1B30)
  cadenas(0x820784B0, 0x82078510);  // las que se leen de un +msg (sub_822787A0)
  cadenas(0x820A1520, 0x820A1540);  // la de +msg (0x820A152C)
  cadenas(0x8202B080, 0x8202B200);  // las de +who
  cadenas(0x82083240, 0x820832A0);
  // Donde se guardan los descodificadores de direcciones de otros jugadores
  // (sub_82827868, sub_82827670): tablas de punteros en los datos.
  for (uint32_t d = 0x82000000; d < 0x82A00000; d += 4) {
    uint32_t v = Leer32(base, d);
    if (v == 0x82827868 || v == 0x82827670) {
      std::string vecinos;
      for (int k = -8; k <= 8; ++k) vecinos += fmt::format(" {:08X}", Leer32(base, d + k * 4));
      REXLOG_INFO("[red_sonda] puntero {:08X} en {:08X}:{}", v, d, vecinos);
    }
  }
  cadenas(0x8202A3C0, 0x8202AA40);
  cadenas(0x820A0980, 0x820A0A40);
  cadenas(0x820A1500, 0x820A1A00);
  cadenas(0x820A1C00, 0x820A2400);
  cadenas(0x820A2F80, 0x820A3000);
}

}  // namespace

REX_HOOK_RAW(sub_82819F48) {
  if (!REXCVAR_GET(nfsmw_red_sonda)) {
    __imp__sub_82819F48(ctx, base);
    return;
  }
  static std::once_flag claves;
  std::call_once(claves, [&] { ApuntarClaves(base); });
  const uint32_t codigo = ctx.r3.u32;
  __imp__sub_82819F48(ctx, base);
  const uint32_t resultado = ctx.r3.u32;

  static std::mutex m;
  static std::unordered_map<uint32_t, uint32_t> ultimos;
  bool cambia;
  {
    std::lock_guard<std::mutex> l(m);
    auto it = ultimos.find(codigo);
    cambia = it == ultimos.end() || it->second != resultado;
    ultimos[codigo] = resultado;
  }
  if (cambia) {
    REXLOG_INFO("[red_sonda] NetConnStatus('{}') -> {:08X} '{}' | {}", Codigo(codigo), resultado,
                Codigo(resultado), Estado(base));
  }
}

// La conexion entre jugadores (ConnApi de EA, puerto 3658): quien la crea
// (sub_82546E18 -> sub_8284D030), quien la pone en marcha tras gcre/gjoi
// (sub_82547130 -> sub_8284C6D0) y sus ajustes (sub_8284CBA8, codigo de 4
// letras en r4). Solo se apunta: entrada (r3..r7) y lo que devuelve.
#define NFSMW_RED_SONDA_LLAMADA(direccion, nombre)                                             \
  REX_HOOK_RAW(sub_##direccion) {                                                              \
    if (!REXCVAR_GET(nfsmw_red_sonda)) {                                                       \
      __imp__sub_##direccion(ctx, base);                                                       \
      return;                                                                                  \
    }                                                                                          \
    const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32, a6 = ctx.r6.u32,          \
                   a7 = ctx.r7.u32;                                                            \
    __imp__sub_##direccion(ctx, base);                                                         \
    REXLOG_INFO("[red_sonda] {} (r3={:08X} r4={:08X} '{}' r5={:08X} r6={:08X} r7={:08X}) -> {:08X}", \
                nombre, a3, a4, Codigo(a4), a5, a6, a7, ctx.r3.u32);                            \
  }

NFSMW_RED_SONDA_LLAMADA(82546E18, "crear conexion de jugadores")
NFSMW_RED_SONDA_LLAMADA(8284D030, "ConnApiCreate")
NFSMW_RED_SONDA_LLAMADA(82547130, "poner jugadores en marcha")
NFSMW_RED_SONDA_LLAMADA(8284CBA8, "ConnApiControl")
NFSMW_RED_SONDA_LLAMADA(8284C770, "ConnApi 8284C770")
NFSMW_RED_SONDA_LLAMADA(8284C810, "ConnApi 8284C810")
NFSMW_RED_SONDA_LLAMADA(8284C898, "ConnApi 8284C898")
NFSMW_RED_SONDA_LLAMADA(8284C8D0, "ConnApi 8284C8D0")
NFSMW_RED_SONDA_LLAMADA(8284C990, "ConnApi 8284C990")

// ConnApiOnline: antes de llamarla, los dos lados de la comparacion que la hace
// fallar (sub_82819C78): el XUID que busca la conexion (objeto de r3+56, +128) y
// la tabla de 4 usuarios de NetConn (0x82911768 -> +72, 32 bytes cada uno:
// nombre de 16 y XUID de 8 en +16).
REX_HOOK_RAW(sub_8284C6D0) {
  if (REXCVAR_GET(nfsmw_red_sonda)) {
    uint32_t conn = ctx.r3.u32;
    uint32_t mangle = conn ? Leer32(base, conn + 56) : 0;
    auto x64 = [&](uint32_t d) {
      return fmt::format("{:08X}{:08X}", Leer32(base, d), Leer32(base, d + 4));
    };
    std::string t = mangle ? fmt::format("busca XUID {} (mangle {:08X})", x64(mangle + 128), mangle)
                           : std::string("sin mangle");
    uint32_t nc = Leer32(base, kPunteroNetConn);
    for (int i = 0; nc && i < 4; ++i) {
      uint32_t e = nc + 72 + i * 32;
      std::string nombre;
      for (int k = 0; k < 16; ++k) {
        char c = static_cast<char>(*(base + e + k));
        if (!c) break;
        nombre += (c >= 0x20 && c < 0x7F) ? c : '.';
      }
      t += fmt::format(" | usuario{} '{}' XUID {}", i, nombre, x64(e + 16));
    }
    REXLOG_INFO("[red_sonda] antes de ConnApiOnline: {}", t);
  }
  const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32, a6 = ctx.r6.u32,
                 a7 = ctx.r7.u32;
  __imp__sub_8284C6D0(ctx, base);
  if (REXCVAR_GET(nfsmw_red_sonda)) {
    REXLOG_INFO("[red_sonda] ConnApiOnline (r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X}) -> {:08X}",
                a3, a4, a5, a6, a7, ctx.r3.u32);
  }
}

// ConnApiConnect(ref, lista, cuantos, id de partida, ...): la lista de clientes
// que monta el juego. Se vuelca entera (sin saber aun el tamano de cada uno) y
// se marca donde aparecen IPs de la LAN 10.0.0.x, para ver de donde saca el
// enlace de juego (puerto 3658) la direccion del otro.
REX_HOOK_RAW(sub_8284C768) {
  const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32, a6 = ctx.r6.u32,
                 a7 = ctx.r7.u32;
  if (REXCVAR_GET(nfsmw_red_sonda) && a4) {
    std::string ips;
    for (uint32_t o = 0; o < 0x400; o += 4) {
      uint32_t v = Leer32(base, a4 + o);
      if ((v & 0xFFFFFF00u) == 0x0A000000u) ips += fmt::format(" +{:03X}={:08X}", o, v);
    }
    REXLOG_INFO("[red_sonda] ConnApiConnect lista {:08X} x{}: IPs en{}", a4, a5, ips);
    for (uint32_t o = 0; o < 0x300; o += 32) {
      std::string linea;
      for (uint32_t k = 0; k < 32; k += 4) linea += fmt::format("{:08X} ", Leer32(base, a4 + o + k));
      REXLOG_INFO("[red_sonda]   +{:03X}: {}", o, linea);
    }
  }
  __imp__sub_8284C768(ctx, base);
  if (REXCVAR_GET(nfsmw_red_sonda)) {
    REXLOG_INFO("[red_sonda] ConnApiConnect (r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X}) -> {:08X}",
                a3, a4, a5, a6, a7, ctx.r3.u32);
  }
}

// La tabla 'xmap' de DirtySock (SocketControl 'xmap', sub_8282AB88): al quedar
// en linea, NetConn (sub_82819600) pone una sola entrada {direccion 0, mascara 0,
// IP del servicio de EA} y sub_82829AD0 la aplica en SocketSendto y
// SocketRecvfrom: con mascara 0 casa con todo, asi que el enlace de juego del
// ConnApi (UDP 3658) se manda al servidor (en Xbox Live, la pasarela segura) y
// lo que llega parece venir de el. Pero tambien es como se llega al servidor:
// NetConn pone 'xdns' = 127.0.0.1, el nombre del servidor se resuelve a esa IP
// y la tabla la cambia por la del servicio. En la LAN el servidor es el PC y
// nadie escucha ahi el UDP: el que se une mandaba su INIT de CommUDP al PC y a
// los 10 s se iba con "No se ha podido conectar con el otro jugador". Con
// red_lan solo se traduce 127.x.x.x (el servidor); las IPs de los otros
// jugadores se dejan como estan (lo mismo que hace la funcion sin tabla).
REX_HOOK_RAW(sub_82829AD0) {
  if (REXCVAR_GET(red_lan) && ctx.r4.u32 && *(base + ctx.r4.u32 + 4) != 127) {
    ctx.r3.u64 = ctx.r4.u64;
    return;
  }
  __imp__sub_82829AD0(ctx, base);
}
