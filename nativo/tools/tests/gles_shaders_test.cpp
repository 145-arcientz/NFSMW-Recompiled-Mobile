// Converts every shader of a GLES library (tools/biblioteca_shaders_gles.mjs) to GLSL ES with
// app/src/nfsmw_gles_shaders.cpp, the same code the device runs, and writes each one as <number>.vert or
// <number>.frag, so they can be compiled with glslang in ES mode (glslang -S vert|frag <file>). It checks
// itself that every module converts and that every texture lands on its register's unit.
//
//   S=sdk/thirdparty
//   clang++ -std=c++20 -O1 -I app/src -I $S/spirv-cross -I $S/xxHash tools/tests/gles_shaders_test.cpp
//     app/src/nfsmw_gles_shaders.cpp app/src/nfsmw_shader_library.cpp $S/spirv-cross/spirv_cross.cpp
//     $S/spirv-cross/spirv_parser.cpp $S/spirv-cross/spirv_cross_parsed_ir.cpp $S/spirv-cross/spirv_cfg.cpp
//     $S/spirv-cross/spirv_glsl.cpp -o gles_shaders_test
//   gles_shaders_test <nfsmw_shaders_gles.nfsp> <output folder> [310|320] [clip_control]
#include "nfsmw_gles_shaders.h"
#include "nfsmw_shader_library.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>

// `layout(location = N) [qualifiers] out` declarations in a converted vertex shader.
uint32_t SalidasDeclaradas(const std::string& fuente) {
  uint32_t n = 0;
  for (size_t i = fuente.find("\nlayout(location = "); i != std::string::npos;
       i = fuente.find("\nlayout(location = ", i + 1)) {
    const size_t fin = fuente.find('\n', i + 1);
    const std::string linea = fuente.substr(i + 1, fin - i - 1);
    if (linea.find(") out ") != std::string::npos || linea.find(" out ") != std::string::npos) ++n;
  }
  return n;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "uso: gles_shaders_test <biblioteca .nfsp> <salida> [310|320] [clip_control]\n");
    return 2;
  }
  nfsmw::gles::OpcionesGlsl opciones;
  if (argc > 3) opciones.version = uint32_t(std::atoi(argv[3]));
  opciones.clip_control = argc > 4 && std::strcmp(argv[4], "clip_control") == 0;

  nfsmw::native::BibliotecaShaders biblioteca;
  try {
    biblioteca.Cargar(std::filesystem::path(argv[1]));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "biblioteca: %s\n", e.what());
    return 1;
  }
  std::filesystem::create_directories(argv[2]);
  const auto escribir = [&](uint32_t n, bool vertices, const std::string& fuente) {
    // With a typical specialization, so the define path is exercised too.
    const auto ruta = std::filesystem::path(argv[2]) / (std::to_string(n) + (vertices ? ".vert" : ".frag"));
    std::ofstream(ruta, std::ios::binary) << nfsmw::gles::ConEspecializacion(fuente, 0x00000302u);
  };
  uint32_t numero = 0, fallos = 0, vertices = 0, samplers = 0, solo_tamano = 0, max_entradas = 0;
  // First the pixel shaders: their input locations are the masks the vertex shaders are tried with.
  std::set<uint64_t> mascaras;
  std::map<uint32_t, std::string> fuentes_ps;
  std::map<uint32_t, uint64_t> mascaras_ps;
  std::map<uint64_t, std::string> pares;  // PS input mask -> a vertex shader converted for it
  for (const auto& shader : biblioteca.shaders()) {
    const uint32_t n = numero++;
    if (shader.vertices) continue;
    const auto glsl = nfsmw::gles::ConvertirAGlsl(shader.spirv, false, opciones);
    if (!glsl.ok) {
      std::fprintf(stderr, "FALLO %u (PS): %s\n", n, glsl.error.c_str());
      ++fallos;
      continue;
    }
    mascaras.insert(glsl.ubicaciones);
    max_entradas = std::max(max_entradas, uint32_t(std::popcount(glsl.ubicaciones)));
    for (const auto& s : glsl.samplers) {
      ++samplers;
      solo_tamano += s.solo_tamano;
      if (s.unidad != s.dimension * nfsmw::gles::kUnidadesPorDimension + s.registro) {
        std::fprintf(stderr, "FALLO %u: unidad %u para el registro %u\n", n, s.unidad, s.registro);
        ++fallos;
      }
    }
    escribir(n, false, glsl.fuente);
    fuentes_ps[n] = glsl.fuente;
    mascaras_ps[n] = glsl.ubicaciones;
  }
  // Each vertex shader with every distinct mask: it must keep exactly the outputs of the mask that it has. The
  // file written is the one with the most outputs.
  numero = 0;
  uint32_t max_salidas = 0, max_salidas_sin_mascara = 0;
  for (const auto& shader : biblioteca.shaders()) {
    const uint32_t n = numero++;
    if (!shader.vertices) continue;
    ++vertices;
    const auto completo = nfsmw::gles::ConvertirAGlsl(shader.spirv, true, opciones);
    if (!completo.ok) {
      std::fprintf(stderr, "FALLO %u (VS): %s\n", n, completo.error.c_str());
      ++fallos;
      continue;
    }
    max_salidas_sin_mascara = std::max(max_salidas_sin_mascara, uint32_t(std::popcount(completo.ubicaciones)));
    if (!completo.samplers.empty()) {
      std::fprintf(stderr, "FALLO %u: un vertex shader con texturas\n", n);
      ++fallos;
    }
    std::string mayor;
    uint32_t salidas_mayor = 0;
    for (uint64_t mascara : mascaras) {
      const auto glsl = nfsmw::gles::ConvertirAGlsl(shader.spirv, true, opciones, mascara);
      if (!glsl.ok || (glsl.ubicaciones & ~mascara) || glsl.ubicaciones != (completo.ubicaciones & mascara)) {
        std::fprintf(stderr, "FALLO %u (VS, mascara %llx): %s\n", n, (unsigned long long)mascara, glsl.error.c_str());
        ++fallos;
        continue;
      }
      const uint32_t salidas = uint32_t(std::popcount(glsl.ubicaciones));
      if (SalidasDeclaradas(glsl.fuente) != salidas) {
        std::fprintf(stderr, "FALLO %u (VS, mascara %llx): declara %u salidas y deberian ser %u\n", n,
                     (unsigned long long)mascara, SalidasDeclaradas(glsl.fuente), salidas);
        ++fallos;
      }
      max_salidas = std::max(max_salidas, salidas);
      // A pair to link: the first vertex shader that writes everything a pixel shader with this mask reads.
      if ((completo.ubicaciones & mascara) == mascara && !pares.count(mascara)) pares[mascara] = glsl.fuente;
      if (mayor.empty() || salidas > salidas_mayor) {
        mayor = glsl.fuente;
        salidas_mayor = salidas;
      }
    }
    if (!mayor.empty()) escribir(n, true, mayor);
  }
  // pares/<n>.frag with pares/<n>.vert, for glslang -l: every pixel shader with a vertex shader matching it.
  std::filesystem::create_directories(std::filesystem::path(argv[2]) / "pares");
  uint32_t sin_pareja = 0;
  for (const auto& [n, fuente] : fuentes_ps) {
    const auto it = pares.find(mascaras_ps[n]);
    if (it == pares.end()) {
      ++sin_pareja;
      continue;
    }
    const auto base = std::filesystem::path(argv[2]) / "pares" / std::to_string(n);
    std::ofstream(base.string() + ".frag", std::ios::binary) << nfsmw::gles::ConEspecializacion(fuente, 0x00000302u);
    std::ofstream(base.string() + ".vert", std::ios::binary) << nfsmw::gles::ConEspecializacion(it->second, 0x00000302u);
  }
  std::printf("parejas para enlazar: %zu (PS sin VS compatible: %u)\n", fuentes_ps.size() - sin_pareja, sin_pareja);
  std::printf("mascaras de PS distintas: %zu; entradas de PS: hasta %u; salidas de VS: hasta %u sin mascara, %u con ella\n",
              mascaras.size(), max_entradas, max_salidas_sin_mascara, max_salidas);
  std::printf("%u shaders (%u VS, %u PS), %u samplers (%u solo tamano), %u fallos\n", numero, vertices,
              numero - vertices, samplers, solo_tamano, fallos);
  return fallos ? 1 : 0;
}
