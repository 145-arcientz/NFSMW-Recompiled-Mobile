/**
 * @file        ui/windowed_app_main_sdl.cpp
 * @brief       Entry point for windowed applications (SDL3 windowing)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#if REX_PLATFORM_ANDROID || REX_PLATFORM_IOS
// PARCHE LOCAL - iOS: SDL owns the real main there too (UIApplicationMain), and calls ours
// as SDL_main once UIKit has started.
#include <SDL3/SDL_main.h>
#endif
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_sdl.h>

#if REX_PLATFORM_IOS
// PARCHE LOCAL - iOS: start-up steps straight to the device log (idevicesyslog), before and
// independently of the engine's logging.
#include <os/log.h>
#define REX_TRAZA_ARRANQUE(texto) os_log(OS_LOG_DEFAULT, "[arranque] %{public}s", texto)
#else
#define REX_TRAZA_ARRANQUE(texto) ((void)0)
#endif

#if REX_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#endif

namespace {

int RunWindowedApp(int argc, char** argv) {
  REX_TRAZA_ARRANQUE("main");
  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();
  REX_TRAZA_ARRANQUE("cvars y log temprano");

  int result;
  {
    rex::ui::SDLWindowedAppContext app_context;
    if (!app_context.Initialize()) {
      REX_TRAZA_ARRANQUE("SDLWindowedAppContext::Initialize fallo");
      return EXIT_FAILURE;
    }
    REX_TRAZA_ARRANQUE("SDL inicializado");

#if REX_PLATFORM_WIN32
    // Apartment-threaded COM for shell dialogs.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
      return EXIT_FAILURE;
    }
#endif

#if REX_PLATFORM_ANDROID
    // Android loads the app and its SDL entry point into the same shared
    // library. App creators register themselves through REX_DEFINE_APP.
    auto app_creator = rex::ui::WindowedApp::GetCreator("nfsmw");
    if (!app_creator) {
      return EXIT_FAILURE;
    }
    std::unique_ptr<rex::ui::WindowedApp> app = app_creator(app_context);
#else
    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);
#endif

    // Match remaining positional args to the app's expected options.
    const auto& option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) {
      parsed[option_names[i]] = remaining[i];
    }
    app->SetParsedArguments(std::move(parsed));

    REX_TRAZA_ARRANQUE("app creada, OnInitialize");
    const bool iniciada = app->OnInitialize();
    REX_TRAZA_ARRANQUE(iniciada ? "OnInitialize bien, bucle de mensajes" : "OnInitialize fallo");
    result = iniciada ? app_context.RunMainMessageLoop() : EXIT_FAILURE;

    app->InvokeOnDestroy();
  }

#if REX_PLATFORM_WIN32
  CoUninitialize();
#endif

  return result;
}

#if REX_PLATFORM_WIN32
// Convert wide argv from CommandLineToArgvW to UTF-8 for cvar::Init.
std::vector<std::string> WideArgsToUtf8(int argc, wchar_t** wargv) {
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    std::wstring wide(wargv[i]);
    if (wide.empty()) {
      args.emplace_back();
      continue;
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr,
                                   0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), size,
                        nullptr, nullptr);
    args.push_back(std::move(utf8));
  }
  return args;
}
#endif

}  // namespace

#if REX_PLATFORM_WIN32

int WINAPI wWinMain(HINSTANCE hinstance, HINSTANCE hinstance_prev, LPWSTR command_line,
                    int show_cmd) {
  (void)hinstance;
  (void)hinstance_prev;
  (void)command_line;
  (void)show_cmd;

  int wargc = 0;
  wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  auto utf8_args = WideArgsToUtf8(wargc, wargv);
  LocalFree(wargv);

  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(utf8_args.size());
  for (auto& s : utf8_args) {
    argv_ptrs.push_back(s.data());
  }
  return RunWindowedApp(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
}

#else

#if REX_PLATFORM_ANDROID
// PARCHE LOCAL - sonda de Vulkan
//
// La app (nativo_android.cpp, en esta misma libreria) la define: si los
// argumentos piden la sonda, la ejecuta y devuelve su resultado; si no,
// devuelve -1 y se arranca el juego como siempre.
extern "C" int NfsmwSondaSiSePide(int argc, char** argv) __attribute__((weak));
#endif

#if REX_PLATFORM_IOS
// PARCHE LOCAL - iOS: the app's launcher (ios/motor/lanzador.mm) shows before the game, in the same
// process, and returns the command line with what was chosen there (argv[0] included); empty to start
// with the usual one.
std::vector<std::string> NfsmwLanzadorIos(int argc, char** argv) __attribute__((weak));
#endif

int main(int argc, char* argv[]) {
#if REX_PLATFORM_ANDROID
  if (NfsmwSondaSiSePide) {
    const int sonda = NfsmwSondaSiSePide(argc, argv);
    if (sonda >= 0) {
      return sonda;
    }
  }
#endif
#if REX_PLATFORM_IOS
  if (NfsmwLanzadorIos) {
    // Static: the cvars may keep pointers into argv.
    static std::vector<std::string> argumentos = NfsmwLanzadorIos(argc, argv);
    if (!argumentos.empty()) {
      static std::vector<char*> punteros;
      for (auto& a : argumentos) {
        punteros.push_back(a.data());
      }
      punteros.push_back(nullptr);
      return RunWindowedApp(static_cast<int>(argumentos.size()), punteros.data());
    }
  }
#endif
  return RunWindowedApp(argc, argv);
}

#endif
