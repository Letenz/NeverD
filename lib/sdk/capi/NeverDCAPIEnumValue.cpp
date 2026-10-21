#include "SessionImpl.h"

#include "neverd/sdk/Limits.h"
#include "neverd/support/NumericInput.h"

#include "llvm/ADT/StringExtras.h"
using namespace neverd;
using namespace neverd::sdk;

const char *neverd_enum_value_json(neverd_session_t Sess, const char *Name,
                                   const char *Value, unsigned Bits,
                                   int Flags) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  try {
    const auto Parsed = Value && std::strlen(Value) <= 32
                            ? parseUnsignedDecimalOrHex(Value)
                            : std::nullopt;
    if (!S->Loaded || !Name || std::strlen(Name) > kMaxEnumTypeNameBytes ||
        !Parsed || !Bits || Bits > 64)
      throw std::invalid_argument(
          "An enum value requires a type and 1..64 logical bits");
    uint64_t Number = *Parsed;
    const auto *Library = S->readLocalTypes();
    const auto *Type = Library ? Library->resolve(Name) : nullptr;
    if (!Type || !Type->Enum || Type->Enum->Values.size() > kMaxEnumValues)
      throw std::invalid_argument(
          "Select a bounded enumeration in Local Types");
    const unsigned EnumBits = Library->builtinBytes(Type->Enum->Underlying) * 8;
    if (!EnumBits || EnumBits > 64 || Bits < EnumBits)
      throw std::invalid_argument("The enum is wider than this operand");
    const auto Mask = Bits == 64 ? UINT64_MAX : (uint64_t(1) << Bits) - 1;
    const auto EnumMask =
        EnumBits == 64 ? UINT64_MAX : (uint64_t(1) << EnumBits) - 1;
    if (Number > Mask &&
        ((Number & ~Mask) != ~Mask || !(Number & (uint64_t(1) << (Bits - 1)))))
      throw std::invalid_argument(
          "The value is outside the operand's logical width");
    Number &= Mask;
    std::string Text;
    llvm::json::Array Aliases;
    auto Spelling = [&](uint64_t V) {
      V &= EnumMask;
      if (types::builtinInfo(Type->Enum->Underlying).Signed &&
          EnumBits < Bits && (V & (uint64_t(1) << (EnumBits - 1))))
        V |= ~EnumMask;
      return V & Mask;
    };
    for (const auto &Member : Type->Enum->Values)
      if (Spelling(Member.Value) == Number) {
        Aliases.push_back(Member.Name);
        if (Text.empty())
          Text = Member.Name;
      }
    uint64_t Remaining = Number;
    if (!Text.empty())
      Remaining = 0;
    if (Text.empty() && (Flags || Type->Enum->Flags)) {
      // Single-bit members make a deterministic decomposition, without
      // dropping bits or choosing overlapping composite aliases arbitrarily.
      for (const auto &Member : Type->Enum->Values) {
        const auto Bit = Spelling(Member.Value);
        if (!Bit || (Bit & (Bit - 1)) || (Remaining & Bit) != Bit)
          continue;
        if (!Text.empty())
          Text += " | ";
        Text += Member.Name;
        Remaining &= ~Bit;
      }
    }
    if (Text.empty() || Remaining) {
      if (!Text.empty())
        Text += " | ";
      Text += "0x" + llvm::utohexstr(Text.empty() ? Number : Remaining);
    }
    llvm::json::Value Result =
        llvm::json::Object{{"text", Text},
                           {"value", "0x" + llvm::utohexstr(Number)},
                           {"bits", Bits},
                           {"aliases", std::move(Aliases)},
                           {"residual", "0x" + llvm::utohexstr(Remaining)}};
    return dupStr(jsonToString(std::move(Result)));
  } catch (const std::exception &E) {
    S->setError(E.what());
    return nullptr;
  }
}
