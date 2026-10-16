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
                                     0x820A09CC, 0x820A09C4, 0x820A260C,
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
