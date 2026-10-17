//===- SourcePatchCallbackCOFF.cpp - Relocated entry unwind programs -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Derive native Windows unwind programs from an authenticated entry prefix.
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/SourcePatchCallback.h"
#include "neverd/loader/ExceptionFunction.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/bit.h"
#include "llvm/Support/ARMWinEH.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/MathExtras.h"

namespace neverd {
namespace {
llvm::Error fail(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "original callback unwind: " + Message);
}

bool boundary(const SourceCallbackPrefix &Prefix, uint32_t Offset) {
  return Offset == Prefix.CopiedSize ||
         llvm::any_of(Prefix.Instructions,
                      [&](const auto &I) { return I.Offset == Offset; });
}

llvm::Error encodePackedOperation(std::vector<uint8_t> &Out,
                                  const UnwindOperation &Op, bool ARM64) {
  using K = UnwindOperationKind;
  const uint64_t N = Op.StackOffset;
  auto Pair = [&](uint16_t Code) {
    Out.push_back(Code >> 8);
    Out.push_back(Code & 255);
  };
  if (Op.Kind == K::Nop) {
    Out.push_back(ARM64 ? 0xe3 : Op.InstructionSize == 2 ? 0xfb : 0xfc);
    return llvm::Error::success();
  }
  if (Op.Kind == K::AllocateStack) {
    if (ARM64 && N % 16 == 0 && N / 16 < 2048) {
      if (N / 16 < 32)
        Out.push_back(N / 16);
      else
        Pair(0xc000 | (N / 16));
      return llvm::Error::success();
    }
    if (!ARM64 && N % 4 == 0 &&
        ((Op.InstructionSize == 2 && N / 4 < 128) ||
         (Op.InstructionSize == 4 && N / 4 < 1024))) {
      if (Op.InstructionSize == 2)
        Out.push_back(N / 4);
      else
        Pair(0xe800 | (N / 4));
      return llvm::Error::success();
    }
  }
  if (ARM64 && Op.Register == 29 &&
      Op.RegisterClass == UnwindRegisterClass::GeneralPurpose) {
    if (Op.Kind == K::SetFramePointer && N == 0) {
      Out.push_back(0xe1);
      return llvm::Error::success();
    }
    if (Op.Kind == K::AddFramePointer && N % 8 == 0 && N / 8 < 256) {
      Pair(0xe200 | (N / 8));
      return llvm::Error::success();
    }
    if (Op.RegisterMask == ((1u << 29) | (1u << 30)) && N % 8 == 0) {
      if (Op.Kind == K::SaveRegisterPair && N / 8 < 64) {
        Out.push_back(0x40 | (N / 8));
        return llvm::Error::success();
      }
      if (Op.Kind == K::SaveRegisterPairPreIndexed && N && N / 8 <= 64) {
        Out.push_back(0x80 | (N / 8 - 1));
        return llvm::Error::success();
      }
    }
  }
  if (ARM64 && N % 8 == 0) {
    const bool FP = Op.RegisterClass == UnwindRegisterClass::FloatingPoint;
    const unsigned First = FP ? 8 : 19, Last = FP ? 15 : 30;
    const bool Pre = Op.Kind == K::SaveRegisterPreIndexed ||
                     Op.Kind == K::SaveRegisterPairPreIndexed;
    const bool IsPair = Op.Kind == K::SaveRegisterPair ||
                        Op.Kind == K::SaveRegisterPairPreIndexed;
    const bool IsSingle =
        Op.Kind == K::SaveRegister || Op.Kind == K::SaveRegisterPreIndexed;
    const uint64_t Slot = N / 8;
    const uint64_t Z = Pre && Slot ? Slot - 1 : Slot;
    if ((FP || Op.RegisterClass == UnwindRegisterClass::GeneralPurpose) &&
        Op.Register >= First && Op.Register <= Last && (!Pre || Slot) &&
        (IsPair || IsSingle)) {
      const uint32_t Mask = IsPair ? (3u << Op.Register) : (1u << Op.Register);
      if (Op.RegisterMask == Mask && (!IsPair || Op.Register < Last) &&
          Z < (Pre && IsSingle ? 32u : 64u)) {
        const uint32_t X = Op.Register - First;
        const uint16_t Code =
            IsPair ? (FP ? (Pre ? 0xda00 : 0xd800) : (Pre ? 0xcc00 : 0xc800))
            : FP   ? (Pre ? 0xde00 : 0xdc00)
                   : (Pre ? 0xd400 : 0xd000);
        Pair(Code | (X << (Pre && IsSingle ? 5 : 6)) | Z);
        return llvm::Error::success();
      }
      if (!FP && IsPair && !Pre && Op.Register <= 29 &&
          (Op.Register - 19) % 2 == 0 &&
          Op.RegisterMask == ((1u << Op.Register) | (1u << 30)) && Z < 64) {
        Pair(0xd600 | (((Op.Register - 19) / 2) << 6) | Z);
        return llvm::Error::success();
      }
    }
  }
  if (!ARM64 && (Op.Kind == K::SaveRegisterPreIndexed ||
                 Op.Kind == K::SaveRegisterPairPreIndexed)) {
    if (Op.RegisterClass == UnwindRegisterClass::GeneralPurpose &&
        !(Op.RegisterMask & ~0x5fffu) && Op.RegisterMask &&
        N == llvm::popcount(Op.RegisterMask) * 4u) {
      if (Op.InstructionSize == 2 && !(Op.RegisterMask & ~0x40ffu)) {
        Pair(0xec00 | (Op.RegisterMask & 255) |
             ((Op.RegisterMask & (1u << 14)) ? 0x100 : 0));
        return llvm::Error::success();
      }
      if (Op.InstructionSize == 4) {
        Pair(0x8000 | (Op.RegisterMask & 0x1fff) |
             ((Op.RegisterMask & (1u << 14)) ? 0x2000 : 0));
        return llvm::Error::success();
      }
    }
    if (Op.RegisterClass == UnwindRegisterClass::FloatingPoint &&
        Op.InstructionSize == 4 && Op.Register <= 31) {
      const unsigned Count = llvm::popcount(Op.RegisterMask);
      if (Count && Count <= 16 && Op.Register + Count <= 32 &&
          Op.Register / 16 == (Op.Register + Count - 1) / 16 &&
          Op.RegisterMask ==
              ((uint32_t((uint64_t(1) << Count) - 1)) << Op.Register) &&
          N == Count * 8u) {
        Pair((Op.Register < 16 ? 0xf500 : 0xf600) | ((Op.Register % 16) << 4) |
             ((Op.Register + Count - 1) % 16));
        return llvm::Error::success();
      }
    }
  }
  return fail("packed Windows entry operation has no callback encoding");
}
} // namespace

llvm::Expected<std::vector<uint8_t>>
encodeSourceCallbackCOFFUnwind(const SourceCallbackPrefix &Prefix,
                               const ExceptionFunction &Function,
                               Arch Architecture) {
  using K = UnwindOperationKind;
  if (Function.CodeRange.Begin != Prefix.OriginalVA ||
      Function.CodeRange.size() < Prefix.CopiedSize ||
      Function.ParseStatus != ExceptionParseStatus::Complete ||
      Function.Kind != RuntimeFunctionKind::Primary ||
      Function.Personality != ExceptionPersonality::None ||
      Function.hasLanguageTable() || Prefix.Instructions.empty())
    return fail(
        "callback needs one complete Windows frame without a language handler");
  if (Architecture == Arch::X64) {
    if (Function.UnwindVersion != 1 || Function.UnwindFlags ||
        Prefix.CopiedSize > 255)
      return fail("callback needs an unchained version-one x64 frame");
    std::vector<uint8_t> Out{
        1,
        uint8_t(std::min<uint32_t>(Function.PrologueSize, Prefix.CopiedSize)),
        0, 0};
    for (const auto &Op : Function.UnwindOperations) {
      if (Op.CodeOffset > Prefix.CopiedSize)
        continue;
      if (!boundary(Prefix, Op.CodeOffset) || !Op.SlotCount ||
          Op.OperandBytes.size() != Op.SlotCount * 2u || Op.Kind == K::Opaque ||
          Op.Kind == K::PushMachineFrame)
        return fail(
            "x64 frame rule is not an exact relocated instruction boundary");
      if (Op.Kind == K::SetFramePointer) {
        if (Function.FrameRegister > 15 || Function.FrameOffset % 16 ||
            Function.FrameOffset / 16 > 15)
          return fail("invalid x64 frame register");
        Out[3] = Function.FrameRegister | ((Function.FrameOffset / 16) << 4);
      }
      Out.insert(Out.end(), Op.OperandBytes.begin(), Op.OperandBytes.end());
    }
    if ((Out.size() - 4) / 2 > 255)
      return fail("too many x64 entry unwind slots");
    Out[2] = (Out.size() - 4) / 2;
    Out.resize(llvm::alignTo(Out.size(), 4), 0);
    return Out;
  }
  const bool ARM64 = Architecture == Arch::AArch64;
  if ((!ARM64 && Architecture != Arch::ARM) ||
      (!ARM64 && Prefix.Mode != InstructionMode::Thumb) ||
      (Function.PackedUnwindData & 3) > 1 ||
      Prefix.Bytes.size() % (ARM64 ? 4 : 2))
    return fail("unsupported Windows ARM callback frame");
  const bool Packed = (Function.PackedUnwindData & 3) == 1;
  std::vector<const UnwindOperation *> Physical;
  for (const auto &Op : Function.UnwindOperations) {
    if (Op.Kind == K::End)
      continue;
    if (!Op.InstructionSize || Op.Kind == K::EndChained ||
        Op.Kind == K::Opaque || Op.Kind == K::CustomStackFrame ||
        Op.Kind == K::SignReturnAddress ||
        Op.Kind == K::AllocateVectorLengthStack)
      return fail(
          "Windows ARM entry uses an unmodeled chained or special frame");
    Physical.push_back(&Op);
  }
  if (!Packed)
    std::reverse(Physical.begin(), Physical.end());
  llvm::ArrayRef<uint8_t> OriginalCodes;
  if (!Packed) {
    if (Function.NativeUnwindBytes.size() < 4)
      return fail("missing original Windows ARM unwind bytes");
    const auto *Words = reinterpret_cast<const llvm::support::ulittle32_t *>(
        Function.NativeUnwindBytes.data());
    const uint32_t First = readLE<uint32_t>(Function.NativeUnwindBytes.data());
    const bool Extended = (First & (ARM64 ? 0xffc00000u : 0xff800000u)) == 0;
    if (Extended && Function.NativeUnwindBytes.size() < 8)
      return fail("truncated original Windows ARM unwind header");
    const llvm::ARM::WinEH::ExceptionDataRecord Record(Words, ARM64);
    const uint64_t Offset =
        (Extended ? 8 : 4) +
        (Record.E() ? 0 : uint64_t(Record.EpilogueCount()) * 4);
    const uint64_t Size = uint64_t(Record.CodeWords()) * 4;
    if (!rangeInBounds(Offset, Size, Function.NativeUnwindBytes.size()))
      return fail("unbounded original Windows ARM unwind codes");
    OriginalCodes =
        llvm::ArrayRef<uint8_t>(Function.NativeUnwindBytes).slice(Offset, Size);
  }
  std::vector<std::vector<uint8_t>> Instructions;
  uint32_t Offset = 0;
  for (const auto *Op : Physical) {
    if (Offset >= Prefix.CopiedSize)
      break;
    const uint32_t End = Offset + Op->InstructionSize;
    const auto I = llvm::find_if(Prefix.Instructions, [&](const auto &I) {
      return I.Offset == Offset && I.Size == Op->InstructionSize;
    });
    if (End > Prefix.CopiedSize || I == Prefix.Instructions.end())
      return fail("Windows ARM unwind widths do not match copied instructions");
    std::vector<uint8_t> Code;
    if (Packed) {
      if (auto Error = encodePackedOperation(Code, *Op, ARM64))
        return Error;
    } else {
      if (!Op->SlotCount ||
          !rangeInBounds(Op->CodeOffset, Op->SlotCount, OriginalCodes.size()))
        return fail("Windows ARM unwind operation leaves its code array");
      auto Bytes = OriginalCodes.slice(Op->CodeOffset, Op->SlotCount);
      Code.assign(Bytes.begin(), Bytes.end());
    }
    Instructions.push_back(std::move(Code));
    Offset = End;
  }
  // Body instructions retain the completed original prologue state. Nops in
  // the unwind stream describe their exact widths; they do not modify code.
  for (const auto &I : Prefix.Instructions)
    if (I.Offset >= Offset)
      Instructions.push_back({uint8_t(ARM64         ? 0xe3
                                      : I.Size == 2 ? 0xfb
                                                    : 0xfc)});
  std::vector<uint8_t> Out(4, 0);
  for (auto I = Instructions.rbegin(); I != Instructions.rend(); ++I)
    Out.insert(Out.end(), I->begin(), I->end());
  Out.push_back(ARM64 ? 0xe4 : 0xff);
  Out.resize(llvm::alignTo(Out.size(), 4), ARM64 ? 0xe3 : 0xfb);
  const uint64_t CodeWords = (Out.size() - 4) / 4;
  const uint64_t Length = Prefix.Bytes.size() / (ARM64 ? 4 : 2);
  if (CodeWords > (ARM64 ? 31u : 15u) || Length >= (1u << 18))
    return fail("Windows ARM callback unwind exceeds compact header limits");
  // Version zero, no language handler, no epilogue scopes, a real prologue.
  writeLE<uint32_t>(Out.data(), uint32_t(Length) |
                                    (uint32_t(CodeWords) << (ARM64 ? 27 : 28)));
  return Out;
}
} // namespace neverd
