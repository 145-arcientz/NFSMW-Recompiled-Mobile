// nfsmw - native renderer: half precision in the pixel shaders (see the header).

#include "nfsmw_spirv_precision.h"

#include <algorithm>

namespace nfsmw::nativo {
namespace {

// SPIR-V opcodes, decorations and enumerants used here (spirv.hpp).
constexpr uint32_t kOpExtInst = 12;
constexpr uint32_t kOpTypeFloat = 22;
constexpr uint32_t kOpTypeVector = 23;
constexpr uint32_t kOpStore = 62;
constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kOpVectorExtractDynamic = 77;
constexpr uint32_t kOpVectorInsertDynamic = 78;
constexpr uint32_t kOpVectorShuffle = 79;
constexpr uint32_t kOpCompositeConstruct = 80;
constexpr uint32_t kOpCompositeExtract = 81;
constexpr uint32_t kOpCompositeInsert = 82;
constexpr uint32_t kOpCopyObject = 83;
constexpr uint32_t kOpImageSampleImplicitLod = 87;
constexpr uint32_t kOpImageGather = 96;
constexpr uint32_t kOpImageDrefGather = 97;
constexpr uint32_t kOpImageSampleDrefImplicitLod = 89;
constexpr uint32_t kOpImageSampleDrefExplicitLod = 90;
constexpr uint32_t kOpImageSampleProjDrefImplicitLod = 93;
constexpr uint32_t kOpImageSampleProjDrefExplicitLod = 94;
constexpr uint32_t kOpFNegate = 127;
constexpr uint32_t kOpFAdd = 129;
constexpr uint32_t kOpFSub = 131;
constexpr uint32_t kOpFMul = 133;
constexpr uint32_t kOpFDiv = 136;
constexpr uint32_t kOpFRem = 140;
constexpr uint32_t kOpFMod = 141;
constexpr uint32_t kOpVectorTimesScalar = 142;
constexpr uint32_t kOpDot = 148;
constexpr uint32_t kOpSelect = 169;
constexpr uint32_t kOpDPdx = 207;
constexpr uint32_t kOpFwidthCoarse = 215;
constexpr uint32_t kOpPhi = 245;
constexpr uint32_t kOpTypeVoid = 19;
constexpr uint32_t kOpTypeForwardPointer = 39;
constexpr uint32_t kDecorationRelaxedPrecision = 0;
constexpr uint32_t kDecorationBuiltIn = 11;
constexpr uint32_t kBuiltInFragDepth = 22;
constexpr uint32_t kMaxBound = 1u << 22;

bool EsMuestreo(uint32_t op) { return op >= kOpImageSampleImplicitLod && op <= kOpImageDrefGather; }
bool EsDeReferencia(uint32_t op) {
  return op == kOpImageSampleDrefImplicitLod || op == kOpImageSampleDrefExplicitLod ||
         op == kOpImageSampleProjDrefImplicitLod || op == kOpImageSampleProjDrefExplicitLod || op == kOpImageDrefGather;
}

// The value operands (ids, not literals) of an instruction that carries color math forward. Empty for the rest.
void Operandos(const uint32_t* w, uint32_t n, uint32_t op, std::vector<uint32_t>& ids) {
  ids.clear();
  switch (op) {
    case kOpExtInst:  // result type, result, set, instruction, operands...
      for (uint32_t i = 5; i < n; ++i) ids.push_back(w[i]);
      return;
    case kOpPhi:  // (value, parent block) pairs
      for (uint32_t i = 3; i + 1 < n; i += 2) ids.push_back(w[i]);
      return;
    case kOpCompositeExtract:  // composite, then literal indices
    case kOpCopyObject:
      if (n > 3) ids.push_back(w[3]);
      return;
    case kOpCompositeInsert:  // object, composite, literals
    case kOpVectorShuffle:    // vector, vector, literals
      if (n > 4) {
        ids.push_back(w[3]);
        ids.push_back(w[4]);
      }
      return;
    case kOpCompositeConstruct:
      for (uint32_t i = 3; i < n; ++i) ids.push_back(w[i]);
      return;
    case kOpSelect:  // condition, a, b
      if (n > 5) {
        ids.push_back(w[4]);
        ids.push_back(w[5]);
      }
      return;
    case kOpVectorExtractDynamic:
    case kOpVectorInsertDynamic:
      for (uint32_t i = 3; i < n; ++i) ids.push_back(w[i]);
      return;
    default:
      if ((op >= kOpFNegate && op <= kOpDot && op != 128 && op != 130 && op != 132 && op != 134 && op != 135 &&
           op != 137 && op != 138 && op != 139) ||
          (op >= kOpDPdx && op <= kOpFwidthCoarse)) {
        for (uint32_t i = 3; i < n; ++i) ids.push_back(w[i]);
      }
      return;
  }
}

}  // namespace

uint32_t RelajarPrecision(std::vector<uint32_t>& spirv) {
  if (spirv.size() < 5 || spirv[0] != 0x07230203u || spirv[3] == 0 || spirv[3] > kMaxBound) {
    return 0;
  }
  const uint32_t limite = spirv[3];
  struct Instr {
    size_t pos;
    uint32_t op;
    uint32_t n;
  };
  std::vector<Instr> instrucciones;
  for (size_t pos = 5; pos < spirv.size();) {
    const uint32_t n = spirv[pos] >> 16;
    if (n == 0 || pos + n > spirv.size()) {
      return 0;
    }
    instrucciones.push_back({pos, spirv[pos] & 0xFFFF, n});
    pos += n;
  }
  std::vector<uint8_t> flotante(limite, 0);  // 32-bit float scalar or vector types
  std::vector<uint8_t> relajado(limite, 0);
  std::vector<uint8_t> ya_relajado(limite, 0);
  std::vector<uint8_t> profundidad(limite, 0);  // FragDepth variables
  std::vector<uint32_t> tipo_de(limite, 0);     // result type of each result id
  size_t donde_insertar = 0;
  for (const Instr& in : instrucciones) {
    const uint32_t* w = spirv.data() + in.pos;
    if (in.op == kOpTypeFloat && in.n >= 3 && w[2] == 32 && w[1] < limite) {
      flotante[w[1]] = 1;
    } else if (in.op == kOpTypeVector && in.n >= 4 && w[1] < limite && w[2] < limite && flotante[w[2]]) {
      flotante[w[1]] = 1;
    } else if (in.op == kOpDecorate && in.n >= 3 && w[1] < limite) {
      if (w[2] == kDecorationRelaxedPrecision) {
        ya_relajado[w[1]] = 1;
      } else if (w[2] == kDecorationBuiltIn && in.n >= 4 && w[3] == kBuiltInFragDepth) {
        profundidad[w[1]] = 1;
      }
    }
    if (!donde_insertar && in.op >= kOpTypeVoid && in.op <= kOpTypeForwardPointer) {
      donde_insertar = in.pos;  // the first type: the annotations end right before it
    }
  }
  if (!donde_insertar) {
    return 0;
  }
  // Result type and id of the instructions that matter (all of them have them in words 1 and 2).
  std::vector<uint32_t> ids;
  for (const Instr& in : instrucciones) {
    const uint32_t* w = spirv.data() + in.pos;
    if (in.n >= 3 && (EsMuestreo(in.op) || in.op == kOpExtInst || in.op == kOpPhi || in.op == kOpSelect ||
                      (in.op >= kOpVectorExtractDynamic && in.op <= kOpCopyObject) ||
                      (in.op >= kOpFNegate && in.op <= kOpDot) || (in.op >= kOpDPdx && in.op <= kOpFwidthCoarse))) {
      if (w[2] < limite && w[1] < limite) {
        tipo_de[w[2]] = w[1];
      }
    }
  }
  // Sources: what a texture sample gives.
  for (const Instr& in : instrucciones) {
    const uint32_t* w = spirv.data() + in.pos;
    if (EsMuestreo(in.op) && in.n >= 3 && w[2] < limite && flotante[w[1] < limite ? w[1] : 0]) {
      relajado[w[2]] = 1;
    }
  }
  // Forward: a float result with a relaxed operand is relaxed too. A few passes cover the loops' phis.
  for (int pasada = 0; pasada < 8; ++pasada) {
    bool cambio = false;
    for (const Instr& in : instrucciones) {
      const uint32_t* w = spirv.data() + in.pos;
      if (in.n < 4 || w[2] >= limite || relajado[w[2]] || w[1] >= limite || !flotante[w[1]]) {
        continue;
      }
      Operandos(w, in.n, in.op, ids);
      for (uint32_t id : ids) {
        if (id < limite && relajado[id]) {
          relajado[w[2]] = 1;
          cambio = true;
          break;
        }
      }
    }
    if (!cambio) {
      break;
    }
  }
  // Backward: what feeds a texture coordinate, a depth reference, the image operands (lod, gradients, offsets) or
  // gl_FragDepth stays at 32 bits, with everything it comes from.
  std::vector<uint32_t> pendientes;
  for (const Instr& in : instrucciones) {
    const uint32_t* w = spirv.data() + in.pos;
    if (EsMuestreo(in.op) && in.n >= 5) {
      // result type, result, sampled image, coordinate, [dref or gather component], [image operands mask, ids...]
      const uint32_t mascara = EsDeReferencia(in.op) || in.op == kOpImageGather ? 6 : 5;
      for (uint32_t i = 4; i < in.n; ++i) {
        if (i != mascara) {
          pendientes.push_back(w[i]);
        }
      }
    } else if (in.op == kOpStore && in.n >= 3 && w[1] < limite && profundidad[w[1]]) {
      pendientes.push_back(w[2]);
    }
  }
  // Definition of each relaxed id, to walk back from it.
  std::vector<uint32_t> definicion(limite, UINT32_MAX);
  for (uint32_t i = 0; i < instrucciones.size(); ++i) {
    const uint32_t* w = spirv.data() + instrucciones[i].pos;
    if (instrucciones[i].n >= 3 && w[2] < limite && relajado[w[2]]) {
      definicion[w[2]] = i;
    }
  }
  while (!pendientes.empty()) {
    const uint32_t id = pendientes.back();
    pendientes.pop_back();
    if (id >= limite || !relajado[id]) {
      continue;
    }
    relajado[id] = 0;
    if (definicion[id] == UINT32_MAX) {
      continue;
    }
    const Instr& in = instrucciones[definicion[id]];
    if (EsMuestreo(in.op)) {
      continue;  // a sample feeding a coordinate (a dependent read) stays at 32 bits; its inputs were never relaxed
    }
    Operandos(spirv.data() + in.pos, in.n, in.op, ids);
    pendientes.insert(pendientes.end(), ids.begin(), ids.end());
  }
  // The decorations, at the end of the annotations.
  std::vector<uint32_t> decoraciones;
  uint32_t marcados = 0;
  for (uint32_t id = 1; id < limite; ++id) {
    if (relajado[id] && !ya_relajado[id]) {
      decoraciones.push_back((3u << 16) | kOpDecorate);
      decoraciones.push_back(id);
      decoraciones.push_back(kDecorationRelaxedPrecision);
      ++marcados;
    }
  }
  if (marcados) {
    spirv.insert(spirv.begin() + ptrdiff_t(donde_insertar), decoraciones.begin(), decoraciones.end());
  }
  return marcados;
}

}  // namespace nfsmw::nativo
