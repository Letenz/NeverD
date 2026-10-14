//===- KernelModelRuntime.cpp - Checked guest runtime services ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guest counted strings and finite Windows debug formatting. The model never
/// passes a guest format, pointer, or variadic list to the host C runtime.
/// Contracts: Microsoft Learn RtlCopyUnicodeString, RtlCompareUnicodeString,
/// DbgPrint, and format-specification-syntax-printf-and-wprintf-functions.
///
//===----------------------------------------------------------------------===//

#include "KernelModelRuntime.h"

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace neverd::emulation::runtime {
namespace {
using namespace windows;
constexpr uint64_t MaxASCII = 0x7f;

#define NEVERD_KERNEL_RUNTIME_STRING(Name, Value)                              \
  constexpr llvm::StringLiteral Name(Value);
#include "KernelRuntimeStrings.def"
#undef NEVERD_KERNEL_RUNTIME_STRING

llvm::Error runtimeError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

llvm::Expected<uint64_t> readInteger(const KernelModel &Model,
                                     GuestMemory &Memory, uint64_t Address,
                                     unsigned Size) {
  if (auto E = Model.validateGuestAccess(Address, Size, false))
    return E;
  return Memory.readInteger(Address, Size);
}

struct CountedString {
  uint16_t Length;
  uint16_t MaximumLength;
  uint64_t Buffer;
};

llvm::Expected<CountedString> readCountedString(const KernelModel &Model,
                                                GuestMemory &Memory,
                                                uint64_t Address) {
  if (!Address)
    return runtimeError("UNICODE_STRING record must not be null");
  if (auto E = Model.validateGuestAccess(Address, UnicodeRecordSize, false))
    return E;
  auto Length = Memory.readInteger(Address, 2);
  if (!Length)
    return Length.takeError();
  auto Maximum = Memory.readInteger(Address + UnicodeMaximumOffset, 2);
  if (!Maximum)
    return Maximum.takeError();
  auto Buffer = Memory.readInteger(Address + UnicodeBufferOffset, 8);
  if (!Buffer)
    return Buffer.takeError();
  if ((*Length & 1) || (*Maximum & 1) || *Length > *Maximum ||
      *Maximum > MaxCountedUnicodeBytes || (*Length && !*Buffer))
    return runtimeError("invalid counted UTF-16 string lengths or buffer");
  return CountedString{static_cast<uint16_t>(*Length),
                       static_cast<uint16_t>(*Maximum), *Buffer};
}

llvm::Expected<std::vector<uint8_t>> readBytes(const KernelModel &Model,
                                               GuestMemory &Memory,
                                               uint64_t Address,
                                               unsigned Size) {
  if (auto E = Model.validateGuestAccess(Address, Size, false))
    return E;
  std::vector<uint8_t> Bytes(Size);
  if (Size)
    if (auto E = Memory.read(Address, Bytes))
      return E;
  return Bytes;
}

uint16_t codeUnit(llvm::ArrayRef<uint8_t> Bytes, size_t Index) {
  return Bytes[Index] | (uint16_t(Bytes[Index + 1]) << 8);
}

enum class Conversion {
  SignedDecimal,
  UnsignedDecimal,
  Octal,
  HexLower,
  HexUpper,
  Pointer,
  String,
  Character,
  CountedString
};

std::optional<Conversion> conversion(char Code) {
  switch (Code) {
#define NEVERD_KERNEL_DEBUG_FORMAT(Code, Kind)                                 \
  case Code:                                                                   \
    return Conversion::Kind;
#include "KernelDebugFormats.def"
#undef NEVERD_KERNEL_DEBUG_FORMAT
  default:
    return std::nullopt;
  }
}

enum class LengthKind {
  None,
#define NEVERD_KERNEL_DEBUG_LENGTH(Spelling, Name, Bits) Name,
#include "KernelDebugLengths.def"
#undef NEVERD_KERNEL_DEBUG_LENGTH
};

struct FormatSpec {
  bool Left = false;
  bool Plus = false;
  bool Space = false;
  bool Zero = false;
  bool Alternate = false;
  unsigned Width = 0;
  std::optional<unsigned> Precision;
  LengthKind Length = LengthKind::None;
  unsigned Bits = 32;
};

class Formatter {
  const KernelModel &Model;
  GuestMemory &Memory;
  ArgumentReader ReadArgument;
  unsigned NextArgument;
  unsigned UsedArguments = 0;
  std::string Output;

  llvm::Expected<uint64_t> argument() {
    if (UsedArguments == MaxDebugArguments)
      return runtimeError("DbgPrint exceeds the variadic argument limit");
    if (!ReadArgument)
      return runtimeError("DbgPrint variadic argument reader is unavailable");
    ++UsedArguments;
    return ReadArgument(NextArgument++);
  }

  llvm::Error append(llvm::StringRef Text) {
    if (Text.size() > MaxDebugBytes - Output.size())
      return runtimeError(llvm::formatv(
          "DbgPrint output exceeds the {0}-byte model limit", MaxDebugBytes));
    Output.append(Text.data(), Text.size());
    return llvm::Error::success();
  }

  llvm::Expected<unsigned> number(llvm::StringRef Format, size_t &Position) {
    unsigned Value = 0;
    while (Position < Format.size() && Format[Position] >= '0' &&
           Format[Position] <= '9') {
      unsigned Digit = Format[Position++] - '0';
      if (Value > (MaxDebugFieldWidth - Digit) / 10)
        return runtimeError("DbgPrint width or precision exceeds model limit");
      Value = Value * 10 + Digit;
    }
    return Value;
  }

  llvm::Expected<FormatSpec> spec(llvm::StringRef Format, size_t &Position) {
    FormatSpec Spec;
    while (Position < Format.size()) {
      bool Flag = true;
      switch (Format[Position]) {
      case '-':
        Spec.Left = true;
        break;
      case '+':
        Spec.Plus = true;
        break;
      case ' ':
        Spec.Space = true;
        break;
      case '0':
        Spec.Zero = true;
        break;
      case '#':
        Spec.Alternate = true;
        break;
      default:
        Flag = false;
        break;
      }
      if (!Flag)
        break;
      ++Position;
    }
    if (Position < Format.size() && Format[Position] == '*') {
      ++Position;
      auto Width = argument();
      if (!Width)
        return Width.takeError();
      const int64_t SignedWidth = static_cast<int32_t>(*Width);
      Spec.Left |= SignedWidth < 0;
      const uint64_t Magnitude = SignedWidth < 0 ? -SignedWidth : SignedWidth;
      if (Magnitude > MaxDebugFieldWidth)
        return runtimeError("DbgPrint width exceeds model limit");
      Spec.Width = Magnitude;
    } else {
      auto Width = number(Format, Position);
      if (!Width)
        return Width.takeError();
      Spec.Width = *Width;
    }
    if (Position < Format.size() && Format[Position] == '.') {
      ++Position;
      if (Position < Format.size() && Format[Position] == '*') {
        ++Position;
        auto Precision = argument();
        if (!Precision)
          return Precision.takeError();
        const int32_t SignedPrecision = static_cast<int32_t>(*Precision);
        if (SignedPrecision > static_cast<int32_t>(MaxDebugFieldWidth))
          return runtimeError("DbgPrint precision exceeds model limit");
        if (SignedPrecision >= 0)
          Spec.Precision = SignedPrecision;
      } else {
        auto Precision = number(Format, Position);
        if (!Precision)
          return Precision.takeError();
        Spec.Precision = *Precision;
      }
    }
    struct LengthDescription {
      llvm::StringLiteral Spelling;
      LengthKind Kind;
      unsigned Bits;
    };
    static constexpr LengthDescription Lengths[] = {
#define NEVERD_KERNEL_DEBUG_LENGTH(Spelling, Name, Bits)                       \
  {Spelling, LengthKind::Name, Bits},
#include "KernelDebugLengths.def"
#undef NEVERD_KERNEL_DEBUG_LENGTH
    };
    for (const auto &Length : Lengths)
      if (Format.drop_front(Position).starts_with(Length.Spelling)) {
        Position += Length.Spelling.size();
        Spec.Length = Length.Kind;
        Spec.Bits = Length.Bits;
        break;
      }
    return Spec;
  }

  llvm::Expected<std::string> string(uint64_t Address, bool Wide,
                                     std::optional<unsigned> Precision) {
    const unsigned Limit = Precision.value_or(MaxDebugBytes);
    if (!Address)
      return NullString.take_front(Limit).str();
    std::string Text;
    const unsigned Step = Wide ? 2 : 1;
    for (unsigned I = 0; I < Limit; ++I) {
      if (Address > UINT64_MAX - uint64_t(I) * Step)
        return runtimeError("DbgPrint string address overflows");
      auto Unit =
          readInteger(Model, Memory, Address + uint64_t(I) * Step, Step);
      if (!Unit)
        return Unit.takeError();
      if (!*Unit)
        return Text;
      if (*Unit > MaxASCII)
        return runtimeError(
            "DbgPrint non-ASCII text requires an unmodeled code page");
      Text.push_back(static_cast<char>(*Unit));
    }
    if (Precision)
      return Text;
    if (Address > UINT64_MAX - uint64_t(Limit) * Step)
      return runtimeError("DbgPrint string address overflows");
    auto Terminator =
        readInteger(Model, Memory, Address + uint64_t(Limit) * Step, Step);
    if (!Terminator)
      return Terminator.takeError();
    if (*Terminator)
      return runtimeError("unterminated DbgPrint string within model limit");
    return Text;
  }

  llvm::Expected<std::string> countedString(uint64_t Address,
                                            const FormatSpec &Spec) {
    if (Model.currentIRQL() != scheduler::PassiveLevel)
      return runtimeError("Unicode DbgPrint requires IRQL PASSIVE_LEVEL");
    if (Spec.Length != LengthKind::Wide && Spec.Length != LengthKind::Long)
      return runtimeError("DbgPrint counted strings require %wZ or %lZ");
    auto Record = readCountedString(Model, Memory, Address);
    if (!Record)
      return Record.takeError();
    if (!Record->Buffer)
      return NullString.take_front(Spec.Precision.value_or(MaxDebugBytes))
          .str();
    unsigned Units = Record->Length / 2;
    if (Spec.Precision)
      Units = std::min(Units, *Spec.Precision);
    if (Units > MaxDebugBytes)
      return runtimeError("DbgPrint counted string exceeds model limit");
    auto Bytes = readBytes(Model, Memory, Record->Buffer, Units * 2);
    if (!Bytes)
      return Bytes.takeError();
    std::string Text;
    for (unsigned I = 0; I < Units; ++I) {
      uint16_t Unit = codeUnit(*Bytes, I * 2);
      if (Unit > MaxASCII)
        return runtimeError(
            "DbgPrint non-ASCII text requires an unmodeled code page");
      Text.push_back(static_cast<char>(Unit));
    }
    return Text;
  }

  llvm::Error render(Conversion Kind, const FormatSpec &Spec, uint64_t Value) {
    std::string Prefix;
    std::string Text;
    const bool Numeric = Kind <= Conversion::Pointer;
    if (Numeric) {
      if (Spec.Length == LengthKind::Wide)
        return runtimeError(
            "wide modifier is invalid for numeric DbgPrint formats");
      const bool Pointer = Kind == Conversion::Pointer;
      if (Pointer && (Spec.Length != LengthKind::None || Spec.Precision ||
                      Spec.Plus || Spec.Space || Spec.Alternate))
        return runtimeError("unsupported DbgPrint pointer format modifiers");
      const unsigned Bits = Pointer ? 64 : Spec.Bits;
      if (Bits < 64)
        Value &= (uint64_t(1) << Bits) - 1;
      bool Negative = false;
      if (Kind == Conversion::SignedDecimal) {
        Negative = Value & (uint64_t(1) << (Bits - 1));
        if (Negative) {
          Value = ~Value + 1;
          if (Bits < 64)
            Value &= (uint64_t(1) << Bits) - 1;
          Prefix = "-";
        } else if (Spec.Plus)
          Prefix = "+";
        else if (Spec.Space)
          Prefix = " ";
      }
      const bool Hex = Kind == Conversion::HexLower ||
                       Kind == Conversion::HexUpper || Pointer;
      const unsigned Base = Hex ? 16 : Kind == Conversion::Octal ? 8 : 10;
      const auto Digits =
          Kind == Conversion::HexUpper || Pointer ? UpperDigits : LowerDigits;
      uint64_t Remaining = Value;
      do {
        Text.push_back(Digits[Remaining % Base]);
        Remaining /= Base;
      } while (Remaining);
      std::reverse(Text.begin(), Text.end());
      const unsigned Precision = Pointer ? 16 : Spec.Precision.value_or(1);
      if (!Precision && !Value)
        Text.clear();
      if (Text.size() < Precision)
        Text.insert(0, Precision - Text.size(), '0');
      if (Spec.Alternate) {
        if (Hex && Value)
          Prefix = Kind == Conversion::HexUpper ? "0X" : "0x";
        else if (Kind == Conversion::Octal &&
                 (Text.empty() || Text.front() != '0'))
          Text.insert(Text.begin(), '0');
      }
    } else if (Kind == Conversion::String || Kind == Conversion::Character) {
      const bool Wide =
          Spec.Length == LengthKind::Wide || Spec.Length == LengthKind::Long;
      if (Wide && Model.currentIRQL() != scheduler::PassiveLevel)
        return runtimeError("Unicode DbgPrint requires IRQL PASSIVE_LEVEL");
      if (Spec.Length != LengthKind::None && Spec.Length != LengthKind::Short &&
          !Wide)
        return runtimeError(
            "unsupported DbgPrint character or string modifier");
      if (Kind == Conversion::String) {
        auto Result = string(Value, Wide, Spec.Precision);
        if (!Result)
          return Result.takeError();
        Text = std::move(*Result);
      } else {
        const uint64_t Unit = Wide ? uint16_t(Value) : uint8_t(Value);
        if (Unit > MaxASCII)
          return runtimeError(
              "DbgPrint non-ASCII character requires an unmodeled code page");
        Text.push_back(static_cast<char>(Unit));
      }
    } else {
      auto Result = countedString(Value, Spec);
      if (!Result)
        return Result.takeError();
      Text = std::move(*Result);
    }
    const size_t Size = Prefix.size() + Text.size();
    const size_t Padding = Spec.Width > Size ? Spec.Width - Size : 0;
    const bool PadZero = Numeric && Spec.Zero && !Spec.Left && !Spec.Precision;
    if (!Spec.Left && !PadZero)
      if (auto E = append(std::string(Padding, ' ')))
        return E;
    if (auto E = append(Prefix))
      return E;
    if (PadZero)
      if (auto E = append(std::string(Padding, '0')))
        return E;
    if (auto E = append(Text))
      return E;
    if (Spec.Left)
      return append(std::string(Padding, ' '));
    return llvm::Error::success();
  }

public:
  Formatter(const KernelModel &Model, GuestMemory &Memory,
            ArgumentReader ReadArgument, unsigned FirstArgument)
      : Model(Model), Memory(Memory), ReadArgument(ReadArgument),
        NextArgument(FirstArgument) {}

  llvm::Expected<std::string> format(uint64_t Address) {
    std::string Format;
    for (unsigned I = 0;; ++I) {
      if (I > MaxDebugFormatBytes || Address > UINT64_MAX - I)
        return runtimeError("unterminated DbgPrint format within model limit");
      auto Byte = readInteger(Model, Memory, Address + I, 1);
      if (!Byte)
        return Byte.takeError();
      if (!*Byte)
        break;
      if (*Byte > MaxASCII)
        return runtimeError("DbgPrint format must contain ASCII text");
      Format.push_back(static_cast<char>(*Byte));
    }
    for (size_t Position = 0; Position < Format.size();) {
      char Ch = Format[Position++];
      if (Ch != '%') {
        if (auto E = append(llvm::StringRef(&Ch, 1)))
          return E;
        continue;
      }
      if (Position < Format.size() && Format[Position] == '%') {
        ++Position;
        if (auto E = append("%"))
          return E;
        continue;
      }
      auto Spec = spec(Format, Position);
      if (!Spec)
        return Spec.takeError();
      if (Position == Format.size())
        return runtimeError(
            "DbgPrint ends with an incomplete format specifier");
      auto Kind = conversion(Format[Position++]);
      if (!Kind)
        return runtimeError("unsupported DbgPrint conversion specifier");
      auto Value = argument();
      if (!Value)
        return Value.takeError();
      if (auto E = render(*Kind, *Spec, *Value))
        return E;
    }
    return Output;
  }
};
} // namespace

llvm::Expected<uint64_t> unicodeOperation(const KernelModel &Model,
                                          GuestMemory &Memory,
                                          UnicodeOperation Operation,
                                          llvm::ArrayRef<uint64_t> A) {
  auto First = readCountedString(Model, Memory, A[0]);
  if (!First)
    return First.takeError();
  if (Operation == UnicodeOperation::Copy) {
    unsigned Length = 0;
    std::vector<uint8_t> Bytes;
    if (A[1]) {
      auto Source = readCountedString(Model, Memory, A[1]);
      if (!Source)
        return Source.takeError();
      Length = std::min(First->MaximumLength, Source->Length);
      auto Read = readBytes(Model, Memory, Source->Buffer, Length);
      if (!Read)
        return Read.takeError();
      Bytes = std::move(*Read);
      // A non-null source also terminates the result when a complete UTF-16
      // code unit fits. Length still describes only the copied source bytes.
      if (Length + 2 <= First->MaximumLength)
        Bytes.resize(Length + 2, 0);
    }
    if (auto E = Model.validateGuestAccess(A[0], 2, true))
      return E;
    if (auto E = Model.validateGuestAccess(First->Buffer, Bytes.size(), true))
      return E;
    if (!Bytes.empty())
      if (auto E = Memory.write(First->Buffer, Bytes))
        return E;
    if (auto E = Memory.writeInteger(A[0], Length, 2))
      return E;
    return 0;
  }
  if (static_cast<uint8_t>(A[2]))
    return runtimeError("case-insensitive Unicode comparison requires an "
                        "unmodeled Windows case table");
  auto Second = readCountedString(Model, Memory, A[1]);
  if (!Second)
    return Second.takeError();
  auto Left = readBytes(Model, Memory, First->Buffer, First->Length);
  if (!Left)
    return Left.takeError();
  auto Right = readBytes(Model, Memory, Second->Buffer, Second->Length);
  if (!Right)
    return Right.takeError();
  int32_t Difference = int32_t(First->Length) - Second->Length;
  for (size_t I = 0, End = std::min(Left->size(), Right->size()); I < End;
       I += 2)
    if (codeUnit(*Left, I) != codeUnit(*Right, I)) {
      Difference = int32_t(codeUnit(*Left, I)) - codeUnit(*Right, I);
      break;
    }
  if (Operation == UnicodeOperation::Equal)
    return Difference == 0;
  return static_cast<uint32_t>(Difference);
}

llvm::Expected<uint64_t> initializeAnsiString(const KernelModel &Model,
                                              GuestMemory &Memory,
                                              llvm::ArrayRef<uint64_t> A) {
  // ntoskrnl (RVA 0x24fb00) scans to the terminator, truncates the counted
  // length to 0xFFFE and counts the terminator in MaximumLength.
  uint64_t Length = 0;
  if (A[1]) {
    for (;; ++Length) {
      if (auto E = checkBufferRange(A[1], Length + 1))
        return E;
      auto Byte = readInteger(Model, Memory, A[1] + Length, 1);
      if (!Byte)
        return Byte.takeError();
      if (!*Byte)
        break;
    }
    Length = std::min<uint64_t>(Length, 0xfffe);
  }
  // The routine clears the whole first quadword, including the padding.
  if (auto E = Model.validateGuestAccess(A[0], UnicodeRecordSize, true))
    return E;
  if (auto E = Memory.writeInteger(A[0], 0, 8))
    return E;
  if (auto E = Memory.writeInteger(A[0], Length, 2))
    return E;
  if (auto E = Memory.writeInteger(A[0] + UnicodeMaximumOffset,
                                   A[1] ? Length + 1 : 0, 2))
    return E;
  if (auto E = Memory.writeInteger(A[0] + UnicodeBufferOffset, A[1], 8))
    return E;
  return StatusSuccess;
}

llvm::Expected<uint64_t> appendUnicodeString(const KernelModel &Model,
                                             GuestMemory &Memory,
                                             llvm::ArrayRef<uint64_t> A,
                                             bool CountedSource) {
  uint64_t Source = A[1];
  uint64_t Length = 0;
  if (CountedSource) {
    // An empty counted source succeeds before the destination is read
    // (RVA 0x3480c0).
    auto Record = readCountedString(Model, Memory, A[1]);
    if (!Record)
      return Record.takeError();
    if (!Record->Length)
      return StatusSuccess;
    Source = Record->Buffer;
    Length = Record->Length;
  } else {
    // A null source is an empty append (RVA 0x32ead0). Others are measured
    // like RtlInitUnicodeStringEx (RVA 0x32eb60), which rejects sources
    // longer than 0x7FFE units; the append then reports a short buffer.
    if (!Source)
      return StatusSuccess;
    for (;; Length += 2) {
      if (auto E = checkBufferRange(Source, Length + 2))
        return E;
      auto Unit = readInteger(Model, Memory, Source + Length, 2);
      if (!Unit)
        return Unit.takeError();
      if (!*Unit)
        break;
    }
    if (Length > MaxUnicodeBytes)
      return StatusBufferTooSmall;
  }
  auto Destination = readCountedString(Model, Memory, A[0]);
  if (!Destination)
    return Destination.takeError();
  const uint64_t Combined = Destination->Length + Length;
  if (Combined > Destination->MaximumLength)
    return StatusBufferTooSmall;
  auto Bytes = readBytes(Model, Memory, Source, Length);
  if (!Bytes)
    return Bytes.takeError();
  // The result is terminated only when a complete UTF-16 unit still fits.
  if (Combined + 1 < Destination->MaximumLength)
    Bytes->resize(Length + 2, 0);
  const uint64_t Target = Destination->Buffer + Destination->Length;
  if (auto E = Model.validateGuestAccess(A[0], 2, true))
    return E;
  if (auto E = Model.validateGuestAccess(Target, Bytes->size(), true))
    return E;
  if (!Bytes->empty())
    if (auto E = Memory.write(Target, *Bytes))
      return E;
  if (auto E = Memory.writeInteger(A[0], Combined, 2))
    return E;
  return StatusSuccess;
}

llvm::Expected<uint64_t> prefixUnicodeString(const KernelModel &Model,
                                             GuestMemory &Memory,
                                             llvm::ArrayRef<uint64_t> A) {
  auto Prefix = readCountedString(Model, Memory, A[0]);
  if (!Prefix)
    return Prefix.takeError();
  auto String = readCountedString(Model, Memory, A[1]);
  if (!String)
    return String.takeError();
  // RVA 0x5edbe0 compares only the prefix's counted units. Equal units never
  // consult the case table, even for a case-insensitive request.
  if (String->Length < Prefix->Length)
    return 0;
  if (!Prefix->Length)
    return 1;
  auto Left = readBytes(Model, Memory, Prefix->Buffer, Prefix->Length);
  if (!Left)
    return Left.takeError();
  auto Right = readBytes(Model, Memory, String->Buffer, Prefix->Length);
  if (!Right)
    return Right.takeError();
  for (size_t I = 0; I < Left->size(); I += 2)
    if (codeUnit(*Left, I) != codeUnit(*Right, I)) {
      if (static_cast<uint8_t>(A[2]))
        return runtimeError("case-insensitive Unicode prefix requires an "
                            "unmodeled Windows case table");
      return 0;
    }
  return 1;
}

llvm::Expected<uint64_t> integerToUnicodeString(const KernelModel &Model,
                                                GuestMemory &Memory,
                                                llvm::ArrayRef<uint64_t> A) {
  // RtlIntegerToChar (RVA 0x6f7000) formats the unsigned value with uppercase
  // digits and no padding before rejecting any record access.
  uint32_t Value = static_cast<uint32_t>(A[0]);
  uint32_t Base = static_cast<uint32_t>(A[1]);
  if (!Base)
    Base = 10;
  unsigned Shift = 0;
  switch (Base) {
  case 2:
    Shift = 1;
    break;
  case 8:
    Shift = 3;
    break;
  case 10:
    break;
  case 16:
    Shift = 4;
    break;
  default:
    return StatusInvalidParameter;
  }
  static constexpr char Digits[] = "0123456789ABCDEF";
  char Reversed[32];
  unsigned Count = 0;
  do {
    uint32_t Digit;
    if (Shift) {
      Digit = Value & ((1u << Shift) - 1);
      Value >>= Shift;
    } else {
      Digit = Value % Base;
      Value /= Base;
    }
    Reversed[Count++] = Digits[Digit];
  } while (Value);
  if (!A[2])
    return runtimeError("UNICODE_STRING record must not be null");
  if (auto E = Model.validateGuestAccess(A[2], UnicodeRecordSize, false))
    return E;
  auto Maximum = Memory.readInteger(A[2] + UnicodeMaximumOffset, 2);
  if (!Maximum)
    return Maximum.takeError();
  auto Buffer = Memory.readInteger(A[2] + UnicodeBufferOffset, 8);
  if (!Buffer)
    return Buffer.takeError();
  // RtlAnsiStringToUnicodeString (RVA 0x6f6840) stores Length before checking
  // that MaximumLength also holds the terminator.
  const uint64_t Length = Count * 2;
  if (auto E = Model.validateGuestAccess(A[2], 2, true))
    return E;
  if (auto E = Memory.writeInteger(A[2], Length, 2))
    return E;
  if (Length + 2 > *Maximum)
    return StatusBufferOverflow;
  std::vector<uint8_t> Bytes(Length + 2, 0);
  for (unsigned I = 0; I < Count; ++I)
    Bytes[I * 2] = static_cast<uint8_t>(Reversed[Count - 1 - I]);
  if (auto E = Model.validateGuestAccess(*Buffer, Bytes.size(), true))
    return E;
  if (auto E = Memory.write(*Buffer, Bytes))
    return E;
  return StatusSuccess;
}

llvm::Expected<uint64_t> unicodeStringToInteger(const KernelModel &Model,
                                                GuestMemory &Memory,
                                                llvm::ArrayRef<uint64_t> A) {
  // RVA 0x6638e0: empty or odd lengths and unsupported radices store zero and
  // fail; other text succeeds and stops at the first invalid digit.
  if (!A[0])
    return runtimeError("UNICODE_STRING record must not be null");
  if (auto E = Model.validateGuestAccess(A[0], UnicodeRecordSize, false))
    return E;
  auto Length = Memory.readInteger(A[0], 2);
  if (!Length)
    return Length.takeError();
  auto Buffer = Memory.readInteger(A[0] + UnicodeBufferOffset, 8);
  if (!Buffer)
    return Buffer.takeError();
  if (auto E = Model.validateGuestAccess(A[2], 4, true))
    return E;
  auto Store = [&](uint32_t Value,
                   uint64_t Status) -> llvm::Expected<uint64_t> {
    if (auto E = Memory.writeInteger(A[2], Value, 4))
      return E;
    return Status;
  };
  if (!*Length || (*Length & 1))
    return Store(0, StatusInvalidParameter);
  auto Bytes = readBytes(Model, Memory, *Buffer, *Length);
  if (!Bytes)
    return Bytes.takeError();
  // Take yields zero once the counted text is exhausted, like the routine's
  // remaining-unit checks; an embedded zero unit also ends the digits.
  size_t Index = 0;
  size_t Remaining = *Length / 2;
  auto Take = [&]() -> uint16_t {
    if (!Remaining)
      return 0;
    --Remaining;
    return codeUnit(*Bytes, 2 * Index++);
  };
  // Units up to and including U+0020 are leading whitespace.
  uint16_t First = 0;
  while (Remaining) {
    const uint16_t Unit = Take();
    if (Unit > 0x20) {
      First = Unit;
      break;
    }
  }
  uint16_t Current = First;
  if (First == u'+' || First == u'-')
    Current = Take();
  uint32_t Base = static_cast<uint32_t>(A[1]);
  unsigned Shift = 0;
  if (!Base) {
    // Only a zero radix recognizes 0x, 0o and 0b. Any other unit after the
    // leading zero is read again as the first digit.
    Base = 10;
    if (Current == u'0') {
      if (Remaining) {
        const size_t SavedIndex = Index;
        const size_t SavedRemaining = Remaining;
        switch (Take()) {
        case u'x':
          Base = 16;
          Shift = 4;
          break;
        case u'o':
          Base = 8;
          Shift = 3;
          break;
        case u'b':
          Base = 2;
          Shift = 1;
          break;
        default:
          Index = SavedIndex;
          Remaining = SavedRemaining;
          break;
        }
      }
      Current = Take();
    }
  } else {
    switch (Base) {
    case 2:
      Shift = 1;
      break;
    case 8:
      Shift = 3;
      break;
    case 10:
      break;
    case 16:
      Shift = 4;
      break;
    default:
      return Store(0, StatusInvalidParameter);
    }
  }
  uint32_t Value = 0;
  while (Current) {
    uint32_t Digit;
    if (uint16_t(Current - u'0') <= 9)
      Digit = Current - u'0';
    else if (uint16_t(Current - u'A') <= 5)
      Digit = Current - u'A' + 10;
    else if (uint16_t(Current - u'a') <= 5)
      Digit = Current - u'a' + 10;
    else
      break;
    if (Digit >= Base)
      break;
    // The routine accumulates without overflow checks in 32 bits.
    Value = Shift ? Digit | (Value << Shift) : Digit + Base * Value;
    Current = Take();
  }
  if (First == u'-')
    Value = 0u - Value;
  return Store(Value, StatusSuccess);
}

llvm::Expected<std::string> formatDebugMessage(const KernelModel &Model,
                                               GuestMemory &Memory,
                                               uint64_t Format,
                                               unsigned FirstArgument,
                                               ArgumentReader ReadArgument) {
  return Formatter(Model, Memory, ReadArgument, FirstArgument).format(Format);
}
} // namespace neverd::emulation::runtime
