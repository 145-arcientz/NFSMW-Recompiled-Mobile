/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <cstring>

#if REX_PLATFORM_MAC
#include <sys/select.h>
#endif

#include <rex/chrono/clock.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#if REX_PLATFORM_WIN32
// NOTE: must be included last as it expects windows.h to already be included.
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr
#include <winsock2.h>                    // NOLINT(build/include_order)
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <string>

#include <rex/cvar.h>

// PARCHE LOCAL (red) - ver docs/diario/red-y-privilegios.md
//
// red_lan: que el juego vea una red de verdad. Sin esto, XNetGetEthernetLinkStatus
// dice "sin cable" y XNetGetTitleXnAddr da 127.0.0.1, y el juego no sale de casa.
// red_dns: responder esa IP a cualquier nombre que pida el juego (los servidores de
// EA), para poner en su lugar un servidor propio en la LAN.
// red_registro: cada llamada de red al log, con "[red]" delante.
REXCVAR_DEFINE_BOOL(red_lan, false, "Network",
                    "Report a working network link and the device's LAN IP to the game");
REXCVAR_DEFINE_STRING(red_ip, "", "Network",
                      "LAN IP to report to the game (empty = detect it)");
REXCVAR_DEFINE_STRING(red_dns, "", "Network",
                      "Answer every DNS lookup of the game with this IPv4 (empty = fail them)");
REXCVAR_DEFINE_BOOL(red_registro, false, "Network", "Log every network call of the game");
REXCVAR_DEFINE_BOOL(rastro_sistema, false, "Kernel",
                    "Log the first calls the game makes to each system export (diagnosis)");

#include <mutex>
#include <ctime>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <vector>
#include <rex/system/thread_state.h>

namespace rex {
bool& RastroSistemaActivo() {
  return REXCVAR_GET(rastro_sistema);
}

// El ruido del kernel (esperas, cerrojos, audio, video) va con las 3 primeras
// llamadas y luego una de cada 2000. Lo demas (Xam, XNotify, red...) sale
// siempre, hasta 8 llamadas por segundo de cada una: asi se ve lo ultimo que
// consulto el juego antes de decidir algo, aunque lo lleve llamando un rato.
static bool RastroRuido(const char* n) {
  static const char* kRuido[] = {"Ke", "Kf", "Rtl", "Nt", "Ob", "Mm", "Ex", "XMA", "XAudio",
                                 "Vd", "XamInputGetState", "XNotifyGetNext"};
  for (const char* p : kRuido) {
    if (std::strncmp(n, p, std::strlen(p)) == 0) return true;
  }
  return false;
}

void RastroSistema(const char* nombre, uint32_t r3, uint32_t r4, uint32_t r5) {
  struct Cuenta {
    uint64_t total = 0;
    uint64_t segundo = 0;
    uint32_t en_segundo = 0;
  };
  static std::mutex m;
  static std::unordered_map<const char*, Cuenta> cuentas;
  const char* corto = nombre + 7;  // sin "__imp__"
  uint64_t ahora = static_cast<uint64_t>(std::time(nullptr));
  uint64_t n;
  bool sale;
  {
    std::lock_guard<std::mutex> l(m);
    Cuenta& c = cuentas[nombre];
    n = ++c.total;
    if (RastroRuido(corto)) {
      sale = n <= 3 || n % 2000 == 0;
    } else {
      if (c.segundo != ahora) {
        c.segundo = ahora;
        c.en_segundo = 0;
      }
      sale = ++c.en_segundo <= 8;
    }
  }
  if (sale) {
    REXKRNL_INFO("[rastro] {} #{} (r3={:08X} r4={:08X} r5={:08X})", corto, n, r3, r4, r5);
  }
}
}  // namespace rex

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

// https://github.com/G91/TitanOffLine/blob/1e692d9bb9dfac386d08045ccdadf4ae3227bb5e/xkelib/xam/xamNet.h
enum {
  XNCALLER_INVALID = 0x0,
  XNCALLER_TITLE = 0x1,
  XNCALLER_SYSAPP = 0x2,
  XNCALLER_XBDM = 0x3,
  XNCALLER_TEST = 0x4,
  NUM_XNCALLER_TYPES = 0x4,
};

// https://github.com/pmrowla/hl2sdk-csgo/blob/master/common/xbox/xboxstubs.h
typedef struct {
  // FYI: IN_ADDR should be in network-byte order.
  in_addr ina;                    // IP address (zero if not static/DHCP)
  in_addr inaOnline;              // Online IP address (zero if not online)
  rex::be<uint16_t> wPortOnline;  // Online port
  uint8_t abEnet[6];              // Ethernet MAC address
  uint8_t abOnline[20];           // Online identification
} XNADDR;

typedef struct {
  rex::be<int32_t> status;
  rex::be<uint32_t> cina;
  in_addr aina[8];
} XNDNS;

typedef struct {
  uint8_t flags;
  uint8_t reserved;
  rex::be<uint16_t> probes_xmit;
  rex::be<uint16_t> probes_recv;
  rex::be<uint16_t> data_len;
  rex::be<uint32_t> data_ptr;
  rex::be<uint16_t> rtt_min_in_msecs;
  rex::be<uint16_t> rtt_med_in_msecs;
  rex::be<uint32_t> up_bits_per_sec;
  rex::be<uint32_t> down_bits_per_sec;
} XNQOSINFO;

typedef struct {
  rex::be<uint32_t> count;
  rex::be<uint32_t> count_pending;
  XNQOSINFO info[1];
} XNQOS;

struct Xsockaddr_t {
  rex::be<uint16_t> sa_family;
  char sa_data[14];
};

struct X_WSADATA {
  rex::be<uint16_t> version;
  rex::be<uint16_t> version_high;
  char description[256 + 1];
  char system_status[128 + 1];
  rex::be<uint16_t> max_sockets;
  rex::be<uint16_t> max_udpdg;
  rex::be<uint32_t> vendor_info_ptr;
};

struct XWSABUF {
  rex::be<uint32_t> len;
  rex::be<uint32_t> buf_ptr;
};

struct XWSAOVERLAPPED {
  rex::be<uint32_t> internal;
  rex::be<uint32_t> internal_high;
  union {
    struct {
      rex::be<uint32_t> low;
      rex::be<uint32_t> high;
    } offset;  // must be named to avoid GCC error
    rex::be<uint32_t> pointer;
  };
  rex::be<uint32_t> event_handle;
};

void LoadSockaddr(const uint8_t* ptr, sockaddr* out_addr) {
  out_addr->sa_family = memory::load_and_swap<uint16_t>(ptr + 0);
  switch (out_addr->sa_family) {
    case AF_INET: {
      auto in_addr = reinterpret_cast<sockaddr_in*>(out_addr);
      in_addr->sin_port = memory::load_and_swap<uint16_t>(ptr + 2);
      // Maybe? Depends on type.
      in_addr->sin_addr.s_addr = *(uint32_t*)(ptr + 4);
      break;
    }
    default:
      assert_unhandled_case(out_addr->sa_family);
      break;
  }
}

void StoreSockaddr(const sockaddr& addr, uint8_t* ptr) {
  switch (addr.sa_family) {
    case AF_UNSPEC:
      std::memset(ptr, 0, sizeof(addr));
      break;
    case AF_INET: {
      auto& in_addr = reinterpret_cast<const sockaddr_in&>(addr);
      memory::store_and_swap<uint16_t>(ptr + 0, in_addr.sin_family);
      memory::store_and_swap<uint16_t>(ptr + 2, in_addr.sin_port);
      // Maybe? Depends on type.
      memory::store_and_swap<uint32_t>(ptr + 4, in_addr.sin_addr.s_addr);
      break;
    }
    default:
      assert_unhandled_case(addr.sa_family);
      break;
  }
}

// PARCHE LOCAL (red): apoyo para el registro y para Android.

static std::atomic<uint32_t> g_red_lineas{0};
// Tope de lineas: un juego sondeando un socket cada fotograma llenaria el log.
#define RED_LOG(...)                                                              \
  do {                                                                            \
    if (REXCVAR_GET(red_registro) && g_red_lineas.fetch_add(1) < 20000) {          \
      REXKRNL_INFO("[red] " __VA_ARGS__);                                         \
    }                                                                             \
  } while (0)

static std::string RedIp(uint32_t ip_red) {  // en orden de red
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&ip_red);
  return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." +
         std::to_string(b[3]);
}

// Los primeros bytes de un paquete, en hexadecimal y en texto: el protocolo de
// salas de EA de la epoca era de texto con cabecera binaria.
static std::string RedVolcado(const uint8_t* p, int n) {
  if (!p || n <= 0) return "";
  int m = std::min(n, 96);
  static const char* kHex = "0123456789ABCDEF";
  std::string hex, txt;
  for (int i = 0; i < m; ++i) {
    hex += kHex[p[i] >> 4];
    hex += kHex[p[i] & 15];
    txt += (p[i] >= 0x20 && p[i] < 0x7F) ? static_cast<char>(p[i]) : '.';
  }
  return hex + " |" + txt + "|";
}

// Quien llama: lr y la cadena de marcos del codigo del 360 (cada marco guarda
// el anterior en 0(sp) y el lr de quien lo llamo en anterior - 8).
static std::string RedPila() {
  auto* ts = rex::runtime::ThreadState::Get();
  auto* c = ts ? ts->context() : nullptr;
  if (!c) return "";
  std::string pila = fmt::format("lr={:08X}", static_cast<uint32_t>(c->lr));
  // Solo dentro de la pila del hilo (PCR: +0x70 tope, +0x74 fondo); el ultimo
  // marco apunta al tope y leer ahi es salirse de la memoria.
  auto* hilo = XThread::GetCurrentThread();
  uint32_t pcr = hilo ? hilo->pcr_ptr() : 0;
  if (!pcr) return pila;
  auto* m = REX_KERNEL_MEMORY();
  uint32_t tope = memory::load_and_swap<uint32_t>(m->TranslateVirtual(pcr + 0x70));
  uint32_t fondo = memory::load_and_swap<uint32_t>(m->TranslateVirtual(pcr + 0x74));
  uint32_t sp = c->r1.u32;
  if (sp < fondo || sp + 4 > tope) return pila;
  for (int i = 0; i < 8; ++i) {
    uint32_t anterior =
        memory::load_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual(sp));
    if (anterior <= sp || anterior + 4 > tope) break;
    pila += fmt::format(" <- {:08X}", memory::load_and_swap<uint32_t>(
                                          REX_KERNEL_MEMORY()->TranslateVirtual(anterior - 8)));
    sp = anterior;
  }
  return pila;
}

// errno de POSIX -> codigo de Winsock, que es lo que el juego lee con
// WSAGetLastError. El original ponia 0 o nada, y un juego no puede distinguir
// "todavia no" (WSAEWOULDBLOCK) de un fallo.
static uint32_t RedErrorWsa(int e) {
#if REX_PLATFORM_WIN32
  return static_cast<uint32_t>(e);
#else
  switch (e) {
    case 0: return 0;
    case EWOULDBLOCK: return 10035;  // WSAEWOULDBLOCK (EAGAIN es el mismo en Linux)
    case EINPROGRESS: return 10035;  // Winsock da WOULDBLOCK al conectar sin bloquear
    case EALREADY: return 10037;
    case ENOTSOCK: return 10038;
    case EDESTADDRREQ: return 10039;
    case EMSGSIZE: return 10040;
    case ENOPROTOOPT: return 10042;
    case EAFNOSUPPORT: return 10047;
    case EADDRINUSE: return 10048;
    case EADDRNOTAVAIL: return 10049;
    case ENETDOWN: return 10050;
    case ENETUNREACH: return 10051;
    case ECONNABORTED: return 10053;
    case ECONNRESET: return 10054;
    case EPIPE: return 10054;
    case ENOBUFS: return 10055;
    case EISCONN: return 10056;
    case ENOTCONN: return 10057;
    case ETIMEDOUT: return 10060;
    case ECONNREFUSED: return 10061;
    case EHOSTUNREACH: return 10065;
    case EINVAL: return 10022;
    case EFAULT: return 10014;
    case EBADF: return 10009;
    default: return 10022;
  }
#endif
}

static uint32_t RedFijarError() {
#if REX_PLATFORM_WIN32
  uint32_t e = WSAGetLastError();
#else
  uint32_t e = RedErrorWsa(errno);
#endif
  XThread::SetLastError(e);
  return e;
}

// La IP de este aparato en la LAN, en orden de red. Un socket UDP "conectado" a
// una direccion de fuera no manda nada, pero deja que el sistema elija la
// interfaz y getsockname dice cual.
static uint32_t RedIpLocal() {
  std::string fija = REXCVAR_GET(red_ip);
  if (!fija.empty()) {
    return inet_addr(fija.c_str());
  }
  static uint32_t cache = 0;
  if (cache) return cache;
#if !REX_PLATFORM_WIN32
  int s = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (s >= 0) {
    sockaddr_in fuera = {};
    fuera.sin_family = AF_INET;
    fuera.sin_port = htons(53);
    fuera.sin_addr.s_addr = inet_addr("8.8.8.8");
    if (::connect(s, reinterpret_cast<sockaddr*>(&fuera), sizeof(fuera)) == 0) {
      sockaddr_in yo = {};
      socklen_t n = sizeof(yo);
      if (::getsockname(s, reinterpret_cast<sockaddr*>(&yo), &n) == 0) {
        cache = yo.sin_addr.s_addr;
      }
    }
    ::close(s);
  }
#endif
  if (!cache) cache = htonl(INADDR_LOOPBACK);
  return cache;
}

// Opciones de socket de Winsock (las que usa el 360) -> las de Linux. Los valores
// llegan en big-endian desde la memoria del juego. Devuelve false si no hay
// equivalente: entonces se finge exito y se apunta en el log.
#if !REX_PLATFORM_WIN32
static bool RedOpcion(uint32_t level, uint32_t optname, int* n_level, int* n_optname) {
  if (level == 0xFFFF) {
    *n_level = SOL_SOCKET;
    switch (optname) {
      case 0x0004: *n_optname = SO_REUSEADDR; return true;
      case 0x0008: *n_optname = SO_KEEPALIVE; return true;
      case 0x0010: *n_optname = SO_DONTROUTE; return true;
      case 0x0020: *n_optname = SO_BROADCAST; return true;
      case 0x0080: *n_optname = SO_LINGER; return true;
      case 0x0100: *n_optname = SO_OOBINLINE; return true;
      case 0x1001: *n_optname = SO_SNDBUF; return true;
      case 0x1002: *n_optname = SO_RCVBUF; return true;
      case 0x1005: *n_optname = SO_SNDTIMEO; return true;
      case 0x1006: *n_optname = SO_RCVTIMEO; return true;
      case 0x1007: *n_optname = SO_ERROR; return true;
      case 0x1008: *n_optname = SO_TYPE; return true;
      default: return false;
    }
  }
  if (level == 6 && optname == 1) {  // IPPROTO_TCP, TCP_NODELAY
    *n_level = IPPROTO_TCP;
    *n_optname = TCP_NODELAY;
    return true;
  }
  return false;
}
#endif

// https://github.com/joolswills/mameox/blob/master/MAMEoX/Sources/xbox_Network.cpp#L136
struct XNetStartupParams {
  uint8_t cfgSizeOfStruct;
  uint8_t cfgFlags;
  uint8_t cfgSockMaxDgramSockets;
  uint8_t cfgSockMaxStreamSockets;
  uint8_t cfgSockDefaultRecvBufsizeInK;
  uint8_t cfgSockDefaultSendBufsizeInK;
  uint8_t cfgKeyRegMax;
  uint8_t cfgSecRegMax;
  uint8_t cfgQosDataLimitDiv4;
  uint8_t cfgQosProbeTimeoutInSeconds;
  uint8_t cfgQosProbeRetries;
  uint8_t cfgQosSrvMaxSimultaneousResponses;
  uint8_t cfgQosPairWaitTimeInSeconds;
};

XNetStartupParams xnet_startup_params = {};

u32 NetDll_XNetStartup_entry(u32 caller, ppc_ptr_t<XNetStartupParams> params) {
  if (params) {
    assert_true(params->cfgSizeOfStruct == sizeof(XNetStartupParams));
    std::memcpy(&xnet_startup_params, params, sizeof(XNetStartupParams));
  }

  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");

  /*
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  RED_LOG("XNetStartup (red_lan={}, ip local {}, red_dns='{}')", REXCVAR_GET(red_lan),
          RedIp(RedIpLocal()), REXCVAR_GET(red_dns));
  return 0;
}

u32 NetDll_XNetCleanup_entry(u32 caller, mapped_void params) {
  RED_LOG("XNetCleanup");
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  // auto xnet = xam->xnet();
  // xam->set_xnet(nullptr);

  // TODO: Shut down and delete.
  // delete xnet;

  return 0;
}

u32 NetDll_XNetGetOpt_entry(u32 one, u32 option_id, mapped_void buffer_ptr,
                            mapped_u32 buffer_size) {
  assert_true(one == 1);
  switch (option_id) {
    case 1:
      if (*buffer_size < sizeof(XNetStartupParams)) {
        *buffer_size = sizeof(XNetStartupParams);
        return 0x2738;  // WSAEMSGSIZE
      }
      std::memcpy(buffer_ptr, &xnet_startup_params, sizeof(XNetStartupParams));
      return 0;
    default:
      REXKRNL_ERROR("NetDll_XNetGetOpt: option {} unimplemented", option_id);
      return 0x2726;  // WSAEINVAL
  }
}

u32 NetDll_XNetRandom_entry(u32 caller, mapped_void buffer_ptr, u32 length) {
  // For now, constant values.
  // This makes replicating things easier.
  std::memset(buffer_ptr, 0xBB, length);

  return 0;
}

u32 NetDll_WSAStartup_entry(u32 caller, u16 version, ppc_ptr_t<X_WSADATA> data_ptr) {
// TODO(benvanik): abstraction layer needed.
#if REX_PLATFORM_WIN32
  WSADATA wsaData;
  ZeroMemory(&wsaData, sizeof(WSADATA));
  int ret = WSAStartup(version, &wsaData);

  auto data_out = REX_KERNEL_MEMORY()->TranslateVirtual(data_ptr.guest_address());

  if (data_ptr) {
    data_ptr->version = wsaData.wVersion;
    data_ptr->version_high = wsaData.wHighVersion;
    std::memcpy(&data_ptr->description, wsaData.szDescription, 0x100);
    std::memcpy(&data_ptr->system_status, wsaData.szSystemStatus, 0x80);
    data_ptr->max_sockets = wsaData.iMaxSockets;
    data_ptr->max_udpdg = wsaData.iMaxUdpDg;

    // Some games (5841099F) want this value round-tripped - they'll compare if
    // it changes and bugcheck if it does.
    uint32_t vendor_ptr = memory::load_and_swap<uint32_t>(data_out + 0x190);
    memory::store_and_swap<uint32_t>(data_out + 0x190, vendor_ptr);
  }
#else
  int ret = 0;
  if (data_ptr) {
    // Guess these values!
    data_ptr->version = version;
    data_ptr->description[0] = '\0';
    data_ptr->system_status[0] = '\0';
    data_ptr->max_sockets = 100;
    data_ptr->max_udpdg = 1024;
  }
#endif

  // DEBUG
  /*
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return ret;
}

u32 NetDll_WSACleanup_entry(u32 caller) {
  // This does nothing. Xenia needs WSA running.
  return 0;
}

u32 NetDll_WSAGetLastError_entry() {
  return XThread::GetLastError();
}

// PARCHE LOCAL (red): un datagrama recibido, repartido en los bufers del juego.
// Devuelve los bytes copiados.
static uint32_t RedRepartir(const uint8_t* datos, uint32_t n, uint32_t buffers_ptr,
                            uint32_t buffer_count) {
  uint32_t hecho = 0;
  for (uint32_t i = 0; i < buffer_count && hecho < n; ++i) {
    auto* b = REX_KERNEL_MEMORY()->TranslateVirtual<XWSABUF*>(buffers_ptr + i * sizeof(XWSABUF));
    uint32_t cabe = std::min<uint32_t>(b->len, n - hecho);
    std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual(b->buf_ptr), datos + hecho, cabe);
    hecho += cabe;
  }
  return hecho;
}

static void RedEscribirOrigen(uint32_t from_ptr, uint32_t fromlen_ptr, const N_XSOCKADDR_IN& de) {
  if (from_ptr) {
    auto* f = REX_KERNEL_MEMORY()->TranslateVirtual<XSOCKADDR_IN*>(from_ptr);
    f->sin_family = de.sin_family;
    f->sin_port = de.sin_port;
    f->sin_addr = de.sin_addr;
    std::memset(f->x_sin_zero, 0, sizeof(f->x_sin_zero));
  }
  if (fromlen_ptr) {
    memory::store_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual(fromlen_ptr), 16);
  }
}

// PARCHE LOCAL (red): WSARecvFrom solapada. El enlace de juego de EA (CommUDP del
// ConnApi, puerto 3658) recibe en un hilo suyo con lecturas solapadas: pide una,
// espera al evento del OVERLAPPED y recoge con WSAGetOverlappedResult. El
// original devolvia -1 sin mas y ese hilo dejaba de leer: el que se une nunca
// oia al anfitrion y a los 3 s se iba con "No se ha podido conectar con el otro
// jugador". Aqui un hilo nativo espera el datagrama (el socket es no bloqueante),
// lo copia, rellena el OVERLAPPED (Internal = 0, InternalHigh = bytes) y avisa al
// evento. Si se cierra el socket, acaba con WSA_OPERATION_ABORTED.
u32 NetDll_WSARecvFrom_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers_ptr,
                             u32 buffer_count, mapped_u32 num_bytes_recv, mapped_u32 flags_ptr,
                             ppc_ptr_t<XSOCKADDR_IN> from_addr, mapped_u32 fromlen_ptr,
                             ppc_ptr_t<XWSAOVERLAPPED> overlapped_ptr,
                             mapped_void completion_routine_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(0x2736);  // WSAENOTSOCK
    return -1;
  }
  const uint32_t bufs = buffers_ptr.guest_address();
  const uint32_t de_ptr = from_addr.guest_address();
  // La firma de Xenia no traia lpFromlen: el OVERLAPPED (9.o argumento, en la
  // pila) se leia de r10, que es el puntero al largo de "from".
  const uint32_t largo_ptr = fromlen_ptr.guest_address();
  if (flags_ptr) *flags_ptr = 0;

  uint32_t capacidad = 0;
  for (uint32_t i = 0; i < buffer_count; ++i) capacidad += buffers_ptr[i].len;
  std::vector<uint8_t> tmp(std::max<uint32_t>(capacidad, 1));

  // Si ya hay algo, se entrega en el acto (tambien con OVERLAPPED: Winsock lo
  // permite y deja el evento senalado).
  N_XSOCKADDR_IN de;
  uint32_t de_largo = sizeof(sockaddr_in);
  int n = socket->RecvFrom(tmp.data(), capacidad, 0, &de, &de_largo);
  if (n >= 0) {
    uint32_t hecho = RedRepartir(tmp.data(), n, bufs, buffer_count);
    RedEscribirOrigen(de_ptr, largo_ptr, de);
    if (num_bytes_recv) *num_bytes_recv = hecho;
    if (overlapped_ptr) {
      overlapped_ptr->internal = 0;
      overlapped_ptr->internal_high = hecho;
      auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(overlapped_ptr->event_handle);
      if (ev) ev->Set(0, false);
    }
    RED_LOG("WSARecvFrom({:08X}) <- {}:{} {} bytes {}", socket_handle, RedIp(htonl(de.sin_addr)),
            static_cast<uint16_t>(de.sin_port), hecho, RedVolcado(tmp.data(), n));
    return 0;
  }
  uint32_t e = RedFijarError();
  if (e != 10035 || !overlapped_ptr) {
    RED_LOG("WSARecvFrom({:08X}) -> falla {}", socket_handle, e);
    return -1;
  }

  // Sin datos y solapada: queda pendiente.
  const uint32_t ov = overlapped_ptr.guest_address();
  overlapped_ptr->internal = 0x103;  // STATUS_PENDING
  overlapped_ptr->internal_high = 0;
  XSocket* original = socket.get();
  std::thread([socket_handle, original, bufs, buffer_count, de_ptr, largo_ptr, ov, capacidad]() {
    std::vector<uint8_t> datos(std::max<uint32_t>(capacidad, 1));
    auto* o = REX_KERNEL_MEMORY()->TranslateVirtual<XWSAOVERLAPPED*>(ov);
    uint32_t estado = 995;  // WSA_OPERATION_ABORTED
    uint32_t hecho = 0;
    for (;;) {
      auto s = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
      if (!s || s.get() != original) break;  // cerrado
      N_XSOCKADDR_IN de;
      uint32_t de_largo = sizeof(sockaddr_in);
      int n = s->RecvFrom(datos.data(), capacidad, 0, &de, &de_largo);
      if (n >= 0) {
        hecho = RedRepartir(datos.data(), n, bufs, buffer_count);
        RedEscribirOrigen(de_ptr, largo_ptr, de);
        estado = 0;
        RED_LOG("WSARecvFrom({:08X}, solapada) <- {}:{} {} bytes {}", socket_handle,
                RedIp(htonl(de.sin_addr)), static_cast<uint16_t>(de.sin_port), hecho,
                RedVolcado(datos.data(), n));
        break;
      }
      int err = errno;
      if (err != EAGAIN && err != EWOULDBLOCK && err != EINTR) {
        estado = RedErrorWsa(err);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    o->internal_high = hecho;
    o->internal = estado;
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(o->event_handle);
    if (ev) ev->Set(0, false);
  }).detach();
  XThread::SetLastError(997);  // WSA_IO_PENDING
  return -1;
}

// PARCHE LOCAL (red): recoge una WSARecvFrom solapada (ver arriba).
u32 NetDll_WSAGetOverlappedResult_entry(u32 caller, u32 socket_handle,
                                        ppc_ptr_t<XWSAOVERLAPPED> overlapped_ptr,
                                        mapped_u32 bytes_ptr, u32 wait, mapped_u32 flags_ptr) {
  if (!overlapped_ptr) {
    XThread::SetLastError(10014);  // WSAEFAULT
    return 0;
  }
  if (overlapped_ptr->internal == 0x103) {
    if (!wait) {
      XThread::SetLastError(996);  // WSA_IO_INCOMPLETE
      return 0;
    }
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(overlapped_ptr->event_handle);
    while (overlapped_ptr->internal == 0x103) {
      if (ev) {
        uint64_t plazo = static_cast<uint64_t>(-10000LL * 50);  // 50 ms, relativo
        ev->Wait(0, 0, 1, &plazo);
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
  }
  if (bytes_ptr) *bytes_ptr = overlapped_ptr->internal_high;
  if (flags_ptr) *flags_ptr = 0;
  uint32_t estado = overlapped_ptr->internal;
  if (estado != 0) {
    XThread::SetLastError(estado);
    return 0;
  }
  return 1;
}

// If the socket is a VDP socket, buffer 0 is the game data length, and buffer 1
// is the unencrypted game data.
u32 NetDll_WSASendTo_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers,
                           u32 num_buffers, mapped_u32 num_bytes_sent, u32 flags,
                           ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len,
                           ppc_ptr_t<XWSAOVERLAPPED> overlapped, mapped_void completion_routine) {
  assert(!overlapped);
  assert(!completion_routine);

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  // Our sockets implementation doesn't support multiple buffers, so we need
  // to combine the buffers the game has given us!
  std::vector<uint8_t> combined_buffer_mem;
  uint32_t combined_buffer_size = 0;
  uint32_t combined_buffer_offset = 0;
  for (uint32_t i = 0; i < num_buffers; i++) {
    combined_buffer_size += buffers[i].len;
    combined_buffer_mem.resize(combined_buffer_size);
    uint8_t* combined_buffer = combined_buffer_mem.data();

    std::memcpy(combined_buffer + combined_buffer_offset,
                REX_KERNEL_MEMORY()->TranslateVirtual(buffers[i].buf_ptr), buffers[i].len);
    combined_buffer_offset += buffers[i].len;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  socket->SendTo(combined_buffer_mem.data(), combined_buffer_size, flags, &native_to, to_len);

  // TODO: Instantly complete overlapped

  return 0;
}

u32 NetDll_WSAWaitForMultipleEvents_entry(u32 num_events, mapped_u32 events, u32 wait_all,
                                          u32 timeout, u32 alertable) {
  if (num_events > 64) {
    XThread::SetLastError(87);  // ERROR_INVALID_PARAMETER
    return ~0u;
  }

  // PARCHE LOCAL (red): tres fallos del original. (1) fWaitAll se pasaba tal
  // cual como tipo de espera del kernel, donde 0 es "todos" y 1 "cualquiera": al
  // reves. El hilo de recepcion de DirtySock (sub_8282A078) espera con
  // fWaitAll = FALSE al evento global y a los de cada socket, y quedaba parado
  // hasta que saltasen todos a la vez: el anfitrion recibia el primer INIT de
  // CommUDP (puerto 3658) y ninguno mas. (2) El plazo va en ms y el kernel lo
  // quiere en unidades de 100 ns, relativo = negativo; antes era un instante
  // absoluto ya pasado. (3) Devolvia siempre 0, no WSA_WAIT_EVENT_0 + indice ni
  // WSA_WAIT_TIMEOUT (0x102): el juego reinicia el evento que cree que salto.
  uint64_t timeout_wait = static_cast<uint64_t>(-10000LL * static_cast<int64_t>(timeout));
  const uint32_t tipo = wait_all ? 0 : 1;

  X_STATUS result = 0;
  do {
    result = xboxkrnl::xeNtWaitForMultipleObjectsEx(num_events, events, tipo, 1, alertable,
                                                    timeout != ~0u ? &timeout_wait : nullptr);
  } while (result == X_STATUS_ALERTED);

  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return ~0u;
  }
  // STATUS_WAIT_0 + i, STATUS_ABANDONED_WAIT_0 + i, STATUS_USER_APC (0xC0) y
  // STATUS_TIMEOUT (0x102) valen lo mismo que sus WSA_WAIT_*.
  return result;
}

u32 NetDll_WSACreateEvent_entry() {
  XEvent* ev = new XEvent(REX_KERNEL_STATE());
  ev->Initialize(true, false);
  return ev->handle();
}

u32 NetDll_WSACloseEvent_entry(u32 event_handle) {
  X_STATUS result = REX_KERNEL_OBJECTS()->ReleaseHandle(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSAResetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::xeNtClearEvent(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSASetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

struct XnAddrStatus {
  // Address acquisition is not yet complete
  static const uint32_t XNET_GET_XNADDR_PENDING = 0x00000000;
  // XNet is uninitialized or no debugger found
  static const uint32_t XNET_GET_XNADDR_NONE = 0x00000001;
  // Host has ethernet address (no IP address)
  static const uint32_t XNET_GET_XNADDR_ETHERNET = 0x00000002;
  // Host has statically assigned IP address
  static const uint32_t XNET_GET_XNADDR_STATIC = 0x00000004;
  // Host has DHCP assigned IP address
  static const uint32_t XNET_GET_XNADDR_DHCP = 0x00000008;
  // Host has PPPoE assigned IP address
  static const uint32_t XNET_GET_XNADDR_PPPOE = 0x00000010;
  // Host has one or more gateways configured
  static const uint32_t XNET_GET_XNADDR_GATEWAY = 0x00000020;
  // Host has one or more DNS servers configured
  static const uint32_t XNET_GET_XNADDR_DNS = 0x00000040;
  // Host is currently connected to online service
  static const uint32_t XNET_GET_XNADDR_ONLINE = 0x00000080;
  // Network configuration requires troubleshooting
  static const uint32_t XNET_GET_XNADDR_TROUBLESHOOT = 0x00008000;
};

u32 NetDll_XNetGetTitleXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  if (REXCVAR_GET(red_lan)) {
    // PARCHE LOCAL (red): la IP de la LAN, como una 360 con DHCP. La MAC sale de
    // la IP para que dos aparatos de la misma red no tengan la misma.
    uint32_t ip = RedIpLocal();
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&ip);
    addr_ptr->ina.s_addr = ip;
    addr_ptr->inaOnline.s_addr = 0;
    addr_ptr->wPortOnline = 0;
    const uint8_t mac[6] = {0x00, 0x1D, 0xD8, b[1], b[2], b[3]};
    std::memcpy(addr_ptr->abEnet, mac, 6);
    std::memset(addr_ptr->abOnline, 0, 20);
    uint32_t estado = XnAddrStatus::XNET_GET_XNADDR_DHCP | XnAddrStatus::XNET_GET_XNADDR_GATEWAY |
                      XnAddrStatus::XNET_GET_XNADDR_DNS;
    static std::atomic<uint32_t> veces{0};
    if (veces.fetch_add(1) < 5) {
      RED_LOG("XNetGetTitleXnAddr -> {} estado {:02X}", RedIp(ip), estado);
    }
    return estado;
  }

  // Just return a loopback address atm.
  addr_ptr->ina.s_addr = htonl(INADDR_LOOPBACK);
  addr_ptr->inaOnline.s_addr = 0;
  addr_ptr->wPortOnline = 0;

  // TODO(gibbed): A proper mac address.
  // RakNet's 360 version appears to depend on abEnet to create "random" 64-bit
  // numbers. A zero value will cause RakPeer::Startup to fail. This causes
  // 58411436 to crash on startup.
  // The 360-specific code is scrubbed from the RakNet repo, but there's still
  // traces of what it's doing which match the game code.
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L382
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L4527
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L4467
  // "Mac address is a poor solution because you can't have multiple connections
  // from the same system"
  std::memset(addr_ptr->abEnet, 0xCC, 6);

  std::memset(addr_ptr->abOnline, 0, 20);

  return XnAddrStatus::XNET_GET_XNADDR_STATIC;
}

u32 NetDll_XNetGetDebugXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  addr_ptr.Zero();

  // XNET_GET_XNADDR_NONE causes caller to gracefully return.
  return XnAddrStatus::XNET_GET_XNADDR_NONE;
}

u32 NetDll_XNetXnAddrToMachineId_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr, mapped_u32 id_ptr) {
  // Tell the caller we're not signed in to live (non-zero ret)
  return 1;
}

void NetDll_XNetInAddrToString_entry(u32 caller, u32 in_addr, mapped_string string_out,
                                     u32 string_size) {
  rex::string::copy_truncating(string_out, "666.666.666.666", string_size);
}

// This converts a XNet address to an IN_ADDR. The IN_ADDR is used for
// subsequent socket calls (like a handle to a XNet address)
u32 NetDll_XNetXnAddrToInAddr_entry(u32 caller, ppc_ptr_t<XNADDR> xn_addr, mapped_void xid,
                                    mapped_void in_addr) {
  if (REXCVAR_GET(red_lan) && xn_addr && in_addr) {
    // PARCHE LOCAL (red): sin la asociacion segura de XNet, la direccion "segura"
    // es directamente la IP de la LAN que trae la XNADDR.
    uint32_t ip = xn_addr->ina.s_addr;
    std::memcpy(in_addr, &ip, 4);
    RED_LOG("XNetXnAddrToInAddr -> {}", RedIp(ip));
    return 0;
  }
  RED_LOG("XNetXnAddrToInAddr -> 1 (falla)");
  return 1;
}

// PARCHE LOCAL (red): las claves de sesion y la "conexion segura" de XNet. En LAN
// no hay asociacion segura que montar: la clave solo tiene que existir y no
// repetirse, y conectar con el otro es ya poder mandarle paquetes.
u32 NetDll_XNetCreateKey_entry(u32 caller, mapped_void xnkid, mapped_void xnkey) {
  static std::atomic<uint32_t> cuenta{0};
  uint32_t n = cuenta.fetch_add(1) + 1;
  uint32_t ip = RedIpLocal();
  uint8_t* id = static_cast<uint8_t*>(static_cast<void*>(xnkid));
  uint8_t* key = static_cast<uint8_t*>(static_cast<void*>(xnkey));
  if (id) {
    std::memcpy(id, &ip, 4);
    memory::store_and_swap<uint32_t>(id + 4, n);
  }
  if (key) {
    for (int i = 0; i < 16; ++i) key[i] = static_cast<uint8_t>(0x5A ^ (i * 31) ^ n);
  }
  RED_LOG("XNetCreateKey -> clave {}", n);
  return 0;
}

u32 NetDll_XNetRegisterKey_entry(u32 caller, mapped_void xnkid, mapped_void xnkey) {
  RED_LOG("XNetRegisterKey");
  return 0;
}

u32 NetDll_XNetUnregisterKey_entry(u32 caller, mapped_void xnkid) {
  RED_LOG("XNetUnregisterKey");
  return 0;
}

u32 NetDll_XNetConnect_entry(u32 caller, u32 in_addr) {
  RED_LOG("XNetConnect({})", RedIp(htonl(in_addr)));
  return 0;
}

u32 NetDll_XNetGetConnectStatus_entry(u32 caller, u32 in_addr) {
  return 2;  // XNET_CONNECT_STATUS_CONNECTED
}

// Does the reverse of the above.
// FIXME: Arguments may not be correct.
u32 NetDll_XNetInAddrToXnAddr_entry(u32 caller, mapped_void in_addr, ppc_ptr_t<XNADDR> xn_addr,
                                    mapped_void xid) {
  return 1;
}

// https://www.google.com/patents/WO2008112448A1?cl=en
// Reserves a port for use by system link
u32 NetDll_XNetSetSystemLinkPort_entry(u32 caller, u32 port) {
  return 1;
}

// https://github.com/ILOVEPIE/Cxbx-Reloaded/blob/master/src/CxbxKrnl/EmuXOnline.h#L39
struct XEthernetStatus {
  static const uint32_t XNET_ETHERNET_LINK_ACTIVE = 0x01;
  static const uint32_t XNET_ETHERNET_LINK_100MBPS = 0x02;
  static const uint32_t XNET_ETHERNET_LINK_10MBPS = 0x04;
  static const uint32_t XNET_ETHERNET_LINK_FULL_DUPLEX = 0x08;
  static const uint32_t XNET_ETHERNET_LINK_HALF_DUPLEX = 0x10;
};

u32 NetDll_XNetGetEthernetLinkStatus_entry(u32 caller) {
  uint32_t estado = REXCVAR_GET(red_lan) ? (XEthernetStatus::XNET_ETHERNET_LINK_ACTIVE |
                                            XEthernetStatus::XNET_ETHERNET_LINK_100MBPS |
                                            XEthernetStatus::XNET_ETHERNET_LINK_FULL_DUPLEX)
                                         : 0;
  static std::atomic<uint32_t> veces{0};
  if (veces.fetch_add(1) < 5) {
    RED_LOG("XNetGetEthernetLinkStatus -> {:02X}", estado);
  }
  return estado;
}

u32 NetDll_XNetDnsLookup_entry(u32 caller, mapped_string host, u32 event_handle, mapped_u32 pdns) {
  // PARCHE LOCAL (red): con red_dns, cualquier nombre resuelve a esa IP.
  std::string nombre = host ? std::string(host.value()) : std::string();
  std::string destino = REXCVAR_GET(red_dns);
  uint32_t ip = destino.empty() ? INADDR_NONE : inet_addr(destino.c_str());
  if (pdns) {
    auto dns_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNDNS));
    auto dns = REX_KERNEL_MEMORY()->TranslateVirtual<XNDNS*>(dns_guest);
    std::memset(dns, 0, sizeof(XNDNS));
    if (ip != INADDR_NONE) {
      dns->status = 0;
      dns->cina = 1;
      dns->aina[0].s_addr = ip;
    } else {
      dns->status = 1;  // non-zero = error
    }
    *pdns = dns_guest;
  }
  RED_LOG("XNetDnsLookup('{}') -> {}", nombre, ip != INADDR_NONE ? RedIp(ip) : "falla");
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

u32 NetDll_XNetDnsRelease_entry(u32 caller, ppc_ptr_t<XNDNS> dns) {
  if (!dns) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(dns.guest_address());
  return 0;
}

// PARCHE LOCAL (red): la medida de conexion con otros jugadores, antes de jugar.
// Era un stub y el juego se quedaba en "Esperando a que se conecten los jugadores"
// repitiendola cada 30 s. En la LAN no hay nada que medir: cada destino (cxna
// direcciones Xbox + cina de servicio) sale ya completo, contactado y con buena
// conexion.
u32 NetDll_XNetQosLookup_entry(u32 caller, u32 cxna, mapped_void apxna, mapped_void apxnkid,
                               mapped_void apxnkey, u32 cina, mapped_void aina,
                               mapped_void adwServiceId, u32 cProbes, u32 dwBitsPerSec,
                               u32 dwFlags, u32 event_handle, mapped_u32 ppxnqos) {
  uint32_t n = cxna + cina;
  uint32_t tam = 8 + std::max<uint32_t>(n, 1) * sizeof(XNQOSINFO);
  if (ppxnqos) {
    auto qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(tam);
    auto qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
    std::memset(qos, 0, tam);
    qos->count = n;
    qos->count_pending = 0;
    for (uint32_t i = 0; i < n; ++i) {
      XNQOSINFO& info = qos->info[i];
      info.flags = 0x01 | 0x02;  // XNET_XNQOSINFO_COMPLETE | TARGET_CONTACTED
      info.probes_xmit = cProbes ? cProbes : 4;
      info.probes_recv = cProbes ? cProbes : 4;
      info.rtt_min_in_msecs = 5;
      info.rtt_med_in_msecs = 8;
      info.up_bits_per_sec = 10000000;
      info.down_bits_per_sec = 10000000;
    }
    *ppxnqos = qos_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    if (ev) ev->Set(0, false);
  }
  RED_LOG("XNetQosLookup({} direcciones, {} servicios, {} sondas) -> todo contactado", cxna, cina,
          cProbes);
  return 0;
}

// PARCHE LOCAL (red): la XNADDR propia (36 bytes, como XNetGetTitleXnAddr con
// red_lan), para la informacion de sesion que rellena XSessionCreate (xgi_app.cpp).
void RedXnAddrPropia(uint8_t* destino) {
  uint32_t ip = RedIpLocal();
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&ip);
  std::memset(destino, 0, 36);
  std::memcpy(destino, &ip, 4);  // ina, en orden de red
  const uint8_t mac[6] = {0x00, 0x1D, 0xD8, b[1], b[2], b[3]};
  std::memcpy(destino + 10, mac, 6);
}

uint32_t RedIpLocalRed() {
  return RedIpLocal();
}

// PARCHE LOCAL (red): eran stubs. El juego los pide al entrar en Xbox Live y al
// unirse a una partida. El id de maquina sale de la IP: distinto en cada aparato.
u32 XNetLogonGetTitleID_entry() {
  return REX_KERNEL_STATE()->title_id();
}

u32 XNetLogonGetMachineID_entry(mapped_u64 machine_id) {
  if (machine_id) {
    uint32_t ip = ntohl(RedIpLocal());
    *machine_id = 0xFA00000000000000ull | ip;
  }
  return 0;
}

u32 NetDll_XNetQosServiceLookup_entry(u32 caller, u32 flags, u32 event_handle, mapped_u32 pqos) {
  RED_LOG("XNetQosServiceLookup(flags {:08X}) -> 0 servicios", flags);
  // Set pqos as some games will try accessing it despite non-successful result
  if (pqos) {
    auto qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNQOS));
    auto qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
    qos->count = qos->count_pending = 0;
    *pqos = qos_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

u32 NetDll_XNetQosRelease_entry(u32 caller, ppc_ptr_t<XNQOS> qos) {
  if (!qos) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(qos.guest_address());
  return 0;
}

u32 NetDll_XNetQosListen_entry(u32 caller, mapped_void id, mapped_void data, u32 data_size, u32 r7,
                               u32 flags) {
  return X_ERROR_FUNCTION_FAILED;
}

u32 NetDll_inet_addr_entry(mapped_string addr_ptr) {
  if (!addr_ptr) {
    return -1;
  }

  uint32_t addr = inet_addr(addr_ptr);
  // https://docs.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-inet_addr#return-value
  // Based on console research it seems like x360 uses old version of inet_addr
  // In case of empty string it return 0 instead of -1
  if (addr == -1 && !addr_ptr.value().length()) {
    return 0;
  }

  return rex::byte_swap(addr);
}

u32 NetDll_socket_entry(u32 caller, u32 af, u32 type, u32 protocol) {
  XSocket* socket = new XSocket(REX_KERNEL_STATE());
  X_STATUS result =
      socket->Initialize(XSocket::AddressFamily((uint32_t)af), XSocket::Type((uint32_t)type),
                         XSocket::Protocol((uint32_t)protocol));

  if (XFAILED(result)) {
    uint32_t e = RedFijarError();
    socket->Release();
    RED_LOG("socket(af {}, tipo {}, proto {}) -> falla {}", af, type, protocol, e);
    return -1;
  }

  RED_LOG("socket(af {}, tipo {}, proto {}) -> {:08X}", af, type, protocol, socket->handle());
  return socket->handle();
}

u32 NetDll_closesocket_entry(u32 caller, u32 socket_handle) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  RED_LOG("closesocket({:08X})", socket_handle);
  // TODO: Absolutely delete this object. It is no longer valid after calling
  // closesocket.
  socket->Close();
  socket->ReleaseHandle();
  return 0;
}

i32 NetDll_shutdown_entry(u32 caller, u32 socket_handle, i32 how) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  auto ret = socket->Shutdown(how);
  if (ret == -1) {
    RedFijarError();
  }
  RED_LOG("shutdown({:08X}, {}) -> {}", socket_handle, how, ret);
  return ret;
}

u32 NetDll_setsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_void optval_ptr, u32 optlen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

#if !REX_PLATFORM_WIN32
  // PARCHE LOCAL (red): niveles y nombres de Winsock -> Linux, y el valor de
  // big-endian. El original pasaba todo tal cual y en Android fallaba o tocaba
  // otra opcion. Las de cifrado de XNet (0x5801/0x5802) las sigue tratando XSocket.
  if (!(level == 0xFFFF && (optname == 0x5801 || optname == 0x5802))) {
    int n_level = 0, n_optname = 0;
    if (!RedOpcion(level, optname, &n_level, &n_optname)) {
      RED_LOG("setsockopt({:08X}, {:X}, {:X}) sin equivalente: se finge exito", socket_handle,
              level, optname);
      return 0;
    }
    const uint8_t* p = static_cast<const uint8_t*>(static_cast<void*>(optval_ptr));
    int ret = -1;
    if (n_level == SOL_SOCKET && n_optname == SO_LINGER && optlen >= 4) {
      linger l = {memory::load_and_swap<uint16_t>(p), memory::load_and_swap<uint16_t>(p + 2)};
      ret = ::setsockopt(socket->native_handle(), n_level, n_optname, &l, sizeof(l));
    } else if (n_level == SOL_SOCKET && (n_optname == SO_SNDTIMEO || n_optname == SO_RCVTIMEO) &&
               optlen >= 4) {
      uint32_t ms = memory::load_and_swap<uint32_t>(p);
      timeval tv = {static_cast<time_t>(ms / 1000), static_cast<suseconds_t>((ms % 1000) * 1000)};
      ret = ::setsockopt(socket->native_handle(), n_level, n_optname, &tv, sizeof(tv));
    } else {
      int v = optlen >= 4 ? static_cast<int>(memory::load_and_swap<uint32_t>(p))
                          : (optlen >= 1 ? p[0] : 0);
      ret = ::setsockopt(socket->native_handle(), n_level, n_optname, &v, sizeof(v));
    }
    if (ret < 0) {
      uint32_t e = RedFijarError();
      RED_LOG("setsockopt({:08X}, {:X}, {:X}) -> falla {}", socket_handle, level, optname, e);
      return -1;
    }
    RED_LOG("setsockopt({:08X}, {:X}, {:X}, largo {})", socket_handle, level, optname, optlen);
    return 0;
  }
#endif
  X_STATUS status = socket->SetOption(level, optname, optval_ptr, optlen);
  return XSUCCEEDED(status) ? 0 : -1;
}

// PARCHE LOCAL (red): eran stubs. El cliente de red de EA pregunta por su propia
// direccion despues de bind y por el resultado de un connect sin bloqueo
// (SO_ERROR), y con un stub se queda leyendo basura.
static void RedGuardarSockaddr(const sockaddr_in& a, XSOCKADDR* x, uint32_t* largo) {
  uint8_t* p = reinterpret_cast<uint8_t*>(x);
  memory::store_and_swap<uint16_t>(p, a.sin_family);
  std::memcpy(p + 2, &a.sin_port, 2);  // ya en orden de red
  std::memcpy(p + 4, &a.sin_addr.s_addr, 4);
  std::memset(p + 8, 0, 8);
  *largo = 16;
}

u32 NetDll_getsockname_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> name,
                             mapped_u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(0x2736);
    return -1;
  }
  sockaddr_in a = {};
  socklen_t n = sizeof(a);
  if (::getsockname(socket->native_handle(), reinterpret_cast<sockaddr*>(&a), &n) < 0) {
    RedFijarError();
    return -1;
  }
  if (name && namelen) {
    uint32_t largo = 0;
    RedGuardarSockaddr(a, name, &largo);
    *namelen = largo;
  }
  RED_LOG("getsockname({:08X}) -> {}:{}", socket_handle, RedIp(a.sin_addr.s_addr),
          ntohs(a.sin_port));
  return 0;
}

u32 NetDll_getpeername_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> name,
                             mapped_u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(0x2736);
    return -1;
  }
  sockaddr_in a = {};
  socklen_t n = sizeof(a);
  if (::getpeername(socket->native_handle(), reinterpret_cast<sockaddr*>(&a), &n) < 0) {
    uint32_t e = RedFijarError();
    RED_LOG("getpeername({:08X}) -> falla {}", socket_handle, e);
    return -1;
  }
  if (name && namelen) {
    uint32_t largo = 0;
    RedGuardarSockaddr(a, name, &largo);
    *namelen = largo;
  }
  RED_LOG("getpeername({:08X}) -> {}:{}", socket_handle, RedIp(a.sin_addr.s_addr),
          ntohs(a.sin_port));
  return 0;
}

u32 NetDll_getsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_void optval_ptr, mapped_u32 optlen_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(0x2736);
    return -1;
  }
#if !REX_PLATFORM_WIN32
  int n_level = 0, n_optname = 0;
  if (!RedOpcion(level, optname, &n_level, &n_optname) || !optval_ptr || !optlen_ptr ||
      *optlen_ptr < 4) {
    RED_LOG("getsockopt({:08X}, {:X}, {:X}) sin equivalente -> 0", socket_handle, level, optname);
    if (optval_ptr && optlen_ptr && *optlen_ptr >= 4) {
      memory::store_and_swap<uint32_t>(static_cast<uint8_t*>(static_cast<void*>(optval_ptr)), 0);
      *optlen_ptr = 4;
    }
    return 0;
  }
  int v = 0;
  socklen_t n = sizeof(v);
  if (::getsockopt(socket->native_handle(), n_level, n_optname, &v, &n) < 0) {
    RedFijarError();
    return -1;
  }
  if (n_level == SOL_SOCKET && n_optname == SO_ERROR) {
    v = static_cast<int>(RedErrorWsa(v));
  }
  memory::store_and_swap<uint32_t>(static_cast<uint8_t*>(static_cast<void*>(optval_ptr)), static_cast<uint32_t>(v));
  *optlen_ptr = 4;
  RED_LOG("getsockopt({:08X}, {:X}, {:X}) -> {}", socket_handle, level, optname, v);
  return 0;
#else
  int n = optlen_ptr ? static_cast<int>(*optlen_ptr) : 0;
  int ret = ::getsockopt(socket->native_handle(), level, optname,
                         static_cast<char*>(static_cast<void*>(optval_ptr)), &n);
  if (ret < 0) {
    RedFijarError();
    return -1;
  }
  if (optlen_ptr) *optlen_ptr = n;
  return 0;
#endif
}

u32 NetDll_ioctlsocket_entry(u32 caller, u32 socket_handle, u32 cmd, mapped_void arg_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

#if !REX_PLATFORM_WIN32
  // PARCHE LOCAL (red): FIONBIO y FIONREAD tienen otro numero en Winsock
  // (0x8004667E y 0x4004667F) que en Linux, y el argumento va en big-endian. El
  // original los pasaba tal cual: en Android el socket nunca dejaba de bloquear.
  if (cmd == 0x8004667E) {
    int v = memory::load_and_swap<uint32_t>(static_cast<uint8_t*>(static_cast<void*>(arg_ptr))) ? 1 : 0;
    if (::ioctl(socket->native_handle(), FIONBIO, &v) < 0) {
      RedFijarError();
      return -1;
    }
    RED_LOG("ioctlsocket({:08X}, FIONBIO, {})", socket_handle, v);
    return 0;
  }
  if (cmd == 0x4004667F) {
    int v = 0;
    if (::ioctl(socket->native_handle(), FIONREAD, &v) < 0) {
      RedFijarError();
      return -1;
    }
    memory::store_and_swap<uint32_t>(static_cast<uint8_t*>(static_cast<void*>(arg_ptr)), static_cast<uint32_t>(v));
    return 0;
  }
  RED_LOG("ioctlsocket({:08X}, {:08X}) desconocido", socket_handle, cmd);
#endif
  X_STATUS status = socket->IOControl(cmd, arg_ptr);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  // TODO
  return 0;
}

u32 NetDll_bind_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name, u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_name(name);
  uint32_t ip = htonl(native_name.sin_addr);
  X_STATUS status = socket->Bind(&native_name, sizeof(sockaddr_in));
  if (XFAILED(status)) {
    uint32_t e = RedFijarError();
    RED_LOG("bind({:08X}, {}:{}) -> falla {}", socket_handle, RedIp(ip),
            static_cast<uint16_t>(native_name.sin_port), e);
    return -1;
  }

  RED_LOG("bind({:08X}, {}:{})", socket_handle, RedIp(ip),
          static_cast<uint16_t>(native_name.sin_port));
  return 0;
}

u32 NetDll_connect_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> name, u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_name(name);
  uint16_t puerto = 0;
  uint32_t ip = 0;
  std::memcpy(&puerto, native_name.sa_data, 2);
  std::memcpy(&ip, native_name.sa_data + 2, 4);
  X_STATUS status = socket->Connect(&native_name, sizeof(sockaddr_in));
  if (XFAILED(status)) {
    // Sin bloqueo, EINPROGRESS -> WSAEWOULDBLOCK, que es lo que espera el juego.
    uint32_t e = RedFijarError();
    RED_LOG("connect({:08X}, {}:{}) -> {}", socket_handle, RedIp(ip), ntohs(puerto),
            e == 10035 ? std::string("en curso (WOULDBLOCK)") : "falla " + std::to_string(e));
    return -1;
  }

  RED_LOG("connect({:08X}, {}:{}) -> conectado", socket_handle, RedIp(ip), ntohs(puerto));
  return 0;
}

u32 NetDll_listen_entry(u32 caller, u32 socket_handle, i32 backlog) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->Listen(backlog);
  if (XFAILED(status)) {
    uint32_t e = RedFijarError();
    RED_LOG("listen({:08X}) -> falla {}", socket_handle, e);
    return -1;
  }

  RED_LOG("listen({:08X}, {})", socket_handle, backlog);
  return 0;
}

u32 NetDll_accept_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> addr_ptr,
                        mapped_u32 addrlen_ptr) {
  if (!addr_ptr) {
    // WSAEFAULT
    XThread::SetLastError(0x271E);
    return -1;
  }

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_addr(addr_ptr);
  int native_len = *addrlen_ptr;
  auto new_socket = socket->Accept(&native_addr, &native_len);
  if (new_socket) {
    addr_ptr->address_family = native_addr.address_family;
    std::memcpy(addr_ptr->sa_data, native_addr.sa_data, *addrlen_ptr - 2);
    *addrlen_ptr = native_len;

    RED_LOG("accept({:08X}) -> {:08X}", socket_handle, new_socket->handle());
    return new_socket->handle();
  } else {
    uint32_t e = RedFijarError();
    if (e != 10035) RED_LOG("accept({:08X}) -> falla {}", socket_handle, e);
    return -1;
  }
}

struct x_fd_set {
  rex::be<uint32_t> fd_count;
  rex::be<uint32_t> fd_array[64];
};

struct host_set {
  uint32_t count;
  object_ref<XSocket> sockets[64];

  void Load(const x_fd_set* guest_set) {
    assert_true(guest_set->fd_count < 64);
    this->count = guest_set->fd_count;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
      if (socket_handle == -1) {
        this->count = i;
        break;
      }
      // Convert from Xenia -> native
      auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
      assert_not_null(socket);
      this->sockets[i] = socket;
    }
  }

  void Store(x_fd_set* guest_set) {
    guest_set->fd_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      guest_set->fd_array[guest_set->fd_count++] = socket->handle();
    }
  }

  void Store(fd_set* native_set) {
    FD_ZERO(native_set);
    for (uint32_t i = 0; i < this->count; ++i) {
      FD_SET(this->sockets[i]->native_handle(), native_set);
    }
  }

  void UpdateFrom(fd_set* native_set) {
    uint32_t new_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      if (FD_ISSET(socket->native_handle(), native_set)) {
        this->sockets[new_count++] = socket;
      }
    }
    this->count = new_count;
  }
};

i32 NetDll_select_entry(i32 caller, i32 nfds, ppc_ptr_t<x_fd_set> readfds,
                        ppc_ptr_t<x_fd_set> writefds, ppc_ptr_t<x_fd_set> exceptfds,
                        mapped_void timeout_ptr) {
  host_set host_readfds = {};
  fd_set native_readfds = {};
  if (readfds) {
    host_readfds.Load(readfds);
    host_readfds.Store(&native_readfds);
  }
  host_set host_writefds = {};
  fd_set native_writefds = {};
  if (writefds) {
    host_writefds.Load(writefds);
    host_writefds.Store(&native_writefds);
  }
  host_set host_exceptfds = {};
  fd_set native_exceptfds = {};
  if (exceptfds) {
    host_exceptfds.Load(exceptfds);
    host_exceptfds.Store(&native_exceptfds);
  }
  timeval* timeout_in = nullptr;
  timeval timeout;
  if (timeout_ptr) {
    // PARCHE LOCAL (red): el timeval del juego va en big-endian; as_array lo leia
    // tal cual. Y en 64 bits tv_sec es de 8 bytes: se escala en variables de 4.
    const uint8_t* t = static_cast<const uint8_t*>(static_cast<void*>(timeout_ptr));
    int32_t seg = static_cast<int32_t>(memory::load_and_swap<uint32_t>(t));
    int32_t useg = static_cast<int32_t>(memory::load_and_swap<uint32_t>(t + 4));
    chrono::Clock::ScaleGuestDurationTimeval(&seg, &useg);
    timeout = {};
    timeout.tv_sec = seg;
    timeout.tv_usec = useg;
    timeout_in = &timeout;
  }
#if !REX_PLATFORM_WIN32
  // PARCHE LOCAL (red): Winsock ignora nfds y el juego pasa lo que sea (0); en
  // POSIX tiene que ser el descriptor mas alto + 1, o select no mira ninguno.
  {
    int alto = -1;
    for (auto* hs : {&host_readfds, &host_writefds, &host_exceptfds}) {
      for (uint32_t i = 0; i < hs->count; ++i) {
        alto = std::max(alto, static_cast<int>(hs->sockets[i]->native_handle()));
      }
    }
    nfds = alto + 1;
  }
#endif
  int ret = select(nfds, readfds ? &native_readfds : nullptr, writefds ? &native_writefds : nullptr,
                   exceptfds ? &native_exceptfds : nullptr, timeout_in);
  if (ret < 0) {
    uint32_t e = RedFijarError();
    RED_LOG("select -> falla {}", e);
  }
  if (readfds) {
    host_readfds.UpdateFrom(&native_readfds);
    host_readfds.Store(readfds);
  }
  if (writefds) {
    host_writefds.UpdateFrom(&native_writefds);
    host_writefds.Store(writefds);
  }
  if (exceptfds) {
    host_exceptfds.UpdateFrom(&native_exceptfds);
    host_exceptfds.Store(exceptfds);
  }

  // TODO(gibbed): modify ret to be what's actually copied to the guest fd_sets?
  return ret;
}

u32 NetDll_recv_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  int ret = socket->Recv(buf_ptr, buf_len, flags);
  if (ret < 0) {
    uint32_t e = RedFijarError();
    if (e != 10035) RED_LOG("recv({:08X}) -> falla {}", socket_handle, e);
  } else {
    RED_LOG("recv({:08X}) -> {} bytes {}", socket_handle, ret,
            RedVolcado(static_cast<const uint8_t*>(static_cast<void*>(buf_ptr)), ret));
  }
  return ret;
}

u32 NetDll_recvfrom_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len,
                          u32 flags, ppc_ptr_t<XSOCKADDR_IN> from_ptr, mapped_u32 fromlen_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_from;
  if (from_ptr) {
    native_from = *from_ptr;
  }
  uint32_t native_fromlen = fromlen_ptr ? fromlen_ptr.value() : 0;
  int ret =
      socket->RecvFrom(buf_ptr, buf_len, flags, &native_from, fromlen_ptr ? &native_fromlen : 0);

  if (from_ptr) {
    from_ptr->sin_family = native_from.sin_family;
    from_ptr->sin_port = native_from.sin_port;
    from_ptr->sin_addr = native_from.sin_addr;
    std::memset(from_ptr->x_sin_zero, 0, sizeof(from_ptr->x_sin_zero));
  }
  if (fromlen_ptr) {
    *fromlen_ptr = native_fromlen;
  }

  if (ret == -1) {
    uint32_t e = RedFijarError();
    if (e != 10035) RED_LOG("recvfrom({:08X}) -> falla {}", socket_handle, e);
  } else {
    RED_LOG("recvfrom({:08X}) <- {}:{} {} bytes {}", socket_handle,
            RedIp(htonl(native_from.sin_addr)), static_cast<uint16_t>(native_from.sin_port), ret,
            RedVolcado(static_cast<const uint8_t*>(static_cast<void*>(buf_ptr)), ret));
  }

  return ret;
}

u32 NetDll_send_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  int ret = socket->Send(buf_ptr, buf_len, flags);
  if (ret < 0) {
    uint32_t e = RedFijarError();
    RED_LOG("send({:08X}, {} bytes) -> falla {}", socket_handle, buf_len, e);
  } else {
    RED_LOG("send({:08X}) -> {} de {} bytes {}", socket_handle, ret, buf_len,
            RedVolcado(static_cast<const uint8_t*>(static_cast<void*>(buf_ptr)), buf_len));
  }
  return ret;
}

u32 NetDll_sendto_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags,
                        ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_to;
  if (to_ptr) native_to = *to_ptr;
  int ret = socket->SendTo(buf_ptr, buf_len, flags, to_ptr ? &native_to : nullptr, to_len);
  uint32_t ip = to_ptr ? htonl(native_to.sin_addr) : 0;
  uint16_t puerto = to_ptr ? static_cast<uint16_t>(native_to.sin_port) : 0;
  if (ret < 0) {
    uint32_t e = RedFijarError();
    RED_LOG("sendto({:08X}) -> {}:{} {} bytes: falla {}", socket_handle, RedIp(ip), puerto,
            buf_len, e);
  } else {
    RED_LOG("sendto({:08X}) -> {}:{} {} bytes {}", socket_handle, RedIp(ip), puerto, ret,
            RedVolcado(static_cast<const uint8_t*>(static_cast<void*>(buf_ptr)), buf_len));
  }
  // La primera vez que un socket manda a un destino, quien lo manda.
  if (REXCVAR_GET(red_registro)) {
    static std::mutex m;
    static std::unordered_map<uint64_t, uint64_t> visto;
    std::lock_guard<std::mutex> l(m);
    uint64_t destino = (static_cast<uint64_t>(ip) << 16) | puerto;
    auto it = visto.find(socket_handle);
    if (it == visto.end() || it->second != destino) {
      visto[socket_handle] = destino;
      RED_LOG("sendto({:08X}) -> {}:{} desde {}", socket_handle, RedIp(ip), puerto, RedPila());
    }
  }
  return ret;
}

u32 NetDll___WSAFDIsSet_entry(u32 socket_handle, ppc_ptr_t<x_fd_set> fd_set) {
  const uint8_t max_fd_count = std::min((uint32_t)fd_set->fd_count, uint32_t(64));
  for (uint8_t i = 0; i < max_fd_count; i++) {
    if (fd_set->fd_array[i] == socket_handle) {
      return 1;
    }
  }
  return 0;
}

void NetDll_WSASetLastError_entry(u32 error_code) {
  XThread::SetLastError(error_code);
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__NetDll_XNetStartup, rex::kernel::xam::NetDll_XNetStartup_entry)
REX_EXPORT(__imp__NetDll_XNetCleanup, rex::kernel::xam::NetDll_XNetCleanup_entry)
REX_EXPORT(__imp__NetDll_XNetGetOpt, rex::kernel::xam::NetDll_XNetGetOpt_entry)
REX_EXPORT(__imp__NetDll_XNetRandom, rex::kernel::xam::NetDll_XNetRandom_entry)
REX_EXPORT(__imp__NetDll_WSAStartup, rex::kernel::xam::NetDll_WSAStartup_entry)
REX_EXPORT(__imp__NetDll_WSACleanup, rex::kernel::xam::NetDll_WSACleanup_entry)
REX_EXPORT(__imp__NetDll_WSAGetLastError, rex::kernel::xam::NetDll_WSAGetLastError_entry)
REX_EXPORT(__imp__NetDll_WSARecvFrom, rex::kernel::xam::NetDll_WSARecvFrom_entry)
REX_EXPORT(__imp__NetDll_WSASendTo, rex::kernel::xam::NetDll_WSASendTo_entry)
REX_EXPORT(__imp__NetDll_WSAWaitForMultipleEvents,
           rex::kernel::xam::NetDll_WSAWaitForMultipleEvents_entry)
REX_EXPORT(__imp__NetDll_WSACreateEvent, rex::kernel::xam::NetDll_WSACreateEvent_entry)
REX_EXPORT(__imp__NetDll_WSACloseEvent, rex::kernel::xam::NetDll_WSACloseEvent_entry)
REX_EXPORT(__imp__NetDll_WSAResetEvent, rex::kernel::xam::NetDll_WSAResetEvent_entry)
REX_EXPORT(__imp__NetDll_WSASetEvent, rex::kernel::xam::NetDll_WSASetEvent_entry)
REX_EXPORT(__imp__NetDll_XNetGetTitleXnAddr, rex::kernel::xam::NetDll_XNetGetTitleXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetGetDebugXnAddr, rex::kernel::xam::NetDll_XNetGetDebugXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToMachineId,
           rex::kernel::xam::NetDll_XNetXnAddrToMachineId_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToString, rex::kernel::xam::NetDll_XNetInAddrToString_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToInAddr, rex::kernel::xam::NetDll_XNetXnAddrToInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToXnAddr, rex::kernel::xam::NetDll_XNetInAddrToXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetSetSystemLinkPort,
           rex::kernel::xam::NetDll_XNetSetSystemLinkPort_entry)
REX_EXPORT(__imp__NetDll_XNetGetEthernetLinkStatus,
           rex::kernel::xam::NetDll_XNetGetEthernetLinkStatus_entry)
REX_EXPORT(__imp__NetDll_XNetDnsLookup, rex::kernel::xam::NetDll_XNetDnsLookup_entry)
REX_EXPORT(__imp__NetDll_XNetDnsRelease, rex::kernel::xam::NetDll_XNetDnsRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosServiceLookup, rex::kernel::xam::NetDll_XNetQosServiceLookup_entry)
REX_EXPORT(__imp__NetDll_XNetQosRelease, rex::kernel::xam::NetDll_XNetQosRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosListen, rex::kernel::xam::NetDll_XNetQosListen_entry)
REX_EXPORT(__imp__NetDll_inet_addr, rex::kernel::xam::NetDll_inet_addr_entry)
REX_EXPORT(__imp__NetDll_socket, rex::kernel::xam::NetDll_socket_entry)
REX_EXPORT(__imp__NetDll_closesocket, rex::kernel::xam::NetDll_closesocket_entry)
REX_EXPORT(__imp__NetDll_shutdown, rex::kernel::xam::NetDll_shutdown_entry)
REX_EXPORT(__imp__NetDll_setsockopt, rex::kernel::xam::NetDll_setsockopt_entry)
REX_EXPORT(__imp__NetDll_ioctlsocket, rex::kernel::xam::NetDll_ioctlsocket_entry)
REX_EXPORT(__imp__NetDll_bind, rex::kernel::xam::NetDll_bind_entry)
REX_EXPORT(__imp__NetDll_connect, rex::kernel::xam::NetDll_connect_entry)
REX_EXPORT(__imp__NetDll_listen, rex::kernel::xam::NetDll_listen_entry)
REX_EXPORT(__imp__NetDll_accept, rex::kernel::xam::NetDll_accept_entry)
REX_EXPORT(__imp__NetDll_select, rex::kernel::xam::NetDll_select_entry)
REX_EXPORT(__imp__NetDll_recv, rex::kernel::xam::NetDll_recv_entry)
REX_EXPORT(__imp__NetDll_recvfrom, rex::kernel::xam::NetDll_recvfrom_entry)
REX_EXPORT(__imp__NetDll_send, rex::kernel::xam::NetDll_send_entry)
REX_EXPORT(__imp__NetDll_sendto, rex::kernel::xam::NetDll_sendto_entry)
REX_EXPORT(__imp__NetDll___WSAFDIsSet, rex::kernel::xam::NetDll___WSAFDIsSet_entry)
REX_EXPORT(__imp__NetDll_WSASetLastError, rex::kernel::xam::NetDll_WSASetLastError_entry)

REX_EXPORT_STUB(__imp__NetDll_UpnpActionCalculateWorkBufferSize);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpCleanup);
REX_EXPORT_STUB(__imp__NetDll_UpnpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpDoWork);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventGetCurrentState);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventUnsubscribe);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchGetDevices);
REX_EXPORT_STUB(__imp__NetDll_UpnpStartup);
REX_EXPORT_STUB(__imp__NetDll_WSACancelOverlappedIO);
REX_EXPORT_STUB(__imp__NetDll_WSAEventSelect);
REX_EXPORT(__imp__NetDll_WSAGetOverlappedResult,
           rex::kernel::xam::NetDll_WSAGetOverlappedResult_entry)
REX_EXPORT_STUB(__imp__NetDll_WSARecv);
REX_EXPORT_STUB(__imp__NetDll_WSASend);
REX_EXPORT_STUB(__imp__NetDll_WSAStartupEx);
REX_EXPORT_STUB(__imp__NetDll_XHttpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_XHttpConnect);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpDoWork);
REX_EXPORT_STUB(__imp__NetDll_XHttpGetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpen);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequestUsingMemory);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryAuthSchemes);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryHeaders);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpReadData);
REX_EXPORT_STUB(__imp__NetDll_XHttpReceiveResponse);
REX_EXPORT_STUB(__imp__NetDll_XHttpResetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpSendRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetCredentials);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetStatusCallback);
REX_EXPORT_STUB(__imp__NetDll_XHttpShutdown);
REX_EXPORT_STUB(__imp__NetDll_XHttpStartup);
REX_EXPORT_STUB(__imp__NetDll_XHttpWriteData);
REX_EXPORT(__imp__NetDll_XNetConnect, rex::kernel::xam::NetDll_XNetConnect_entry)
REX_EXPORT(__imp__NetDll_XNetCreateKey, rex::kernel::xam::NetDll_XNetCreateKey_entry)
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseLookup);
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseRelease);
REX_EXPORT_STUB(__imp__NetDll_XNetGetBroadcastVersionStatus);
REX_EXPORT(__imp__NetDll_XNetGetConnectStatus, rex::kernel::xam::NetDll_XNetGetConnectStatus_entry)
REX_EXPORT_STUB(__imp__NetDll_XNetGetSystemLinkPort);
REX_EXPORT_STUB(__imp__NetDll_XNetGetXnAddrPlatform);
REX_EXPORT_STUB(__imp__NetDll_XNetInAddrToServer);
REX_EXPORT_STUB(__imp__NetDll_XNetQosGetListenStats);
REX_EXPORT(__imp__NetDll_XNetQosLookup, rex::kernel::xam::NetDll_XNetQosLookup_entry)
REX_EXPORT(__imp__XNetLogonGetTitleID, rex::kernel::xam::XNetLogonGetTitleID_entry)
REX_EXPORT(__imp__XNetLogonGetMachineID, rex::kernel::xam::XNetLogonGetMachineID_entry)
REX_EXPORT(__imp__NetDll_XNetRegisterKey, rex::kernel::xam::NetDll_XNetRegisterKey_entry)
REX_EXPORT_STUB(__imp__NetDll_XNetReplaceKey);
REX_EXPORT_STUB(__imp__NetDll_XNetServerToInAddr);
REX_EXPORT_STUB(__imp__NetDll_XNetSetOpt);
REX_EXPORT_STUB(__imp__NetDll_XNetStartupEx);
REX_EXPORT_STUB(__imp__NetDll_XNetTsAddrToInAddr);
REX_EXPORT_STUB(__imp__NetDll_XNetUnregisterInAddr);
REX_EXPORT(__imp__NetDll_XNetUnregisterKey, rex::kernel::xam::NetDll_XNetUnregisterKey_entry)
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadContinue);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetParseTime);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetReceivedDataSize);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStart);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStop);
REX_EXPORT_STUB(__imp__NetDll_XnpCapture);
REX_EXPORT_STUB(__imp__NetDll_XnpConfig);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnP);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnPPortAndExternalAddr);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptRecv);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetExtendedReceiveCallback);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmit);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmitAsIp);
REX_EXPORT_STUB(__imp__NetDll_XnpGetActiveSocketList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetConfigStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpGetKeyList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetQosLookupList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetSecAssocList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetChallengeResponse);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetPState);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpNoteSystemTime);
REX_EXPORT_STUB(__imp__NetDll_XnpPersistTitleState);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetAggregateMeasurement);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetEntries);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryLoad);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistorySaveMeasurements);
REX_EXPORT_STUB(__imp__NetDll_XnpRegisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpReplaceKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpSetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpToolIpProxyInject);
REX_EXPORT_STUB(__imp__NetDll_XnpToolSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpUnregisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpUpdateConfigParams);
REX_EXPORT(__imp__NetDll_getpeername, rex::kernel::xam::NetDll_getpeername_entry)
REX_EXPORT(__imp__NetDll_getsockname, rex::kernel::xam::NetDll_getsockname_entry)
REX_EXPORT(__imp__NetDll_getsockopt, rex::kernel::xam::NetDll_getsockopt_entry)
