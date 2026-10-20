//===- VaListCalls.cpp - The calls that pass a va_list on -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A System V x86-64 function whose prologue builds a va_list may pass the
/// list's address to calls.  The compiler may also place another object in
/// the list's stack slot once va_end is past, such as a buffer it passes to
/// `strerror_r`, and C's 24-byte va_list holds no such object.
///
/// Each call takes one of the slot's objects.  Where a single call takes the
/// slot's address, and the function reads and writes the slot only through
/// va_start (recognizeVaStartForwarder), that call takes the va_list, unless
/// a routine's declaration has no va_list there.  Where more calls take it,
/// each must be shown to take a va_list: the va_list parameter of a
/// variadic routine's va_list form (`vfprintf`'s third), or a parameter of a
/// function in this image that reads and writes nothing through it outside
/// the list's 24 bytes and passes it on only to such parameters.
///
/// Calls take their register arguments as CallArgCollection finds them: the
/// value each integer argument register holds as the call reads it.
///
//===----------------------------------------------------------------------===//

#include "HighEntryStackOffsets.h"
#include "VariadicStackArguments.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/MedABIPass.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {
namespace {

/// The System V x86-64 va_list's bytes: gp_offset, fp_offset,
/// overflow_arg_area and reg_save_area.
constexpr int64_t kVaListBytes = 24;

/// A parameter: its function's entry and the position of its integer
/// argument register.
using Parameter = std::pair<va_t, unsigned>;

/// A value's identity, as MedVar::operator== compares it.
using ValueKey = std::tuple<uint8_t, int, int>;

ValueKey valueKey(const MedVar &V) {
  return {static_cast<uint8_t>(V.Kind), V.Id, V.SSAVer};
}

/// Whether \p Func takes a parameter in the register at \p RegOff.
bool hasRegisterParameter(const MedFunc &Func, uint64_t RegOff) {
  return llvm::any_of(Func.Params,
                      [&](const MedVar &P) { return P.RegOff == RegOff; });
}

/// One call's register arguments: the value each integer argument register
/// of its callee holds as the call reads it, by position.  Target is 0 for
/// an indirect call.
struct CallArguments {
  const MedOp *Call = nullptr;
  va_t Target = 0;
  std::vector<std::optional<MedVar>> Registers;
};

class Module {
public:
  Module(const std::vector<MedFunc> &Funcs, Arch TheArch, BinaryFormat Format,
         llvm::function_ref<NamedCallee(va_t)> Name)
      : TRI(getTargetRegInfo(TheArch)), Format(Format), Name(Name) {
    for (const MedFunc &Func : Funcs)
      ByEntry.emplace(Func.Entry, &Func);
  }

  const MedFunc *function(va_t Entry) const {
    const auto It = ByEntry.find(Entry);
    return It == ByEntry.end() ? nullptr : It->second;
  }

  /// What \p Target's name says of it.
  const NamedCallee &named(va_t Target) const {
    const auto [It, Inserted] = Named.try_emplace(Target);
    if (Inserted)
      It->second = Name(Target);
    return It->second;
  }

  /// The integer argument registers of \p Callee, a function of this image,
  /// in order; the convention's for any other callee.
  std::vector<uint64_t> argumentRegisters(const MedFunc *Callee) const {
    const IntegerArgumentLayout Layout =
        Callee ? integerArgumentLayoutOf(*Callee, TRI)
               : TRI.integerArgumentLayout(Format);
    return {Layout.Registers.begin(), Layout.Registers.end()};
  }

  /// Every call of \p Func with its register arguments.  A function of this
  /// image takes only the registers it has parameters in, and a routine its
  /// declaration fixes the parameters of only those.
  std::vector<CallArguments> calls(const MedFunc &Func) const {
    std::vector<CallArguments> Calls;
    for (const MedBlock &Block : Func.Blocks)
      for (size_t I = 0; I < Block.Ops.size(); ++I) {
        const MedOp &Op = Block.Ops[I];
        if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
          continue;
        CallArguments Call;
        Call.Call = &Op;
        if (Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
            Op.Inputs[0].isConst())
          Call.Target = static_cast<va_t>(Op.Inputs[0].ConstVal);
        const MedFunc *Callee = Call.Target ? function(Call.Target) : nullptr;
        const std::optional<unsigned> Declared =
            Call.Target && !Callee ? named(Call.Target).IntegerParams
                                   : std::nullopt;
        const std::vector<uint64_t> Registers = argumentRegisters(Callee);
        for (size_t Position = 0; Position < Registers.size(); ++Position) {
          const uint64_t Register = Registers[Position];
          const bool Taken = Callee ? hasRegisterParameter(*Callee, Register)
                                    : !Declared || Position < *Declared;
          MedVar Value;
          if (Taken && detail::reachingRegisterAtCall(Func, Block, I, Register,
                                                      TRI, Format, Value))
            Call.Registers.push_back(Value);
          else
            Call.Registers.push_back(std::nullopt);
        }
        Calls.push_back(std::move(Call));
      }
    return Calls;
  }

  const TargetRegInfo &TRI;
  const BinaryFormat Format;

private:
  llvm::function_ref<NamedCallee(va_t)> Name;
  std::map<va_t, const MedFunc *> ByEntry;
  mutable std::map<va_t, NamedCallee> Named;
};

/// What \p Func does with the pointer its integer argument register
/// \p RegOff brings: whether each use reads or writes within a va_list there,
/// or passes the pointer unchanged to a call, and the parameters it passes it
/// to.
struct PointerUse {
  bool Contained = true;
  std::vector<Parameter> Passes;
};

PointerUse pointerUse(const Module &M, const MedFunc &Func, uint64_t RegOff) {
  PointerUse Use;
  const auto Escapes = [&] {
    Use.Contained = false;
    Use.Passes.clear();
    return Use;
  };
  if (!hasRegisterParameter(Func, RegOff))
    return Escapes();
  // An indirect jump may take the register on to code no call names.
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::INDIR_BR)
        return Escapes();

  const uint16_t Width = static_cast<uint16_t>(M.TRI.PointerSize);
  // A read of the register's incoming value: the pointer, or a part of it.
  const auto EntryRead = [&](const MedVar &V) {
    return V.Kind == MedVar::Reg && V.SSAVer == 0 &&
           V.RegOff < RegOff + Width && RegOff < V.RegOff + V.Size;
  };
  // Each value computed from the pointer, by its byte offset from it.
  std::map<ValueKey, int64_t> Derived;
  bool Conflict = false;
  const auto OffsetOf = [&](const MedVar &V) -> std::optional<int64_t> {
    if (V.isConst())
      return std::nullopt;
    if (EntryRead(V) && V.RegOff == RegOff && V.Size == Width)
      return int64_t{0};
    const auto It = Derived.find(valueKey(V));
    if (It == Derived.end())
      return std::nullopt;
    return It->second;
  };
  const auto ConstantOf = [](const MedVar &V) -> std::optional<int64_t> {
    if (!V.isConst())
      return std::nullopt;
    return static_cast<int64_t>(V.ConstVal);
  };
  const auto SelfCopy = [](const MedOp &Op) {
    return Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
           Op.Inputs[0] == Op.Output;
  };
  // The offset \p Op's result is at, where it moves the pointer: a copy, or
  // a constant added or subtracted.
  const auto Moved = [&](const MedOp &Op) -> std::optional<int64_t> {
    if (SelfCopy(Op) || Op.Output.isConst())
      return std::nullopt;
    switch (Op.Opcode) {
    case NdOp::COPY:
      return Op.NumInputs >= 1 && Op.Output.Size == Width
                 ? OffsetOf(Op.Inputs[0])
                 : std::nullopt;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
      if (Op.NumInputs < 2)
        return std::nullopt;
      if (const auto Base = OffsetOf(Op.Inputs[0]))
        if (const auto C = ConstantOf(Op.Inputs[1]))
          return Op.Opcode == NdOp::INT_ADD ? *Base + *C : *Base - *C;
      if (Op.Opcode == NdOp::INT_ADD)
        if (const auto Base = OffsetOf(Op.Inputs[1]))
          if (const auto C = ConstantOf(Op.Inputs[0]))
            return *Base + *C;
      return std::nullopt;
    default:
      return std::nullopt;
    }
  };
  // A join of the pointer at one offset on every edge, or of itself.
  const auto Joined = [&](const PhiNode &Phi) -> std::optional<int64_t> {
    std::optional<int64_t> Offset;
    for (const auto &[Pred, Arg] : Phi.Args) {
      (void)Pred;
      if (Arg == Phi.Output)
        continue;
      const auto At = OffsetOf(Arg);
      if (!At || (Offset && *Offset != *At))
        return std::nullopt;
      Offset = At;
    }
    return Offset;
  };
  for (bool Grew = true; Grew && !Conflict;) {
    Grew = false;
    const auto Define = [&](const MedVar &Out, int64_t Offset) {
      const auto [It, Inserted] = Derived.emplace(valueKey(Out), Offset);
      if (Inserted)
        Grew = true;
      else if (It->second != Offset)
        Conflict = true;
    };
    for (const MedBlock &Block : Func.Blocks) {
      for (const PhiNode &Phi : Block.Phis)
        if (const auto Offset = Joined(Phi))
          Define(Phi.Output, *Offset);
      for (const MedOp &Op : Block.Ops)
        if (const auto Offset = Moved(Op))
          Define(Op.Output, *Offset);
    }
  }
  if (Conflict)
    return Escapes();
  // The values that hold the pointer on some paths only: a join of it with
  // another value, such as the register's state where a call on one path
  // changed it, and the copies of such a join.  They carry the pointer only
  // where something uses them.
  std::set<ValueKey> Mixed;
  const auto MayHold = [&](const MedVar &V) {
    return !V.isConst() && (EntryRead(V) || OffsetOf(V).has_value() ||
                            Mixed.count(valueKey(V)));
  };
  const auto Carried = [&](const MedOp &Op) {
    return Op.Opcode == NdOp::COPY && !SelfCopy(Op) && Op.NumInputs >= 1 &&
           !Op.Output.isConst() && Mixed.count(valueKey(Op.Inputs[0]));
  };
  for (bool Grew = true; Grew;) {
    Grew = false;
    for (const MedBlock &Block : Func.Blocks) {
      for (const PhiNode &Phi : Block.Phis)
        if (!Derived.count(valueKey(Phi.Output)) &&
            llvm::any_of(Phi.Args, [&](const auto &Incoming) {
              return Incoming.second != Phi.Output && MayHold(Incoming.second);
            }))
          Grew |= Mixed.insert(valueKey(Phi.Output)).second;
      for (const MedOp &Op : Block.Ops)
        if (Carried(Op))
          Grew |= Mixed.insert(valueKey(Op.Output)).second;
    }
  }

  // Within the va_list: \p Bytes accessed at \p Offset.
  const auto Within = [](int64_t Offset, int64_t Bytes) {
    return Offset >= 0 && Bytes > 0 && Offset + Bytes <= kVaListBytes;
  };
  for (const MedBlock &Block : Func.Blocks) {
    for (const MedOp &Op : Block.Ops) {
      if (SelfCopy(Op) || Carried(Op))
        continue;
      // A call's argument inputs repeat its argument registers and stack
      // stores, which are checked as such.
      const unsigned Inputs =
          Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL
              ? std::min<unsigned>(Op.NumInputs, 1)
              : Op.NumInputs;
      for (unsigned I = 0; I < Inputs; ++I) {
        const MedVar &In = Op.Inputs[I];
        const std::optional<int64_t> At = OffsetOf(In);
        if (!At) {
          if (MayHold(In))
            return Escapes();
          continue;
        }
        if (Moved(Op))
          continue;
        const bool Plain =
            Op.MemoryAddressSpace == NdMemoryAddressSpace::Default && I == 0;
        if (Op.Opcode == NdOp::LOAD && Plain && Within(*At, Op.Output.Size))
          continue;
        if (Op.Opcode == NdOp::STORE && Plain && Op.NumInputs >= 2 &&
            Within(*At, Op.Inputs[1].Size))
          continue;
        return Escapes();
      }
    }
  }
  // A call that finds the pointer in an argument register takes it there.
  for (const CallArguments &Call : M.calls(Func))
    for (size_t I = 0; I < Call.Registers.size(); ++I) {
      if (!Call.Registers[I] || !MayHold(*Call.Registers[I]))
        continue;
      const std::optional<int64_t> At = OffsetOf(*Call.Registers[I]);
      if (!At || *At != 0 || !Call.Target)
        return Escapes();
      Use.Passes.push_back({Call.Target, static_cast<unsigned>(I)});
    }
  return Use;
}

} // namespace

void markVaListCalls(std::vector<MedFunc> &Funcs, Arch TheArch,
                     BinaryFormat Format,
                     llvm::function_ref<NamedCallee(va_t)> Named) {
  for (MedFunc &Func : Funcs)
    Func.VaListCalls.clear();
  if (TheArch != Arch::X64)
    return;
  const Module M(Funcs, TheArch, Format, Named);

  // The parameters each call passes a function's va_list to, by function
  // and call site; an empty parameter stands for an indirect call's.
  std::map<std::pair<size_t, va_t>, std::vector<std::optional<Parameter>>>
      Sites;
  for (size_t F = 0; F < Funcs.size(); ++F) {
    const MedFunc &Func = Funcs[F];
    if (!Func.VaListDisplacement)
      continue;
    detail::HighEntryStackOffsets Offsets(Func, TheArch);
    for (const CallArguments &Call : M.calls(Func))
      for (size_t I = 0; I < Call.Registers.size(); ++I)
        if (Call.Registers[I] &&
            Offsets.offset(*Call.Registers[I]) == Func.VaListDisplacement)
          Sites[{F, Call.Call->Addr}].push_back(
              Call.Target ? std::optional<Parameter>(
                                {Call.Target, static_cast<unsigned>(I)})
                          : std::nullopt);
  }

  std::map<size_t, unsigned> SitesOf;
  for (const auto &[Site, Params] : Sites)
    ++SitesOf[Site.first];

  // What each parameter of a function in this image that a call reaches with
  // a va_list does with it, through every parameter it passes it on to.
  std::map<Parameter, PointerUse> Uses;
  std::deque<Parameter> Work;
  const auto Reach = [&](const Parameter &Param) {
    if (!M.named(Param.first).VaList && M.function(Param.first) &&
        Uses.emplace(Param, PointerUse{}).second)
      Work.push_back(Param);
  };
  for (const auto &[Site, Params] : Sites)
    if (SitesOf[Site.first] > 1)
      for (const std::optional<Parameter> &Param : Params)
        if (Param)
          Reach(*Param);
  while (!Work.empty()) {
    const Parameter Param = Work.front();
    Work.pop_front();
    const MedFunc &Callee = *M.function(Param.first);
    const std::vector<uint64_t> Registers = M.argumentRegisters(&Callee);
    PointerUse Use;
    if (Param.second < Registers.size())
      Use = pointerUse(M, Callee, Registers[Param.second]);
    else
      Use.Contained = false;
    for (const Parameter &Next : Use.Passes)
      Reach(Next);
    Uses[Param] = std::move(Use);
  }

  // A parameter takes a va_list where its routine's name says so, or where
  // the pointer stays within one and goes on only to parameters that take
  // one.  Recursion keeps the parameters nothing disproves.
  const auto TakesVaList = [&](const Parameter &Param) {
    if (const std::optional<unsigned> Position = M.named(Param.first).VaList)
      return *Position == Param.second;
    const auto It = Uses.find(Param);
    return It != Uses.end() && It->second.Contained;
  };
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (auto &[Param, Use] : Uses)
      if (Use.Contained && !llvm::all_of(Use.Passes, TakesVaList)) {
        Use.Contained = false;
        Changed = true;
      }
  }
  // A routine whose declaration fixes its parameters takes no va_list where
  // it has none.
  const auto NamedOtherwise = [&](const Parameter &Param) {
    const NamedCallee &Callee = M.named(Param.first);
    return Callee.VaList ? *Callee.VaList != Param.second
                         : Callee.IntegerParams.has_value();
  };
  for (const auto &[Site, Params] : Sites) {
    // A call that alone takes the slot's address takes the va_list.
    const bool Alone = SitesOf[Site.first] == 1;
    Funcs[Site.first].VaListCalls[Site.second] =
        llvm::all_of(Params, [&](const std::optional<Parameter> &Param) {
          if (Alone)
            return !Param || !NamedOtherwise(*Param);
          return Param && TakesVaList(*Param);
        });
  }
}

} // namespace neverd
