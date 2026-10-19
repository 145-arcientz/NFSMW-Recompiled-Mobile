// nfsmw - OpenGL ES backend: the library's SPIR-V as GLSL ES (see the header).
#include "nfsmw_gles_shaders.h"

#include <spirv_glsl.hpp>

#include <bit>
#include <cstdio>
#include <exception>
#include <string_view>
#include <utility>

namespace nfsmw::gles {
namespace {

// Name given by tools/biblioteca_shaders_gles.mjs to each texture: g_Tex2D_<r>, g_Tex3D_<r>, g_TexCube_<r>.
bool LeerTextura(std::string_view nombre, uint32_t& dimension, uint32_t& registro) {
  static constexpr std::pair<std::string_view, uint32_t> kPrefijos[] = {
      {"g_Tex2D_", kDimension2D}, {"g_Tex3D_", kDimension3D}, {"g_TexCube_", kDimensionCubo}};
  for (const auto& [prefijo, d] : kPrefijos) {
    if (nombre.substr(0, prefijo.size()) != prefijo) continue;
    uint32_t r = 0;
    const auto resto = nombre.substr(prefijo.size());
    if (resto.empty() || resto.size() > 2) return false;
    for (char c : resto) {
      if (c < '0' || c > '9') return false;
      r = r * 10 + uint32_t(c - '0');
    }
    if (r >= kUnidadesPorDimension) return false;
    dimension = d;
    registro = r;
    return true;
  }
  return false;
}

/*
 * Turns the vertex shader outputs at those locations into plain globals: what is written to them is then dead
 * and the driver drops it. SPIRV-Cross has mask_stage_output_by_location, but its GLSL backend ignores it (only
 * MSL and HLSL use it), so this is done on the text: each output is one line of the form
 * `layout(location = N) [flat|smooth|centroid|noperspective|invariant ]* out <type> <name>;`.
 * Returns how many declarations it changed.
 */
uint32_t QuitarSalidas(std::string& fuente, uint64_t ubicaciones) {
  static constexpr std::string_view kCalificadores[] = {"flat ", "smooth ", "centroid ", "noperspective ", "invariant "};
  uint32_t cambiadas = 0;
  for (uint32_t ubicacion = 0; ubicacion < 64; ++ubicacion) {
    if (!(ubicaciones >> ubicacion & 1)) continue;
    const std::string prefijo = "layout(location = " + std::to_string(ubicacion) + ") ";
    for (size_t inicio = 0; inicio < fuente.size();) {
      size_t fin = fuente.find('\n', inicio);
      if (fin == std::string::npos) fin = fuente.size();
      std::string_view linea(fuente.data() + inicio, fin - inicio);
      if (linea.substr(0, prefijo.size()) == prefijo) {
        std::string_view resto = linea.substr(prefijo.size());
        for (bool otro = true; otro;) {
          otro = false;
          for (const auto& c : kCalificadores) {
            if (resto.substr(0, c.size()) == c) {
              resto.remove_prefix(c.size());
              otro = true;
            }
          }
        }
        if (resto.substr(0, 4) == "out ") {
          const std::string declaracion(resto.substr(4));
          fuente.replace(inicio, fin - inicio, declaracion);
          ++cambiadas;
          break;
        }
      }
      inicio = fin + 1;
    }
  }
  return cambiadas;
}

}  // namespace

ShaderGlsl ConvertirAGlsl(std::span<const uint32_t> spirv, bool vertices, const OpcionesGlsl& opciones,
                          uint64_t salidas_vs) {
  ShaderGlsl resultado;
  try {
    spirv_cross::CompilerGLSL compilador(spirv.data(), spirv.size());
    // Only what the entry point really uses: the translator declares every register.
    const auto activas = compilador.get_active_interface_variables();
    compilador.set_enabled_interface_variables(activas);

    spirv_cross::CompilerGLSL::Options o;
    o.version = opciones.version;
    o.es = true;
    o.vulkan_semantics = false;
    o.enable_420pack_extension = false;
    o.emit_uniform_buffer_as_plain_uniforms = false;
    o.fragment.default_float_precision = spirv_cross::CompilerGLSL::Options::Highp;
    o.fragment.default_int_precision = spirv_cross::CompilerGLSL::Options::Highp;
    o.vertex.fixup_clipspace = vertices && !opciones.clip_control;
    o.vertex.flip_vert_y = false;
    compilador.set_common_options(o);

    const auto recursos = compilador.get_shader_resources(activas);
    for (const auto& ubo : recursos.uniform_buffers) {
      const uint32_t binding = compilador.get_decoration(ubo.id, spv::DecorationBinding);
      const uint32_t set = compilador.get_decoration(ubo.id, spv::DecorationDescriptorSet);
      if (set != 4 || binding > kBloqueCompartidas) {
        resultado.error = "bloque uniforme inesperado: " + ubo.name;
        return resultado;
      }
      compilador.unset_decoration(ubo.id, spv::DecorationDescriptorSet);
    }
    if (!recursos.push_constant_buffers.empty() || !recursos.storage_buffers.empty() ||
        !recursos.storage_images.empty()) {
      resultado.error = "el modulo usa push constants, storage buffers o imagenes de almacenamiento";
      return resultado;
    }

    uint64_t quitar = 0;
    for (const auto& variable : vertices ? recursos.stage_outputs : recursos.stage_inputs) {
      if (!compilador.has_decoration(variable.id, spv::DecorationLocation)) continue;
      const uint32_t ubicacion = compilador.get_decoration(variable.id, spv::DecorationLocation);
      if (ubicacion >= 64) {
        resultado.error = "ubicacion de interfaz fuera de rango: " + variable.name;
        return resultado;
      }
      if (vertices && !(salidas_vs >> ubicacion & 1)) {
        quitar |= uint64_t(1) << ubicacion;
        continue;
      }
      resultado.ubicaciones |= uint64_t(1) << ubicacion;
    }

    // Images only queried for size (textureSize) need a sampler to be combined with.
    compilador.build_dummy_sampler_for_combined_images();
    compilador.build_combined_image_samplers();
    for (const auto& combinado : compilador.get_combined_image_samplers()) {
      const std::string& imagen = compilador.get_name(combinado.image_id);
      SamplerGlsl s;
      if (!LeerTextura(imagen, s.dimension, s.registro)) {
        resultado.error = "textura sin registro: " + imagen;
        return resultado;
      }
      const std::string& sampler = compilador.get_name(combinado.sampler_id);
      s.solo_tamano = sampler.empty() || sampler.rfind("SPIRV_Cross_Dummy", 0) == 0;
      if (!s.solo_tamano && sampler != "g_Sampler_" + std::to_string(s.registro)) {
        // The translator always pairs a register's texture with that register's sampler: anything else would
        // need two sampler objects on one texture unit.
        resultado.error = "textura " + imagen + " con el sampler de otro registro: " + sampler;
        return resultado;
      }
      s.unidad = s.dimension * kUnidadesPorDimension + s.registro;
      compilador.set_name(combinado.combined_id, imagen + (s.solo_tamano ? "_tam" : ""));
      compilador.set_decoration(combinado.combined_id, spv::DecorationBinding, s.unidad);
      resultado.samplers.push_back(s);
    }

    resultado.fuente = compilador.compile();
    if (QuitarSalidas(resultado.fuente, quitar) != uint32_t(std::popcount(quitar))) {
      resultado.error = "no se encontraron todas las declaraciones de salida que habia que quitar";
      return resultado;
    }
    resultado.ok = true;
  } catch (const std::exception& e) {
    resultado.error = e.what();
  }
  return resultado;
}

std::string ConEspecializacion(const std::string& fuente, uint32_t especializacion) {
  // #version has to stay the first line.
  const size_t fin = fuente.find('\n');
  if (fin == std::string::npos) return fuente;
  char definicion[64];
  std::snprintf(definicion, sizeof(definicion), "#define SPIRV_CROSS_CONSTANT_ID_0 %uu\n", especializacion);
  std::string salida;
  salida.reserve(fuente.size() + sizeof(definicion));
  salida.append(fuente, 0, fin + 1);
  salida.append(definicion);
  salida.append(fuente, fin + 1, std::string::npos);
  return salida;
}

}  // namespace nfsmw::gles
