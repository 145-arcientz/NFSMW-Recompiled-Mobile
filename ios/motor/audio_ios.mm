// La salida de audio de iOS: el driver AAudio de la app de Android
// (android/app/src/main/cpp/audio/aaudio_driver.cpp) con AVAudioEngine, en vez del driver de SDL
// del SDK, que en iOS va por AudioQueue: una cola de buferes grandes, con su propio ritmo, lejos de
// lo que pide el juego.
//
// AVAudioEngine con un AVAudioSourceNode es la salida de baja latencia de Apple (RemoteIO por
// debajo): su bloque de render corre en el hilo de tiempo real de E/S y pide los fotogramas justo
// cuando el altavoz los necesita. Lo demas, como en Android:
//   - un anillo sin cerrojo entre SubmitFrame (hilo de audio del juego) y el render (tiempo real),
//     que no puede esperar a nadie;
//   - el semaforo del juego se suelta segun consume el altavoz, un permiso por cada 256 fotogramas
//     (un bloque del juego): eso clava el audio del juego a 48 kHz, sin bombas ni relojes propios;
//   - el pliegue de 5.1 a estereo, el volumen (audio_ganancia_pct), el limitador del SDK y el
//     silencio mientras suena la pista de una pelicula.
// El bufer de E/S se pide de un bloque del juego (256 fotogramas, 5,33 ms; nfsmw_audio_ios_bufer).
//
// Lo propio de iOS:
//   - La sesion de audio (AVAudioSession) la pone SDL, como con su driver (categoria Playback): la
//     pista de las peliculas sigue yendo por SDL (nfsmw_video_wmv3.cpp), y SDL, si encuentra la
//     sesion distinta de como la deja el, para todo para cambiarla; y al cerrar su ultimo
//     dispositivo la desactiva, lo que pararia tambien este motor. Por eso aqui se abre primero un
//     dispositivo de SDL en pausa, que configura la sesion y se queda abierto todo el juego: las
//     peliculas abren sus flujos sobre el mismo dispositivo y SDL ya no vuelve a tocar la sesion.
//     Despues solo se le pide el bufer de E/S y los 48 kHz.
//   - Interrupciones (una llamada, Siri), cambios de ruta (cascos, Bluetooth) y el reinicio de los
//     servicios de audio: el motor se para solo y se vuelve a arrancar.
//   - En segundo plano se pausa (como el AAudio en Android, con los avisos de SDL).

#import <AVFoundation/AVFoundation.h>

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>

#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/downmix.h>
#include <rex/audio/output_limiter.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>

REXCVAR_DEFINE_BOOL(nfsmw_audio_ios, true, "Audio",
                    "iOS: sacar el sonido por AVAudioEngine (el driver de baja latencia de NFSMW "
                    "Recompiled) en vez de por el de SDL del SDK")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_audio_ios_bufer, 256, "Audio",
                     "iOS: el bufer de E/S que se le pide al sistema, en fotogramas a 48 kHz (256 = "
                     "5,33 ms, un bloque del juego). iOS lo redondea a lo que admite la ruta")
    .range(64, 4096)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DECLARE(int32_t, audio_ganancia_pct);
REXCVAR_DECLARE(bool, audio_mute);

namespace nfsmw::audio_ios {

// Las macros X_STATUS_* lo usan sin su espacio de nombres.
using rex::X_STATUS;

namespace {

constexpr double kFrecuencia = 48000;

// Pesos del plegado de 5.1 a estereo: los del driver AAudio.
constexpr float kCentro = 0.7071f;
constexpr float kTraseros = 0.7071f;
constexpr float kGraves = 0.5f;

inline float LeerFloatBE(uint32_t crudo) {
  const uint32_t le = __builtin_bswap32(crudo);
  float f;
  std::memcpy(&f, &le, sizeof(float));
  return f;
}

}  // namespace

class Driver final : public rex::audio::AudioDriver {
 public:
  Driver(rex::memory::Memory* memory, rex::thread::Semaphore* semaphore)
      : AudioDriver(memory), semaphore_(semaphore), anillo_(new float[kCapacidad]()),
        cola_(dispatch_queue_create("nfsmw.audio", DISPATCH_QUEUE_SERIAL)) {}

  ~Driver() override { Shutdown(); }

  bool Initialize();
  void Shutdown();
  void SubmitFrame(uint32_t samples_ptr) override;

  void Pausar();
  void Reanudar();

 private:
  static bool SDLCALL VigilarEventos(void* userdata, SDL_Event* event);

  // categoria: ponerla aqui (sin SDL); con SDL ya la ha puesto el.
  bool ConfigurarSesion(bool categoria);
  bool CrearMotor();
  // En su cola: vuelve a poner en marcha la sesion y el motor, si se pararon.
  void Rearrancar(const char* porque);
  void Informar();

  // El render, en el hilo de tiempo real de E/S: nada de cerrojos, ni de Objective-C, ni de reservar
  // memoria.
  OSStatus Render(AVAudioFrameCount fotogramas, AudioBufferList* salida);

  // Un bloque del juego: 256 muestras por canal, ya plegadas a estereo, intercaladas.
  static constexpr size_t kMuestrasPorCanal = 256;
  static constexpr size_t kCanales = 2;
  static constexpr size_t kFloatsPorBloque = kMuestrasPorCanal * kCanales;
  // Los 64 bloques de AudioSystem::kMaximumQueuedFrames: con el semaforo nunca hay mas en vuelo.
  static constexpr size_t kBloquesAnillo = 64;
  static constexpr size_t kCapacidad = kBloquesAnillo * kFloatsPorBloque;  // 32768 floats, 128 KB
  static_assert((kCapacidad & (kCapacidad - 1)) == 0, "la capacidad tiene que ser potencia de 2");
  static constexpr size_t kMascara = kCapacidad - 1;

  rex::thread::Semaphore* semaphore_ = nullptr;
  std::unique_ptr<float[]> anillo_;
  alignas(64) std::atomic<size_t> escritura_{0};  // solo lo toca el productor
  alignas(64) std::atomic<size_t> lectura_{0};    // solo lo toca el render
  // Fotogramas consumidos que aun no llegan a un bloque entero. Solo lo toca el render.
  size_t consumidos_ = 0;

  // Para el registro (los escribe el render, los lee el productor).
  std::atomic<uint64_t> pedidos_{0};
  std::atomic<uint64_t> mudos_{0};
  std::chrono::steady_clock::time_point ultimoInforme_{};
  uint64_t pedidosAntes_ = 0;
  uint64_t mudosAntes_ = 0;

  // Solo el productor (SubmitFrame): el estado del limitador del SDK.
  float limitador_ = 1.0f;

  std::atomic<bool> en_marcha_{false};
  std::atomic<bool> pausado_{false};
  bool sdl_audio_ = false;
  SDL_AudioDeviceID sdl_retenido_ = 0;

  // El camino de control (arrancar, parar, rearrancar) va en su cola; el render no la toca nunca.
  dispatch_queue_t cola_;
  AVAudioEngine* motor_ = nil;
  AVAudioSourceNode* fuente_ = nil;
  NSMutableArray<id>* observadores_ = nil;
};

bool Driver::ConfigurarSesion(bool categoria) {
  AVAudioSession* sesion = AVAudioSession.sharedInstance;
  NSError* error = nil;
  // Lo mismo que pone SDL con SDL_HINT_AUDIO_CATEGORY=playback (UpdateAudioSession).
  if (categoria && ![sesion setCategory:AVAudioSessionCategoryPlayback
                                   mode:AVAudioSessionModeDefault
                                options:AVAudioSessionCategoryOptionDuckOthers
                                  error:&error]) {
    REXLOG_WARN("[audio ios] categoria de la sesion: {}", error.localizedDescription.UTF8String);
  }
  [sesion setPreferredSampleRate:kFrecuencia error:nil];
  [sesion setPreferredIOBufferDuration:REXCVAR_GET(nfsmw_audio_ios_bufer) / kFrecuencia error:nil];
  if (![sesion setActive:YES error:&error]) {
    REXLOG_ERROR("[audio ios] la sesion de audio no se activa: {}", error.localizedDescription.UTF8String);
    return false;
  }
  return true;
}

bool Driver::CrearMotor() {
  motor_ = [[AVAudioEngine alloc] init];
  AVAudioFormat* formato = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:kFrecuencia channels:kCanales];
  Driver* yo = this;
  fuente_ = [[AVAudioSourceNode alloc]
      initWithFormat:formato
         renderBlock:^OSStatus(BOOL*, const AudioTimeStamp*, AVAudioFrameCount fotogramas, AudioBufferList* salida) {
           return yo->Render(fotogramas, salida);
         }];
  [motor_ attachNode:fuente_];
  // Directo al mezclador principal, que va a la salida: si la ruta no va a 48 kHz, el convierte.
  [motor_ connect:fuente_ to:motor_.mainMixerNode format:formato];
  [motor_ prepare];
  NSError* error = nil;
  if (![motor_ startAndReturnError:&error]) {
    REXLOG_ERROR("[audio ios] AVAudioEngine no arranca: {}", error.localizedDescription.UTF8String);
    return false;
  }
  return true;
}

void Driver::Informar() {
  AVAudioSession* sesion = AVAudioSession.sharedInstance;
  AVAudioSessionPortDescription* salida = sesion.currentRoute.outputs.firstObject;
  REXLOG_INFO("[audio ios] AVAudioEngine en marcha: {:.0f} Hz, E/S de {:.2f} ms ({:.0f} fotogramas), latencia "
              "de salida {:.1f} ms, salida '{}'",
              sesion.sampleRate, sesion.IOBufferDuration * 1000.0, sesion.IOBufferDuration * sesion.sampleRate,
              sesion.outputLatency * 1000.0, salida ? salida.portName.UTF8String : "?");
}

bool Driver::Initialize() {
  rex::audio::SetOutputGain(float(REXCVAR_GET(audio_ganancia_pct)) / 100.0f);
  // La pista de las peliculas va por SDL y lo da por hecho.
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
  sdl_audio_ = SDL_InitSubSystem(SDL_INIT_AUDIO);
  if (!sdl_audio_) {
    REXLOG_WARN("[audio ios] SDL audio no inicia ({}): las peliculas iran sin sonido", SDL_GetError());
  }

  // Abierto en pausa todo el juego, y antes que el motor: SDL pone la sesion como la quiere y, con su
  // dispositivo siempre abierto, ni la cambia ni la desactiva por una pelicula.
  if (sdl_audio_) {
    // A 48 kHz: SDL le pide a la sesion la frecuencia de su dispositivo (44,1 kHz si no se le dice), y
    // la del juego es esta. Las peliculas convierten a ella en su flujo.
    const SDL_AudioSpec formato = {SDL_AUDIO_F32, int(kCanales), int(kFrecuencia)};
    sdl_retenido_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &formato);
    if (sdl_retenido_) {
      SDL_PauseAudioDevice(sdl_retenido_);
    } else {
      REXLOG_WARN("[audio ios] sin dispositivo de SDL retenido ({}): tras una pelicula puede hacer falta "
                  "rearrancar",
                  SDL_GetError());
    }
  }
  __block bool bien = false;
  const bool categoria = sdl_retenido_ == 0;
  dispatch_sync(cola_, ^{
    bien = ConfigurarSesion(categoria) && CrearMotor();
  });
  if (!bien) {
    return false;
  }
  Informar();

  en_marcha_.store(true, std::memory_order_release);
  SDL_AddEventWatch(VigilarEventos, this);

  // El motor se para solo con una interrupcion o un cambio de ruta: se vuelve a arrancar.
  Driver* yo = this;
  dispatch_queue_t cola = cola_;
  NSNotificationCenter* centro = NSNotificationCenter.defaultCenter;
  observadores_ = [NSMutableArray array];
  // De cualquier motor: tras reiniciarse los servicios de audio, el motor es otro.
  [observadores_ addObject:[centro addObserverForName:AVAudioEngineConfigurationChangeNotification
                                               object:nil
                                                queue:nil
                                           usingBlock:^(NSNotification*) {
                                             dispatch_async(cola, ^{
                                               yo->Rearrancar("cambio de ruta o de formato");
                                             });
                                           }]];
  [observadores_ addObject:[centro addObserverForName:AVAudioSessionInterruptionNotification
                                               object:AVAudioSession.sharedInstance
                                                queue:nil
                                           usingBlock:^(NSNotification* aviso) {
                                             const auto tipo = AVAudioSessionInterruptionType(
                                                 [aviso.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue]);
                                             if (tipo == AVAudioSessionInterruptionTypeEnded) {
                                               dispatch_async(cola, ^{
                                                 yo->Rearrancar("fin de una interrupcion");
                                               });
                                             }
                                           }]];
  [observadores_ addObject:[centro addObserverForName:AVAudioSessionMediaServicesWereResetNotification
                                               object:AVAudioSession.sharedInstance
                                                queue:nil
                                           usingBlock:^(NSNotification*) {
                                             dispatch_async(cola, ^{
                                               // Los servicios de audio se reiniciaron: todo lo
                                               // anterior ya no vale.
                                               [yo->motor_ stop];
                                               yo->motor_ = nil;
                                               yo->fuente_ = nil;
                                               if (yo->en_marcha_.load() && yo->ConfigurarSesion(yo->sdl_retenido_ == 0) &&
                                                   yo->CrearMotor()) {
                                                 yo->Informar();
                                               }
                                             });
                                           }]];
  return true;
}

void Driver::Rearrancar(const char* porque) {
  if (!en_marcha_.load(std::memory_order_acquire) || pausado_.load() || !motor_) {
    return;
  }
  if (motor_.isRunning) {
    REXLOG_INFO("[audio ios] {}: sigue en marcha", porque);
    return;
  }
  [AVAudioSession.sharedInstance setActive:YES error:nil];
  NSError* error = nil;
  if ([motor_ startAndReturnError:&error]) {
    REXLOG_INFO("[audio ios] {}: otra vez en marcha", porque);
    Informar();
  } else {
    REXLOG_WARN("[audio ios] {}: no rearranca: {}", porque, error.localizedDescription.UTF8String);
  }
}

void Driver::Pausar() {
  pausado_.store(true);
  dispatch_async(cola_, ^{
    [motor_ pause];
    REXLOG_INFO("[audio ios] pausa");
  });
}

void Driver::Reanudar() {
  pausado_.store(false);
  dispatch_async(cola_, ^{
    Rearrancar("vuelta de segundo plano");
  });
}

bool SDLCALL Driver::VigilarEventos(void* userdata, SDL_Event* event) {
  auto* driver = static_cast<Driver*>(userdata);
  if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
    driver->Pausar();
  } else if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
    driver->Reanudar();
  }
  return true;
}

// Tambien cuando Initialize no llego al final: lo que si se abrio se cierra.
void Driver::Shutdown() {
  const bool estaba = en_marcha_.exchange(false, std::memory_order_acq_rel);
  if (estaba) {
    SDL_RemoveEventWatch(VigilarEventos, this);
    for (id o in observadores_) [NSNotificationCenter.defaultCenter removeObserver:o];
    observadores_ = nil;
  }
  // Lo que quedara en la cola, antes de soltar el motor.
  dispatch_sync(cola_, ^{
    [motor_ stop];
    motor_ = nil;
    fuente_ = nil;
  });
  // Despertar a quien espere en el semaforo del audio.
  if (estaba && semaphore_) {
    semaphore_->Release(16, nullptr);
  }
  if (sdl_retenido_) {
    SDL_CloseAudioDevice(sdl_retenido_);
    sdl_retenido_ = 0;
  }
  if (sdl_audio_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_audio_ = false;
  }
}

void Driver::SubmitFrame(uint32_t samples_ptr) {
  if (!en_marcha_.load(std::memory_order_relaxed)) {
    return;
  }
  // El fotograma de audio de la Xbox 360 vive en memoria virtual del invitado.
  uint8_t* datos = memory_ ? memory_->TranslateVirtual<uint8_t*>(samples_ptr) : nullptr;
  if (!datos && memory_) {
    datos = TranslatePhysical(samples_ptr);
  }
  if (!datos) {
    return;
  }

  // Sin hueco para un bloque entero: se descarta el NUEVO. El productor no toca nunca el indice de
  // lectura. Con el anillo del tamano de kMaximumQueuedFrames no deberia pasar.
  const size_t w = escritura_.load(std::memory_order_relaxed);
  const size_t r = lectura_.load(std::memory_order_acquire);
  if (kCapacidad - (w - r) < kFloatsPorBloque) {
    return;
  }

  // PLANAR, 6 canales de 256 muestras (6144 bytes), en big-endian: 0=FL, 1=FR, 2=C, 3=LFE, 4=BL, 5=BR.
  const auto* src = reinterpret_cast<const uint32_t*>(datos);
  const uint32_t* fl = src + 0 * kMuestrasPorCanal;
  const uint32_t* fr = src + 1 * kMuestrasPorCanal;
  const uint32_t* c = src + 2 * kMuestrasPorCanal;
  const uint32_t* lfe = src + 3 * kMuestrasPorCanal;
  const uint32_t* bl = src + 4 * kMuestrasPorCanal;
  const uint32_t* br = src + 5 * kMuestrasPorCanal;

  // Un bloque son 512 floats y la capacidad es multiplo de 512: nunca cruza el final del anillo.
  float* dst = anillo_.get() + (w & kMascara);
  if (REXCVAR_GET(audio_mute) || rex::audio::IsGameOutputSuppressed()) {
    // Suena la pista de una pelicula (o se ha callado el juego): el bloque se publica igual, que el
    // ritmo lo sigue marcando el altavoz.
    std::memset(dst, 0, kFloatsPorBloque * sizeof(float));
  } else {
    // Sin recortar: el limitador necesita ver los picos de verdad.
    const float ganancia = rex::audio::GetOutputGain();
    for (size_t i = 0; i < kMuestrasPorCanal; ++i) {
      const float medio = LeerFloatBE(c[i]) * kCentro + LeerFloatBE(lfe[i]) * kGraves;
      dst[i * 2 + 0] = (LeerFloatBE(fl[i]) + medio + LeerFloatBE(bl[i]) * kTraseros) * ganancia;
      dst[i * 2 + 1] = (LeerFloatBE(fr[i]) + medio + LeerFloatBE(br[i]) * kTraseros) * ganancia;
    }
    rex::audio::LimitOutput(dst, kMuestrasPorCanal, kCanales, int(kFrecuencia), limitador_);
  }
  // Publicar el bloque: el release hace que el render vea las muestras antes que el indice nuevo.
  escritura_.store(w + kFloatsPorBloque, std::memory_order_release);

  // Cada 10 s, al registro, desde aqui (el render no puede escribir): lo que pidio el altavoz y lo que
  // se quedo sin datos.
  const auto ahora = std::chrono::steady_clock::now();
  if (ahora - ultimoInforme_ >= std::chrono::seconds(10)) {
    const uint64_t pedidos = pedidos_.load(std::memory_order_relaxed);
    const uint64_t mudos = mudos_.load(std::memory_order_relaxed);
    if (ultimoInforme_.time_since_epoch().count() != 0) {
      const uint64_t p = pedidos - pedidosAntes_;
      const uint64_t m = mudos - mudosAntes_;
      REXLOG_INFO("[audio ios] en 10 s: {} fotogramas pedidos, {} de silencio por falta de datos ({:.2f} %), "
                  "{} bloques en el anillo",
                  p, m, p ? 100.0 * double(m) / double(p) : 0.0, (w + kFloatsPorBloque - r) / kFloatsPorBloque);
    }
    pedidosAntes_ = pedidos;
    mudosAntes_ = mudos;
    ultimoInforme_ = ahora;
  }
  // El semaforo NO se suelta aqui: lo suelta el render segun consume el altavoz.
}

OSStatus Driver::Render(AVAudioFrameCount fotogramas, AudioBufferList* salida) {
  const size_t pedidos = size_t(fotogramas);
  const size_t r = lectura_.load(std::memory_order_relaxed);
  const size_t w = escritura_.load(std::memory_order_acquire);
  const size_t hay = std::min((w - r) / kCanales, pedidos);
  const float* anillo = anillo_.get();
  if (salida->mNumberBuffers >= 2) {
    // El formato estandar: un bufer por canal.
    auto* izq = static_cast<float*>(salida->mBuffers[0].mData);
    auto* der = static_cast<float*>(salida->mBuffers[1].mData);
    for (size_t i = 0; i < hay; ++i) {
      // r es par y la capacidad tambien: el par de un fotograma nunca se parte.
      const size_t p = (r + i * kCanales) & kMascara;
      izq[i] = anillo[p];
      der[i] = anillo[p + 1];
    }
    std::fill(izq + hay, izq + pedidos, 0.0f);
    std::fill(der + hay, der + pedidos, 0.0f);
  } else if (salida->mNumberBuffers == 1) {
    auto* out = static_cast<float*>(salida->mBuffers[0].mData);
    const size_t n = hay * kCanales;
    const size_t pos = r & kMascara;
    const size_t primero = std::min(n, kCapacidad - pos);
    std::memcpy(out, anillo + pos, primero * sizeof(float));
    if (n > primero) std::memcpy(out + primero, anillo, (n - primero) * sizeof(float));
    std::fill(out + n, out + pedidos * kCanales, 0.0f);
  }
  lectura_.store(r + hay * kCanales, std::memory_order_release);
  pedidos_.fetch_add(pedidos, std::memory_order_relaxed);
  if (hay < pedidos) mudos_.fetch_add(pedidos - hay, std::memory_order_relaxed);

  // Un permiso por cada 256 fotogramas que consume el altavoz, haya dato o no: eso es lo que clava el
  // hilo de audio del juego a 48 kHz (187,5 bloques/s).
  if (semaphore_) {
    consumidos_ += pedidos;
    const size_t soltar = consumidos_ / kMuestrasPorCanal;
    if (soltar > 0) {
      consumidos_ -= soltar * kMuestrasPorCanal;
      semaphore_->Release(int32_t(soltar), nullptr);
    }
  }
  return noErr;
}

class Sistema final : public rex::audio::AudioSystem {
 public:
  explicit Sistema(rex::runtime::FunctionDispatcher* function_dispatcher) : AudioSystem(function_dispatcher) {}

  static bool IsAvailable() { return true; }
  static std::unique_ptr<rex::audio::AudioSystem> Create(rex::runtime::FunctionDispatcher* function_dispatcher) {
    return std::make_unique<Sistema>(function_dispatcher);
  }

  X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore, rex::audio::AudioDriver** out_driver) override {
    auto driver = std::make_unique<Driver>(memory_, semaphore);
    if (driver->Initialize()) {
      *out_driver = driver.release();
      return X_STATUS_SUCCESS;
    }
    driver.reset();
    // Mejor el sonido de SDL que ninguno.
    REXLOG_ERROR("[audio ios] el driver del cliente {} no arranca: va por el de SDL", index);
    auto sdl = std::make_unique<rex::audio::sdl::SDLAudioDriver>(memory_, semaphore);
    if (!sdl->Initialize()) {
      sdl->Shutdown();
      return X_STATUS_UNSUCCESSFUL;
    }
    *out_driver = sdl.release();
    return X_STATUS_SUCCESS;
  }

  void DestroyDriver(rex::audio::AudioDriver* driver) override {
    // El de SDL se cierra como lo hace su sistema (SDLAudioSystem::DestroyDriver).
    if (auto* sdl = dynamic_cast<rex::audio::sdl::SDLAudioDriver*>(driver)) {
      sdl->Shutdown();
    }
    delete driver;
  }
};

}  // namespace nfsmw::audio_ios

// La llama NfsmwApp::OnPreSetup (nativo/app/src/nfsmw_app.h), con la factoria de audio por defecto
// (SDL) ya puesta. El nombre es el de la app de Android, que hace lo mismo con su AAudio.
void NfsmwAndroidAudio(rex::RuntimeConfig& config) {
  if (REXCVAR_GET(nfsmw_audio_ios)) {
    config.audio_factory = REX_AUDIO_BACKEND(nfsmw::audio_ios::Sistema);
  }
}
