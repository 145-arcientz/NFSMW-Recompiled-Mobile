// nfsmw - WMV movies decoded natively with FFmpeg (see nfsmw_video_wmv3.h)
//
// ASF container (the minimum for these files)
//  - Header: child objects with GUID and size. File properties (packet size and packet count), the
//    properties of each stream (the video one carries a BITMAPINFOHEADER with the width, the height
//    and the 4 WMV3 sequence bytes after it) and the data object, whose packets start 50 bytes later.
//  - Fixed-size packets with one or more payloads. Each payload is a piece of a media object (a
//    frame) with its number, its offset within the object and, in the replicated data, the total size
//    and the time. A frame is complete once all its bytes have been received.
//  - 1-byte replicated data = compressed payloads: several small objects in a row, each with its size
//    in one byte.
// Without B frames, each compressed frame yields one output frame in the same order.

#include "nfsmw_video_wmv3.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <thread>
#include <span>

#include <rex/filesystem.h>
#include <rex/audio/downmix.h>
#include <rex/audio/output_limiter.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <SDL3/SDL.h>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/channel_layout.h"
#include "libavutil/samplefmt.h"
}

namespace nfsmw::video_wmv3 {
namespace {

constexpr uint8_t kGuidCabecera[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                       0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr uint8_t kGuidFichero[16] = {0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11,
                                      0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kGuidFlujo[16] = {0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11,
                                    0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kGuidVideo[16] = {0xC0, 0xEF, 0x19, 0xBC, 0x4D, 0x5B, 0xCF, 0x11,
                                    0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};
constexpr uint8_t kGuidAudio[16] = {0x40, 0x9E, 0x69, 0xF8, 0x4D, 0x5B, 0xCF, 0x11,
                                    0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};
constexpr uint8_t kGuidDatos[16] = {0x36, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                    0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr size_t kMaxCabecera = 512 * 1024;

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }
uint64_t Le64(const uint8_t* p) { return uint64_t(Le32(p)) | (uint64_t(Le32(p + 4)) << 32); }

// ASF variable-length field: 0, 1, 2 or 4 bytes depending on the type (0-3).
bool LeerVariable(const std::vector<uint8_t>& b, size_t& p, int tipo, uint32_t& valor) {
  static constexpr int kBytes[4] = {0, 1, 2, 4};
  const int n = kBytes[tipo & 3];
  if (p + size_t(n) > b.size()) {
    return false;
  }
  valor = n == 0 ? 0 : n == 1 ? b[p] : n == 2 ? Le16(&b[p]) : Le32(&b[p]);
  p += size_t(n);
  return true;
}

struct Objeto {
  std::vector<uint8_t> datos;
  uint32_t recibidos = 0;
  bool clave = false;
};

// File opened through the SDK's VFS (folder or ISO).
struct FicheroVfs {
  rex::filesystem::File* f = nullptr;
  uint64_t tamano = 0;

  ~FicheroVfs() {
    if (f) {
      f->Destroy();
    }
  }

  bool Abrir(const std::string& ruta) {
    rex::filesystem::FileAction accion;
    const auto estado = REX_KERNEL_FS()->OpenFile(nullptr, ruta, rex::filesystem::FileDisposition::kOpen,
                                                  rex::filesystem::FileAccess::kGenericRead, false, true, &f,
                                                  &accion);
    if (estado != 0 || !f) {
      REXLOG_WARN("[video] WMV3 nativo: no se puede abrir '{}' ({:08X})", ruta, uint32_t(estado));
      f = nullptr;
      return false;
    }
    tamano = f->entry()->size();
    return true;
  }

  bool Leer(uint64_t desplazamiento, uint8_t* destino, size_t bytes) {
    size_t leidos = 0;
    const auto estado = f->ReadSync(std::span<uint8_t>(destino, bytes), size_t(desplazamiento), &leidos);
    return estado == 0 && leidos == bytes;
  }
};

struct Contenedor {
  InfoWmv info;
  uint64_t inicio_datos = 0;
  uint64_t paquetes = 0;
  uint32_t tamano_paquete = 0;
  uint32_t flujo_video = 0;
  uint32_t flujo_audio = 0;
  int canales_audio = 0;
  int frecuencia_audio = 0;
  int bloque_audio = 0;
  int64_t bitrate_audio = 0;
  int bits_audio = 0;
  std::vector<uint8_t> extra_audio;
  AVCodecID codec_audio = AV_CODEC_ID_NONE;
};

bool LeerCabecera(FicheroVfs& fichero, const std::string& ruta, Contenedor& c) {
  std::vector<uint8_t> cabecera(size_t(std::min<uint64_t>(fichero.tamano, kMaxCabecera)));
  if (cabecera.size() < 128 || !fichero.Leer(0, cabecera.data(), cabecera.size()) ||
      std::memcmp(cabecera.data(), kGuidCabecera, 16) != 0) {
    REXLOG_WARN("[video] WMV3 nativo: '{}' no empieza por una cabecera ASF", ruta);
    return false;
  }
  const uint64_t tam_cabecera = Le64(&cabecera[16]);
  size_t p = 30;
  bool video = false;
  while (p + 24 <= cabecera.size() && p < tam_cabecera) {
    const uint8_t* o = &cabecera[p];
    const uint64_t tam = Le64(o + 16);
    if (tam < 24 || p + tam > cabecera.size()) {
      break;
    }
    if (std::memcmp(o, kGuidFichero, 16) == 0 && tam >= 104) {
      c.paquetes = Le64(o + 56);
      c.tamano_paquete = Le32(o + 92);
    } else if (std::memcmp(o, kGuidFlujo, 16) == 0 && tam >= 78) {
      const uint32_t flujo = Le16(o + 72) & 0x7F;
      const uint8_t* especifico = o + 78;
      const uint32_t tam_especifico = Le32(o + 64);
      if (!video && std::memcmp(o + 24, kGuidVideo, 16) == 0 && tam_especifico >= 51 &&
          78 + uint64_t(tam_especifico) <= tam) {
        c.flujo_video = flujo;
        const uint8_t* bmi = especifico + 11;
        const uint32_t tam_bmi = Le32(bmi);
        c.info.ancho = int(Le32(bmi + 4));
        c.info.alto = std::abs(int(Le32(bmi + 8)));
        if (tam_bmi > 40 && tam_bmi <= tam_especifico - 11) {
          c.info.secuencia.assign(bmi + 40, bmi + tam_bmi);
        }
        video = std::memcmp(bmi + 16, "WMV3", 4) == 0;
      } else if (std::memcmp(o + 24, kGuidAudio, 16) == 0 && tam_especifico >= 18 &&
                 78 + uint64_t(tam_especifico) <= tam) {
        const uint8_t* wave = especifico;
        const uint32_t cb_extra = Le16(wave + 16);
        const uint16_t formato_audio = Le16(wave);
        if ((formato_audio == 0x0162 || formato_audio == 0x0161) && 18 + cb_extra <= tam_especifico) {
          c.flujo_audio = flujo;
          c.canales_audio = Le16(wave + 2);
          c.frecuencia_audio = int(Le32(wave + 4));
          c.bloque_audio = Le16(wave + 12);
          c.bitrate_audio = int64_t(Le32(wave + 8)) * 8;
          c.bits_audio = Le16(wave + 14);
          c.extra_audio.assign(wave + 18, wave + 18 + cb_extra);
          if (c.canales_audio > 0 && c.frecuencia_audio > 0) {
            c.codec_audio = formato_audio == 0x0162 ? AV_CODEC_ID_WMAPRO : AV_CODEC_ID_WMAV2;
          }
        }
      }
    }
    p += size_t(tam);
  }
  if (!video || !c.tamano_paquete || !c.info.ancho || !c.info.alto) {
    REXLOG_WARN("[video] WMV3 nativo: '{}' sin flujo WMV3 utilizable (paquete {}, {}x{})", ruta, c.tamano_paquete,
                c.info.ancho, c.info.alto);
    return false;
  }
  // The data object comes right after the header; its packets start 50 bytes later.
  std::vector<uint8_t> datos(50);
  if (!fichero.Leer(tam_cabecera, datos.data(), datos.size()) || std::memcmp(datos.data(), kGuidDatos, 16) != 0) {
    REXLOG_WARN("[video] WMV3 nativo: '{}' sin objeto de datos detras de la cabecera", ruta);
    return false;
  }
  c.inicio_datos = tam_cabecera + 50;
  return true;
}

}  // namespace

bool LeerInfoWmv(const std::string& ruta, InfoWmv& info) {
  FicheroVfs fichero;
  Contenedor c;
  if (!fichero.Abrir(ruta) || !LeerCabecera(fichero, ruta, c)) {
    return false;
  }
  info = std::move(c.info);
  return true;
}

// --- Audio WMA Pro de las peliculas ------------------------------------------------------------------------

struct AudioWmaPro::Estado {
  struct PaqueteAudio {
    std::vector<uint8_t> datos;
    uint32_t recibidos = 0;
  };

  FicheroVfs fichero;
  Contenedor c;
  uint64_t siguiente_paquete = 0;
  std::vector<uint8_t> paquete;
  std::map<uint32_t, PaqueteAudio> en_curso;
  AVCodecContext* codec = nullptr;
  AVPacket* pkt = nullptr;
  AVFrame* frame = nullptr;
  SDL_AudioStream* salida = nullptr;
  std::atomic<bool> detener{false};
  std::thread hilo;
  std::atomic<uint64_t> paquetes_decodificados{0};
  std::atomic<uint64_t> muestras_salida{0};
  // Steady-clock time of the last frame the game decoded (Latido).
  std::atomic<int64_t> latido_us{0};
  bool callado = false;
  bool sustituye_salida = false;  // owned by the decoder thread; released after joining it
  static constexpr int64_t kSilencioUs = 800'000;

  void SustituirSalidaJuego(bool activa) {
    if (sustituye_salida == activa) return;
    sustituye_salida = activa;
    rex::audio::SetGameOutputSuppressed(activa);
    REXLOG_INFO("[video] salida del juego {} durante la pista nativa de la pelicula",
                activa ? "silenciada" : "restaurada");
  }

  static int64_t AhoraUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  // Waits for room in the output queue, and for the game to keep showing the movie. Returns false when the
  // audio has to stop (the movie object is going away).
  //
  // Frames come every 33-40 ms, but while the game loads behind a movie (the attract movie under the title
  // screen) it stalls for a few hundred ms and then goes on: the audio keeps playing through that. When the
  // player skips a cutscene the frames stop for good; after kSilencioUs the queued audio is dropped and nothing
  // more is sent. If frames come back, the audio carries on from where it was.
  bool EsperarTurno() {
    const int kMaxColaBytes = c.frecuencia_audio * 2 * int(sizeof(float)) / 4;
    for (;;) {
      if (detener.load(std::memory_order_relaxed)) {
        return false;
      }
      const int64_t sin_video = AhoraUs() - latido_us.load(std::memory_order_relaxed);
      if (sin_video > kSilencioUs) {
        if (!callado) {
          callado = true;
          SDL_ClearAudioStream(salida);
          SustituirSalidaJuego(false);
          REXLOG_INFO("[video] audio WMA Pro: el juego dejo de pedir fotogramas; audio en silencio");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      callado = false;
      if (SDL_GetAudioStreamQueued(salida) > kMaxColaBytes) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
      return true;
    }
  }

  ~Estado() {
    detener.store(true, std::memory_order_relaxed);
    if (hilo.joinable()) {
      hilo.join();
    }
    SustituirSalidaJuego(false);
    if (salida) {
      SDL_DestroyAudioStream(salida);
    }
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    if (codec) avcodec_free_context(&codec);
  }

  void Entregar(const uint8_t* datos, uint32_t bytes) {
    auto decodificar = [&]() {
      av_packet_unref(pkt);
      if (bytes > uint32_t(std::numeric_limits<int>::max()) || av_new_packet(pkt, int(bytes)) < 0) {
        return;
      }
      std::memcpy(pkt->data, datos, bytes);
      if (avcodec_send_packet(codec, pkt) < 0) {
        return;
      }
      while (avcodec_receive_frame(codec, frame) >= 0) {
        ConvertirYEnviar(frame);
        av_frame_unref(frame);
      }
      paquetes_decodificados.fetch_add(1, std::memory_order_relaxed);
    };
    decodificar();
  }

  float Muestra(int canal, int indice, bool planar, AVSampleFormat formato) const {
    const uint8_t* p = frame->extended_data[planar ? canal : 0];
    const int canales = std::max(frame->channels, 1);
    const int posicion = planar ? indice : indice * canales + canal;
    switch (formato) {
      case AV_SAMPLE_FMT_U8: return (float(p[posicion]) - 128.0f) / 128.0f;
      case AV_SAMPLE_FMT_S16: return reinterpret_cast<const int16_t*>(p)[posicion] / 32768.0f;
      case AV_SAMPLE_FMT_S32: return reinterpret_cast<const int32_t*>(p)[posicion] / 2147483648.0f;
      case AV_SAMPLE_FMT_S64: return float(double(reinterpret_cast<const int64_t*>(p)[posicion]) / 9223372036854775808.0);
      case AV_SAMPLE_FMT_FLT: return reinterpret_cast<const float*>(p)[posicion];
      case AV_SAMPLE_FMT_DBL: return float(reinterpret_cast<const double*>(p)[posicion]);
      default: return 0.0f;
    }
  }

  // 5.1 to stereo, at the movie's own rate (SDL resamples to the device). Same weights as the game's fold on
  // Android (front 1, centre and surround 0.707, LFE 0.5). Limit the unclipped stereo sum at the source rate.
  float limitador = 1.0f;

  void ConvertirYEnviar(const AVFrame* f) {
    if (f->nb_samples <= 0 || !f->extended_data || !f->extended_data[0]) return;
    const AVSampleFormat formato = static_cast<AVSampleFormat>(f->format);
    const AVSampleFormat compacto = av_get_packed_sample_fmt(formato);
    if (compacto == AV_SAMPLE_FMT_NONE) return;
    frame = const_cast<AVFrame*>(f);
    const bool planar = av_sample_fmt_is_planar(formato) != 0;
    const int canales = std::max(f->channels, 1);
    const int cantidad = f->nb_samples;
    const uint64_t layout =
        f->channel_layout ? f->channel_layout : uint64_t(av_get_default_channel_layout(canales));

    // Weight of each input channel on the left and the right.
    std::array<float, 8> peso_izq{}, peso_der{};
    for (int ch = 0; ch < canales && ch < 8; ++ch) {
      const uint64_t destino = layout ? av_channel_layout_extract_channel(layout, ch) : 0;
      float izq = 0.0f, der = 0.0f;
      switch (destino) {
        case AV_CH_FRONT_LEFT: izq = 1.0f; break;
        case AV_CH_FRONT_RIGHT: der = 1.0f; break;
        case AV_CH_FRONT_LEFT_OF_CENTER: case AV_CH_BACK_LEFT: case AV_CH_SIDE_LEFT:
        case AV_CH_TOP_FRONT_LEFT: case AV_CH_TOP_BACK_LEFT: izq = 0.70710678f; break;
        case AV_CH_FRONT_RIGHT_OF_CENTER: case AV_CH_BACK_RIGHT: case AV_CH_SIDE_RIGHT:
        case AV_CH_TOP_FRONT_RIGHT: case AV_CH_TOP_BACK_RIGHT: der = 0.70710678f; break;
        case AV_CH_FRONT_CENTER: case AV_CH_TOP_CENTER: case AV_CH_TOP_FRONT_CENTER:
          izq = der = 0.70710678f; break;
        case AV_CH_LOW_FREQUENCY: izq = der = 0.5f; break;
        default:
          if (canales == 1) izq = der = 1.0f;
          else if (ch == 0) izq = 1.0f;
          else if (ch == 1) der = 1.0f;
          break;
      }
      peso_izq[size_t(ch)] = izq;
      peso_der[size_t(ch)] = der;
    }

    std::vector<float> pcm(size_t(cantidad) * 2);
    for (int i = 0; i < cantidad; ++i) {
      float izq = 0.0f, der = 0.0f;
      for (int ch = 0; ch < canales && ch < 8; ++ch) {
        const float m = Muestra(ch, i, planar, compacto);
        izq += m * peso_izq[size_t(ch)];
        der += m * peso_der[size_t(ch)];
      }
      pcm[size_t(i) * 2] = izq;
      pcm[size_t(i) * 2 + 1] = der;
    }
    rex::audio::LimitOutput(pcm.data(), size_t(cantidad), 2,
                           f->sample_rate > 0 ? f->sample_rate : c.frecuencia_audio, limitador);
    // Keep a short lead-in (250 ms) so decoding runs ahead without buffering the whole movie, and only while the
    // game is still showing it.
    if (EsperarTurno() && SDL_PutAudioStreamData(salida, pcm.data(), int(pcm.size() * sizeof(float)))) {
      // The movie has its own SDL stream. Keep the guest mixer draining but
      // inaudible here, so it cannot double the dialogue or add decoding noise.
      SustituirSalidaJuego(true);
      muestras_salida.fetch_add(uint64_t(cantidad), std::memory_order_relaxed);
    }
  }

  void Carga(uint32_t objeto, uint32_t desplazamiento, uint32_t tamano, const uint8_t* datos, uint32_t bytes) {
    if (!tamano || tamano > 1024 * 1024 || uint64_t(desplazamiento) + bytes > tamano) return;
    auto& o = en_curso[objeto];
    if (o.datos.empty()) o.datos.assign(tamano, 0);
    if (o.datos.size() != tamano) { en_curso.erase(objeto); return; }
    std::memcpy(o.datos.data() + desplazamiento, datos, bytes);
    o.recibidos += bytes;
    if (o.recibidos >= o.datos.size()) {
      Entregar(o.datos.data(), uint32_t(o.datos.size()));
      en_curso.erase(objeto);
    }
  }

  bool LeerPaquete() {
    if (siguiente_paquete >= c.paquetes) return false;
    paquete.resize(c.tamano_paquete);
    if (!fichero.Leer(c.inicio_datos + siguiente_paquete * uint64_t(c.tamano_paquete), paquete.data(), c.tamano_paquete))
      return false;
    ++siguiente_paquete;
    const auto& b = paquete;
    size_t p = 0;
    const uint8_t ec = b[p];
    if (ec & 0x80) p += 1 + (ec & 0x0F);
    if (p + 2 > b.size()) return false;
    const uint8_t tipos = b[p++];
    const uint8_t propiedades = b[p++];
    uint32_t longitud = 0, secuencia = 0, relleno = 0;
    if (!LeerVariable(b, p, (tipos >> 5) & 3, longitud) || !LeerVariable(b, p, (tipos >> 1) & 3, secuencia) ||
        !LeerVariable(b, p, (tipos >> 3) & 3, relleno)) return false;
    p += 6;
    const bool multiples = tipos & 1;
    int n = 1, tipo_longitud = 0;
    if (multiples) {
      if (p >= b.size()) return false;
      n = b[p] & 0x3F;
      tipo_longitud = (b[p] >> 6) & 3;
      ++p;
    }
    const size_t fin = std::min<size_t>(b.size(), (longitud ? longitud : c.tamano_paquete)) -
                       std::min<size_t>(relleno, b.size());
    for (int i = 0; i < n && p < fin; ++i) {
      const uint32_t flujo = b[p++] & 0x7F;
      uint32_t objeto = 0, desplazamiento = 0, replicados = 0;
      if (!LeerVariable(b, p, (propiedades >> 4) & 3, objeto) ||
          !LeerVariable(b, p, (propiedades >> 2) & 3, desplazamiento) ||
          !LeerVariable(b, p, propiedades & 3, replicados) || p + replicados > b.size()) return false;
      const size_t pos_replicados = p;
      p += replicados;
      uint32_t bytes = 0;
      if (multiples) {
        if (!LeerVariable(b, p, tipo_longitud, bytes)) return false;
      } else {
        bytes = uint32_t(fin > p ? fin - p : 0);
      }
      if (p + bytes > b.size()) return false;
      if (flujo == c.flujo_audio && replicados >= 8) {
        Carga(objeto, desplazamiento, Le32(&b[pos_replicados]), &b[p], bytes);
      }
      p += bytes;
    }
    return true;
  }

  void Ejecutar() {
    while (!detener.load(std::memory_order_relaxed) && LeerPaquete()) {}

    // Keep ownership through the final queued samples. Restore gameplay on EOF,
    // a skipped movie, or shutdown, even if the movie object survives in memory.
    while (!detener.load(std::memory_order_relaxed) && SDL_GetAudioStreamQueued(salida) > 0) {
      if (AhoraUs() - latido_us.load(std::memory_order_relaxed) > kSilencioUs) {
        SDL_ClearAudioStream(salida);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    SustituirSalidaJuego(false);

    REXLOG_INFO("[video] audio WMA Pro: {} paquetes ASF, {} bloques decodificados, {} muestras estéreo", c.paquetes,
                paquetes_decodificados.load(), muestras_salida.load());
  }
};

AudioWmaPro::AudioWmaPro() : e_(std::make_unique<Estado>()) {}
AudioWmaPro::~AudioWmaPro() = default;

void AudioWmaPro::Latido() {
  const int64_t ahora = Estado::AhoraUs();
  const int64_t antes = e_->latido_us.exchange(ahora, std::memory_order_relaxed);
  if (antes && ahora - antes > 250'000) {
    REXLOG_INFO("[video] audio WMA Pro: fotogramas de nuevo tras {} ms", (ahora - antes) / 1000);
  }
}

bool AudioWmaPro::Abrir(const std::string& ruta) {
  auto& e = *e_;
  if (!e.fichero.Abrir(ruta) || !LeerCabecera(e.fichero, ruta, e.c) || e.c.codec_audio == AV_CODEC_ID_NONE) {
    REXLOG_WARN("[video] audio WMA nativo no disponible para '{}'", ruta);
    return false;
  }
  const AVCodec* decoder = avcodec_find_decoder(e.c.codec_audio);
  if (!decoder) {
    REXLOG_ERROR("[video] FFmpeg no tiene el descodificador WMA {}", int(e.c.codec_audio));
    return false;
  }
  e.codec = avcodec_alloc_context3(decoder);
  e.codec->sample_rate = e.c.frecuencia_audio;
  e.codec->channels = e.c.canales_audio;
  e.codec->block_align = e.c.bloque_audio;
  e.codec->bit_rate = e.c.bitrate_audio;
  e.codec->bits_per_coded_sample = e.c.bits_audio;
  e.codec->channel_layout = uint64_t(av_get_default_channel_layout(e.c.canales_audio));
  if (!e.c.extra_audio.empty()) {
    e.codec->extradata = static_cast<uint8_t*>(av_mallocz(e.c.extra_audio.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(e.codec->extradata, e.c.extra_audio.data(), e.c.extra_audio.size());
    e.codec->extradata_size = int(e.c.extra_audio.size());
  }
  if (avcodec_open2(e.codec, decoder, nullptr) < 0) {
    REXLOG_ERROR("[video] no se pudo abrir el descodificador WMA Pro para '{}'", ruta);
    return false;
  }
  e.pkt = av_packet_alloc();
  e.frame = av_frame_alloc();
  SDL_AudioSpec spec{};
  spec.freq = e.c.frecuencia_audio > 0 ? e.c.frecuencia_audio : 48000;  // SDL resamples to the device
  spec.format = SDL_AUDIO_F32LE;
  spec.channels = 2;
  e.salida = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (!e.salida || !SDL_ResumeAudioStreamDevice(e.salida)) {
    REXLOG_ERROR("[video] no se pudo abrir la salida de audio de la cinematica: {}", SDL_GetError());
    return false;
  }
  REXLOG_INFO("[video] audio nativo {} '{}' flujo {}: {} canales a {} Hz, bloque {}, extradata {} bytes",
              decoder->name, ruta, e.c.flujo_audio, e.c.canales_audio, e.c.frecuencia_audio,
              e.c.bloque_audio, e.c.extra_audio.size());
  e.latido_us.store(Estado::AhoraUs(), std::memory_order_relaxed);
  e.hilo = std::thread([estado = e_.get()] { estado->Ejecutar(); });
  return true;
}

// --- DescodificadorWmv3 ------------------------------------------------------------------------------------

struct DescodificadorWmv3::Estado {
  AVCodecContext* codec = nullptr;
  AVPacket* pkt = nullptr;
  AVFrame* frame = nullptr;

  ~Estado() {
    if (frame) {
      av_frame_free(&frame);
    }
    if (pkt) {
      av_packet_free(&pkt);
    }
    if (codec) {
      avcodec_free_context(&codec);
    }
  }
};

DescodificadorWmv3::DescodificadorWmv3() : e_(std::make_unique<Estado>()) {}
DescodificadorWmv3::~DescodificadorWmv3() = default;

bool DescodificadorWmv3::Abrir(const InfoWmv& info) {
  const AVCodec* wmv3 = avcodec_find_decoder(AV_CODEC_ID_WMV3);
  if (!wmv3) {
    REXLOG_ERROR("[video] WMV3 nativo: FFmpeg sin descodificador WMV3");
    return false;
  }
  e_ = std::make_unique<Estado>();
  e_->codec = avcodec_alloc_context3(wmv3);
  e_->codec->width = e_->codec->coded_width = info.ancho;
  e_->codec->height = e_->codec->coded_height = info.alto;
  e_->codec->thread_count = 1;
  // Each frame comes out of the call that decodes it, without reordering. With B frames FFmpeg would otherwise
  // return the previous anchor (one frame late), and the game expects the picture of what it just asked for.
  // Without B frames nothing changes: FFmpeg's VC-1 decoder already has no delay (vc1dec.c, low_delay).
  e_->codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
  if (!info.secuencia.empty()) {
    e_->codec->extradata =
        static_cast<uint8_t*>(av_mallocz(info.secuencia.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(e_->codec->extradata, info.secuencia.data(), info.secuencia.size());
    e_->codec->extradata_size = int(info.secuencia.size());
  }
  if (avcodec_open2(e_->codec, wmv3, nullptr) < 0) {
    REXLOG_ERROR("[video] WMV3 nativo: avcodec_open2 fallo ({}x{})", info.ancho, info.alto);
    e_ = std::make_unique<Estado>();
    return false;
  }
  // With B frames, vc1dec.c computes low_delay again at the first frame from has_b_frames (1 when the sequence
  // allows B frames), and the first I frame did not come out (EAGAIN): the video had no picture until the next I
  // frame, 4 s later. With has_b_frames at 0, low_delay stays at 1 from the first frame.
  e_->codec->has_b_frames = 0;
  e_->pkt = av_packet_alloc();
  e_->frame = av_frame_alloc();
  ancho_ = info.ancho;
  alto_ = info.alto;
  fotogramas_ = 0;
  return true;
}

bool DescodificadorWmv3::Descodificar(const uint8_t* datos, size_t bytes, bool clave, Fotograma& salida) {
  if (!e_->codec || !bytes) {
    return false;
  }
  av_packet_unref(e_->pkt);
  if (av_new_packet(e_->pkt, int(bytes)) < 0) {
    return false;
  }
  std::memcpy(e_->pkt->data, datos, bytes);
  e_->pkt->flags = clave ? AV_PKT_FLAG_KEY : 0;
  int r = avcodec_send_packet(e_->codec, e_->pkt);
  if (r < 0) {
    REXLOG_WARN("[video] WMV3 nativo: avcodec_send_packet {} en el fotograma {} ({} bytes)", r, fotogramas_, bytes);
    return false;
  }
  r = avcodec_receive_frame(e_->codec, e_->frame);
  if (r < 0) {
    REXLOG_WARN("[video] WMV3 nativo: avcodec_receive_frame {} en el fotograma {} ({} bytes)", r, fotogramas_,
                bytes);
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    salida.planos[i] = e_->frame->data[i];
    salida.pasos[i] = e_->frame->linesize[i];
  }
  salida.ancho = e_->frame->width;
  salida.alto = e_->frame->height;
  salida.clave = clave;
  salida.bytes = uint32_t(bytes);
  ++fotogramas_;
  return true;
}

// --- PeliculaWmv (diagnostico) ------------------------------------------------------------------------------

struct PeliculaWmv::Estado {
  FicheroVfs fichero;
  Contenedor c;
  uint64_t siguiente_paquete = 0;
  std::vector<uint8_t> paquete;
  std::map<uint32_t, Objeto> en_curso;
  std::deque<Objeto> listos;
  DescodificadorWmv3 descodificador;

  // Adds a payload to its media object; once complete, the object moves to the ready queue.
  void Carga(uint32_t objeto, uint32_t desplazamiento, uint32_t tamano_objeto, bool clave, const uint8_t* datos,
             uint32_t bytes) {
    auto& o = en_curso[objeto];
    if (o.datos.empty()) {
      o.datos.assign(tamano_objeto, 0);
      o.clave = clave;
    }
    if (uint64_t(desplazamiento) + bytes > o.datos.size()) {
      en_curso.erase(objeto);  // inconsistent piece: the object is dropped
      return;
    }
    std::memcpy(o.datos.data() + desplazamiento, datos, bytes);
    o.recibidos += bytes;
    if (o.recibidos >= o.datos.size()) {
      listos.push_back(std::move(o));
      en_curso.erase(objeto);
    }
  }

  bool LeerPaquete() {
    if (siguiente_paquete >= c.paquetes) {
      return false;
    }
    paquete.resize(c.tamano_paquete);
    if (!fichero.Leer(c.inicio_datos + siguiente_paquete * uint64_t(c.tamano_paquete), paquete.data(),
                      c.tamano_paquete)) {
      return false;
    }
    ++siguiente_paquete;
    const auto& b = paquete;
    size_t p = 0;
    const uint8_t ec = b[p];
    if (ec & 0x80) {
      p += 1 + (ec & 0x0F);
    }
    if (p + 2 > b.size()) {
      return false;
    }
    const uint8_t tipos = b[p];
    const uint8_t propiedades = b[p + 1];
    p += 2;
    uint32_t longitud_paquete = 0, secuencia = 0, relleno = 0;
    if (!LeerVariable(b, p, (tipos >> 5) & 3, longitud_paquete) || !LeerVariable(b, p, (tipos >> 1) & 3, secuencia) ||
        !LeerVariable(b, p, (tipos >> 3) & 3, relleno)) {
      return false;
    }
    p += 6;  // send time and duration
    const bool multiples = tipos & 1;
    int n_cargas = 1;
    int tipo_longitud_carga = 0;
    if (multiples) {
      if (p >= b.size()) {
        return false;
      }
      n_cargas = b[p] & 0x3F;
      tipo_longitud_carga = (b[p] >> 6) & 3;
      ++p;
    }
    const size_t fin = std::min<size_t>(b.size(), (longitud_paquete ? longitud_paquete : c.tamano_paquete)) -
                       std::min<size_t>(relleno, b.size());
    for (int i = 0; i < n_cargas && p < fin; ++i) {
      const uint8_t numero = b[p++];
      const uint32_t flujo = numero & 0x7F;
      const bool clave = numero & 0x80;
      uint32_t objeto = 0, desplazamiento = 0, replicados = 0;
      if (!LeerVariable(b, p, (propiedades >> 4) & 3, objeto) ||
          !LeerVariable(b, p, (propiedades >> 2) & 3, desplazamiento) ||
          !LeerVariable(b, p, propiedades & 3, replicados) || p + replicados > b.size()) {
        return false;
      }
      const size_t pos_replicados = p;
      p += replicados;
      uint32_t bytes = 0;
      if (multiples) {
        if (!LeerVariable(b, p, tipo_longitud_carga, bytes)) {
          return false;
        }
      } else {
        bytes = uint32_t(fin > p ? fin - p : 0);
      }
      if (p + bytes > b.size()) {
        return false;
      }
      if (flujo == c.flujo_video) {
        if (replicados >= 8) {
          Carga(objeto, desplazamiento, Le32(&b[pos_replicados]), clave, &b[p], bytes);
        } else if (replicados == 1) {
          // Compressed payloads: whole objects in a row, each preceded by its size in one byte.
          size_t q = p;
          uint32_t sub = objeto;
          while (q < p + bytes) {
            const uint32_t n = b[q++];
            if (q + n > p + bytes) {
              break;
            }
            Carga(sub++, 0, n, clave, &b[q], n);
            q += n;
          }
        }
      }
      p += bytes;
    }
    return true;
  }
};

PeliculaWmv::PeliculaWmv() : e_(std::make_unique<Estado>()) {}
PeliculaWmv::~PeliculaWmv() = default;

bool PeliculaWmv::Abrir(const std::string& ruta) {
  ruta_ = ruta;
  if (!e_->fichero.Abrir(ruta) || !LeerCabecera(e_->fichero, ruta, e_->c)) {
    return false;
  }
  info_ = e_->c.info;
  if (!e_->descodificador.Abrir(info_)) {
    return false;
  }
  const auto& s = info_.secuencia;
  REXLOG_INFO("[video] WMV3 nativo: '{}' {}x{}, {} paquetes de {} bytes, flujo {}, secuencia {:02X}{:02X}{:02X}{:02X}",
              ruta, info_.ancho, info_.alto, e_->c.paquetes, e_->c.tamano_paquete, e_->c.flujo_video,
              s.size() > 0 ? s[0] : 0, s.size() > 1 ? s[1] : 0, s.size() > 2 ? s[2] : 0, s.size() > 3 ? s[3] : 0);
  return true;
}

bool PeliculaWmv::Siguiente(Fotograma& salida) {
  while (e_->listos.empty()) {
    if (!e_->LeerPaquete()) {
      return false;
    }
  }
  Objeto o = std::move(e_->listos.front());
  e_->listos.pop_front();
  if (!e_->descodificador.Descodificar(o.datos.data(), o.datos.size(), o.clave, salida)) {
    return false;
  }
  ++fotogramas_;
  return true;
}

}  // namespace nfsmw::video_wmv3
