// El anfitrion de la LAN en segundo plano: ver segundo_plano.h.
//
// - El silencio es un WAV en memoria (1 s, 8 kHz, mono) en bucle con AVAudioPlayer: lo mas barato que
//   cuenta como "reproduciendo" para iOS.
// - La sesion: Playback con MixWithOthers, para no cortar ni bajar la musica de otra app. Solo si
//   no es ya Playback: el juego pone la suya (Playback con DuckOthers, audio_ios.mm) y no se le toca.
// - El juego no usa la GPU en segundo plano aunque la app siga viva: su anillo se para en el siguiente
//   cambio de fotograma y su audio se pausa (NfsmwAndroidPausaEnSwap, audio_ios.mm).

#include "segundo_plano.h"

#include "servidor_ea.h"

#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace {

AVAudioPlayer* g_silencio = nil;
bool g_avisos = false;

NSData* WavSilencio() {
  constexpr uint32_t kFrecuencia = 8000, kMuestras = 8000;  // 1 s
  constexpr uint32_t kBytes = kMuestras * 2;                // mono, 16 bits
  NSMutableData* d = [NSMutableData dataWithLength:44 + kBytes];
  uint8_t* p = static_cast<uint8_t*>(d.mutableBytes);
  auto poner32 = [](uint8_t* q, uint32_t v) { std::memcpy(q, &v, 4); };  // little endian (arm64)
  auto poner16 = [](uint8_t* q, uint16_t v) { std::memcpy(q, &v, 2); };
  std::memcpy(p, "RIFF", 4);
  poner32(p + 4, 36 + kBytes);
  std::memcpy(p + 8, "WAVEfmt ", 8);
  poner32(p + 16, 16);
  poner16(p + 20, 1);  // PCM
  poner16(p + 22, 1);  // mono
  poner32(p + 24, kFrecuencia);
  poner32(p + 28, kFrecuencia * 2);
  poner16(p + 32, 2);
  poner16(p + 34, 16);
  std::memcpy(p + 36, "data", 4);
  poner32(p + 40, kBytes);
  return d;  // las muestras ya son ceros
}

void Sonar() {
  AVAudioSession* sesion = AVAudioSession.sharedInstance;
  NSError* error = nil;
  if (![sesion.category isEqualToString:AVAudioSessionCategoryPlayback] &&
      ![sesion setCategory:AVAudioSessionCategoryPlayback
                      mode:AVAudioSessionModeDefault
                   options:AVAudioSessionCategoryOptionMixWithOthers
                     error:&error]) {
    NSLog(@"[segundo plano] categoria de la sesion: %@", error.localizedDescription);
  }
  [sesion setActive:YES error:nil];
  if (!g_silencio) {
    g_silencio = [[AVAudioPlayer alloc] initWithData:WavSilencio() error:&error];
    if (!g_silencio) {
      NSLog(@"[segundo plano] sin reproductor: %@", error.localizedDescription);
      return;
    }
    g_silencio.numberOfLoops = -1;
  }
  if (!g_silencio.isPlaying && ![g_silencio play]) {
    NSLog(@"[segundo plano] el silencio no suena: la app se congelara en segundo plano");
  }
}

void Revivir() {
  // Un poco despues de volver: los hilos del servidor ven sus sockets anulados en su siguiente vuelta.
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(1.5 * NSEC_PER_SEC)),
                 dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                   std::string error;
                   if (!nfsmw::servidor::Revivir(&error) && !error.empty()) {
                     NSLog(@"[segundo plano] el servidor no se vuelve a abrir: %s", error.c_str());
                   }
                 });
}

void Avisos() {
  if (g_avisos) return;
  g_avisos = true;
  NSNotificationCenter* centro = NSNotificationCenter.defaultCenter;
  // Una llamada o Siri paran el reproductor; al acabar, otra vez.
  [centro addObserverForName:AVAudioSessionInterruptionNotification
                      object:nil
                       queue:NSOperationQueue.mainQueue
                  usingBlock:^(NSNotification* aviso) {
                    const auto tipo = AVAudioSessionInterruptionType(
                        [aviso.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue]);
                    if (tipo == AVAudioSessionInterruptionTypeEnded && g_silencio) Sonar();
                  }];
  [centro addObserverForName:UIApplicationDidBecomeActiveNotification
                      object:nil
                       queue:NSOperationQueue.mainQueue
                  usingBlock:^(NSNotification*) {
                    if (g_silencio) Sonar();
                    Revivir();
                  }];
}

}  // namespace

namespace nfsmw::segundo_plano {

void Mantener(bool si) {
  if (si) {
    Avisos();
    Sonar();
  } else if (g_silencio) {
    [g_silencio stop];
    g_silencio = nil;
  }
}

}  // namespace nfsmw::segundo_plano
