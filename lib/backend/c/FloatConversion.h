//===- FloatConversion.h - Scalar FP conversion policy ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_FLOATCONVERSION_H
#define NEVERD_BACKEND_C_FLOATCONVERSION_H

#include "neverd/ir/FloatConversion.h"

#include <stdexcept>
#include <string>
#include <tuple>

namespace neverd::c_float {

struct Conversion {
  unsigned Bits, FloatBits;
  bool Signed;
  FPToIntegerPolicy Policy;

  auto key() const { return std::tuple{Bits, FloatBits, Signed}; }
  void validate() const {
    if ((Bits != 1 && Bits != 8 && Bits != 16 && Bits != 32 && Bits != 64 &&
         Bits != 128) ||
        (FloatBits != 32 && FloatBits != 64 && FloatBits != 80))
      throw std::runtime_error("unsupported C scalar float conversion shape");
  }
};

/// Return the exact unsigned result bits. Capture the input once and keep
/// every potentially out-of-range C cast behind the corresponding guard.
template <typename Stream>
void writeConversion(Stream &OS, const std::string &Name, Conversion Shape) {
  Shape.validate();
  const unsigned CarrierBits = Shape.Bits == 1 ? 8 : Shape.Bits;
  const std::string Type = CarrierBits == 128
                               ? "unsigned __int128"
                               : "uint" + std::to_string(CarrierBits) + "_t";
  const std::string FloatType = Shape.FloatBits == 32   ? "float"
                                : Shape.FloatBits == 80 ? "long double"
                                                        : "double";
  const std::string Bound = "0x1p" + std::to_string(Shape.Bits - Shape.Signed) +
                            (Shape.FloatBits == 32 && Shape.Bits < 128 ? "f"
                             : Shape.FloatBits == 80                   ? "L"
                                                                       : "");
  const std::string Maximum =
      "((" + Type + ")~(" + Type + ")0 >> " +
      std::to_string(CarrierBits - Shape.Bits + Shape.Signed) + ")";
  const std::string Minimum =
      Shape.Signed
          ? "((" + Type + ")1 << " + std::to_string(Shape.Bits - 1) + ")"
          : "0";
  const std::string CastType =
      Shape.Signed
          ? (CarrierBits == 128 ? "__int128"
                                : "int" + std::to_string(CarrierBits) + "_t")
          : Type;
  const bool Indefinite = Shape.Policy == FPToIntegerPolicy::X86Indefinite;
  const std::string Invalid = Shape.Signed ? Minimum : Maximum;
  OS << "static inline " << Type << " " << Name << "(" << FloatType
     << " value) {\n"
     << "    if (__builtin_isnan(value)) return "
     << (Indefinite ? Invalid : "0") << ";\n"
     << "    if (value >= " << Bound << ") return "
     << (Indefinite ? Invalid : Maximum) << ";\n";
  if (Indefinite && !Shape.Signed)
    OS << "    if (value <= -1) return " << Invalid << ";\n";
  OS << "    if (value " << (Shape.Signed ? "< -" : "<= 0")
     << (Shape.Signed ? Bound : "") << ") return " << Minimum << ";\n"
     << "    return (" << Type << ")(" << CastType << ")value";
  if (Shape.Bits == 1)
    OS << " & 1";
  OS << ";\n}\n\n";
}

} // namespace neverd::c_float

#endif
