// nfsmw - captures of the game image for the native renderer tests. With
// nfsmw_captura_cada_s > 0 it saves a PNG every N seconds in capturas/ next
// to the executable, in native and in emulation mode, to compare the two
// without looking at the screen.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

namespace rex::ui {
class Presenter;
}

namespace nfsmw::captura {

// Starts the capture thread if the cvar asks for it. The presenter is requested
// on every capture: it may not exist yet at startup.
void Arrancar(std::function<rex::ui::Presenter*()> obtener_presentador);

// Stops the thread (idempotent).
void Parar();

// Writes an R8G8B8A8 image (alpha ignored, rows of ancho * 4 bytes) as an uncompressed PNG.
// For diagnostics (nfsmw_volcado in nfsmw_nativo_destinos.cpp).
bool GuardarPng(const std::filesystem::path& ruta, const uint8_t* rgba, uint32_t ancho, uint32_t alto);

}  // namespace nfsmw::captura
