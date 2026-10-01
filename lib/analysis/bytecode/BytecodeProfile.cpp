//===- BytecodeProfile.cpp - External decoder specifications --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/analysis/BytecodeDecoder.h"

#include "llvm/Support/JSON.h"

using namespace neverd;
using namespace neverd::analysis;

namespace {

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "bytecode profile: " + Message);
}

llvm::Error keys(const llvm::json::Object &O,
                 std::initializer_list<llvm::StringRef> Allowed) {
  for (const auto &KV : O)
    if (std::find(Allowed.begin(), Allowed.end(), KV.first.str()) ==
        Allowed.end())
      return invalid("unknown key " + KV.first.str());
  return llvm::Error::success();
}

llvm::Expected<uint64_t> number(const llvm::json::Value &V) {
  if (auto I = V.getAsUINT64())
    return *I;
  if (auto S = V.getAsString()) {
    uint64_t N;
    if (!S->getAsInteger(0, N))
      return N;
  }
  return invalid("expected an unsigned integer or integer string");
}

template <typename T>
llvm::Error readNumber(const llvm::json::Object &O, llvm::StringRef Name,
                       T &Output, bool Required = false) {
  const auto *V = O.get(Name);
  if (!V)
    return Required ? invalid("missing " + Name) : llvm::Error::success();
  auto N = number(*V);
  if (!N)
    return N.takeError();
  if (*N > std::numeric_limits<T>::max())
    return invalid("out of range " + Name);
  Output = *N;
  return llvm::Error::success();
}

llvm::Expected<BytecodeValue> value(const llvm::json::Object &O) {
  if (auto E = keys(O, {"offset", "bytes", "shift", "mask", "scale", "addend",
                        "bits", "pc_relative", "lookup"}))
    return std::move(E);
  BytecodeValue V;
  if (auto E = readNumber(O, "offset", V.Offset))
    return std::move(E);
  if (auto E = readNumber(O, "bytes", V.Bytes))
    return std::move(E);
  if (auto E = readNumber(O, "shift", V.Shift))
    return std::move(E);
  if (auto E = readNumber(O, "mask", V.Mask))
    return std::move(E);
  if (auto E = readNumber(O, "scale", V.Scale))
    return std::move(E);
  if (auto E = readNumber(O, "addend", V.Addend))
    return std::move(E);
  if (auto E = readNumber(O, "bits", V.ValueBits))
    return std::move(E);
  if (O.get("pc_relative")) {
    auto B = O.getBoolean("pc_relative");
    if (!B)
      return invalid("pc_relative must be boolean");
    V.PCRelative = *B;
  }
  if (O.get("lookup")) {
    auto *A = O.getArray("lookup");
    if (!A || A->empty() || A->size() > 65536)
      return invalid("invalid lookup table");
    for (const auto &Item : *A) {
      auto N = number(Item);
      if (!N)
        return N.takeError();
      V.Lookup.push_back(*N);
    }
  }
  return V;
}

llvm::Expected<BytecodeOperand> operand(const llvm::json::Value &V) {
  const auto *O = V.getAsObject();
  if (!O)
    return invalid("operand must be an object");
  if (auto E = keys(*O, {"space", "size", "value"}))
    return std::move(E);
  BytecodeOperand Result;
  auto Space = O->getString("space");
  if (!Space)
    return invalid("missing operand space");
  if (*Space == "reg")
    Result.Space = VnodeSpace::REG;
  else if (*Space == "temp")
    Result.Space = VnodeSpace::TEMP;
  else if (*Space == "const")
    Result.Space = VnodeSpace::CONST;
  else
    return invalid("unknown operand space");
  if (auto E = readNumber(*O, "size", Result.Size, true))
    return std::move(E);
  if (!Result.Size)
    return invalid("operand size must be nonzero");
  const auto *Field = O->getObject("value");
  if (!Field)
    return invalid("missing operand value");
  auto F = value(*Field);
  if (!F)
    return F.takeError();
  Result.Value = std::move(*F);
  return Result;
}

llvm::Expected<BytecodeOperation> operation(const llvm::json::Value &V) {
  const auto *O = V.getAsObject();
  if (!O)
    return invalid("operation must be an object");
  if (auto E = keys(*O, {"op", "output", "inputs"}))
    return std::move(E);
  auto Name = O->getString("op");
  if (!Name)
    return invalid("missing operation name");
  BytecodeOperation Result;
  bool Found = false;
  for (unsigned I = 0; I != static_cast<unsigned>(NdOp::_COUNT); ++I)
    if (*Name == ndOpName(static_cast<NdOp>(I))) {
      Result.Opcode = static_cast<NdOp>(I);
      Found = true;
      break;
    }
  if (!Found)
    return invalid("unknown operation " + *Name);
  if (auto *Output = O->get("output")) {
    auto R = operand(*Output);
    if (!R)
      return R.takeError();
    Result.Output = std::move(*R);
  }
  const auto *Inputs = O->getArray("inputs");
  if (!Inputs || Inputs->size() > 6)
    return invalid("missing or oversized operation inputs");
  for (const auto &Input : *Inputs) {
    auto R = operand(Input);
    if (!R)
      return R.takeError();
    Result.Inputs.push_back(std::move(*R));
  }
  return Result;
}

} // namespace

llvm::Expected<BytecodeProfile>
neverd::analysis::readBytecodeProfile(llvm::StringRef Text) {
  if (Text.size() > 64 * 1024 * 1024)
    return invalid("JSON size limit exceeded");
  auto Parsed = llvm::json::parse(Text);
  if (!Parsed)
    return Parsed.takeError();
  const auto *O = Parsed->getAsObject();
  if (!O)
    return invalid("root must be an object");
  if (auto E = keys(*O, {"version", "register_bytes", "temporary_bytes",
                         "byte_order", "encodings"}))
    return std::move(E);
  unsigned Version = 0;
  if (auto E = readNumber(*O, "version", Version, true))
    return std::move(E);
  if (Version != 1)
    return invalid("unsupported version");
  BytecodeProfile P;
  if (auto E = readNumber(*O, "register_bytes", P.RegisterBytes, true))
    return std::move(E);
  if (auto E = readNumber(*O, "temporary_bytes", P.TemporaryBytes))
    return std::move(E);
  auto Order = O->getString("byte_order");
  if (!Order || (*Order != "little" && *Order != "big"))
    return invalid("byte_order must be little or big");
  P.ByteOrder =
      *Order == "little" ? llvm::endianness::little : llvm::endianness::big;
  const auto *Encodings = O->getArray("encodings");
  if (!Encodings || Encodings->empty() || Encodings->size() > 65536)
    return invalid("missing or oversized encodings");
  uint64_t Ops = 0;
  for (const auto &Item : *Encodings) {
    const auto *EO = Item.getAsObject();
    if (!EO)
      return invalid("encoding must be an object");
    if (auto E = keys(*EO, {"size", "match", "operations"}))
      return std::move(E);
    BytecodeEncoding Encoding;
    if (auto E = readNumber(*EO, "size", Encoding.Size, true))
      return std::move(E);
    const auto *Matches = EO->getArray("match");
    const auto *Operations = EO->getArray("operations");
    if (!Matches || Matches->size() > 4096 || !Operations ||
        Operations->size() > 4096 || (Ops += Operations->size()) > 1000000)
      return invalid("invalid match or operation array");
    for (const auto &M : *Matches) {
      const auto *MO = M.getAsObject();
      if (!MO)
        return invalid("match must be an object");
      if (auto E = keys(*MO, {"offset", "mask", "value"}))
        return std::move(E);
      BytecodeMatch Match;
      if (auto E = readNumber(*MO, "offset", Match.Offset, true))
        return std::move(E);
      if (auto E = readNumber(*MO, "mask", Match.Mask))
        return std::move(E);
      if (auto E = readNumber(*MO, "value", Match.Value, true))
        return std::move(E);
      Encoding.Match.push_back(Match);
    }
    for (const auto &Op : *Operations) {
      auto R = operation(Op);
      if (!R)
        return R.takeError();
      Encoding.Operations.push_back(std::move(*R));
    }
    P.Encodings.push_back(std::move(Encoding));
  }
  return P;
}
