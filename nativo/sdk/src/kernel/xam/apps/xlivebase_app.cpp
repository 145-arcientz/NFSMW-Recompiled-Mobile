/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xam/apps/xlivebase_app.h>
#include <rex/logging.h>
#include <rex/thread.h>
#include <rex/system/xthread.h>

#include <string>

#include <rex/cvar.h>

#if REX_PLATFORM_WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

// PARCHE LOCAL (red): definidas en xam_net.cpp. Ver XOnlineGetServiceInfo abajo.
REXCVAR_DECLARE(std::string, red_dns);
REXCVAR_DECLARE(bool, red_lan);

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state) : App(kernel_state, 0xFC) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x00058004: {
      // Called on startup, seems to just return a bool in the buffer.
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetNatType({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // XONLINE_NAT_OPEN
      return X_E_SUCCESS;
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      REXKRNL_DEBUG("CXLiveLogon::GetServiceInfo({:08X}, {:08X})", buffer_ptr, buffer_length);
      // PARCHE LOCAL (red): con red_dns, el servicio esta en esa IP. Most
      // Wanted pide el servicio de EA (0x45410004) y, con la IP que le den, abre
      // la pasarela segura (XNetConnect); sin esto su NetConn se queda en "-svc"
      // y el juego dice "Has perdido tu conexion a Xbox Live". Aqui buffer_ptr es
      // el id del servicio y buffer_length el puntero a XONLINE_SERVICE_INFO
      // { dwServiceID, IN_ADDR serviceIP, WORD wServicePort, WORD wReserved }.
      {
        std::string destino = REXCVAR_GET(red_dns);
        uint32_t ip = destino.empty() ? INADDR_NONE : inet_addr(destino.c_str());
        if (ip != INADDR_NONE && buffer_length) {
          auto info = memory_->TranslateVirtual(buffer_length);
          memory::store_and_swap<uint32_t>(info + 0, buffer_ptr);
          std::memcpy(info + 4, &ip, 4);  // ya en orden de red
          memory::store_and_swap<uint16_t>(info + 8, 0);
          memory::store_and_swap<uint16_t>(info + 10, 0);
          REXKRNL_INFO("[red] XOnlineGetServiceInfo({:08X}) -> {}", buffer_ptr, destino);
          return X_E_SUCCESS;
        }
      }
      return 0x80151802;  // ERROR_CONNECTION_INVALID
    }
    case 0x00058020: {
      // 0x00058004 is called right before this.
      // We should create a XamEnumerate-able empty list here, but I'm not
      // sure of the format.
      // buffer_length seems to be the same ptr sent to 0x00058004.
      REXKRNL_DEBUG("CXLiveFriends::Enumerate({:08X}, {:08X}) unimplemented", buffer_ptr,
                    buffer_length);
      return X_E_FAIL;
    }
    case 0x00058023: {
      REXKRNL_DEBUG(
          "CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, {:08X}) "
          "unimplemented",
          buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058046: {
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only takes
      // input
      REXKRNL_DEBUG("XLiveBaseUnk58046({:08X}, {:08X}) unimplemented", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x0005800E: {
      // PARCHE LOCAL (red): XUserMuteListQuery(user_index, XUID, BOOL* silenciado).
      // buffer_ptr apunta a { user_index, pad, XUID (8), resultado }, y
      // buffer_length es el puntero al BOOL de salida. Most Wanted lo pregunta por
      // cada jugador de la partida; sin escribir la salida leia basura de la pila y
      // fallaba con un acceso invalido. Nadie esta silenciado.
      if (buffer_length) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(buffer_length), 0);
      }
      if (buffer_ptr) {
        memory::store_and_swap<uint32_t>(buffer + 16, 0);
      }
      return X_E_SUCCESS;
    }
    case 0x00058037: {
      REXKRNL_DEBUG("XPresenceInitialize({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  // PARCHE LOCAL (red): en la LAN, lo desconocido se apunta (con sus parametros,
  // para saber que es) y va bien. Most Wanted manda 0x5000D al empezar la sesion
  // de una partida y, con el error, se queda en "Comenzando sesion de juego".
  if (REXCVAR_GET(red_lan)) {
    std::string hex;
    if (buffer_ptr && buffer_length && buffer_length <= 256) {
      for (uint32_t i = 0; i < buffer_length; ++i) {
        hex += fmt::format("{:02X}", buffer[i]);
        if (i % 4 == 3) hex += " ";
      }
    }
    // Quien lo manda: lr y la cadena de marcos (el codigo del 360 guarda el lr del
    // que llama en marco_anterior - 8).
    std::string pila;
    auto* ts = rex::runtime::ThreadState::Get();
    if (auto* c = ts ? ts->context() : nullptr) {
      pila = fmt::format("lr={:08X}", static_cast<uint32_t>(c->lr));
      // Solo dentro de la pila del hilo (PCR: +0x70 tope, +0x74 fondo).
      auto* hilo = XThread::GetCurrentThread();
      uint32_t pcr = hilo ? hilo->pcr_ptr() : 0;
      uint32_t tope = pcr ? memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(pcr + 0x70)) : 0;
      uint32_t fondo = pcr ? memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(pcr + 0x74)) : 0;
      uint32_t sp = c->r1.u32;
      if (sp < fondo || sp + 4 > tope) sp = 0;
      for (int i = 0; i < 6 && sp; ++i) {
        uint32_t anterior = memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(sp));
        if (anterior <= sp || anterior + 4 > tope) break;
        pila += fmt::format(" <- {:08X}",
                            memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(anterior - 8)));
        sp = anterior;
      }
    }
    REXKRNL_INFO("[red] XLIVEBASE {:08X} desconocido -> exito. Parametros: {} | {}", message, hex,
                 pila);
    return X_E_SUCCESS;
  }
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
