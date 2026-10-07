#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neverd::worker {

/// Display role of a span of formatted listing text (ListingRoles.def).
enum class ListingRole : std::uint8_t {
#define NEVERD_LISTING_ROLE(Name, Code) Name = Code,
#include "ListingRoles.def"
};

/// Operand text dialect of an engine architecture.
enum class OperandDialect : std::uint8_t { X86, AArch64, ARM, Generic };
OperandDialect operandDialect(std::string_view architecture);

struct TextSpan {
  std::uint32_t start = 0;
  std::uint32_t length = 0;
  ListingRole role = ListingRole::Plain;
  /// Location a name span denotes, for navigation.
  std::optional<std::uint64_t> address;
};

/// Text plus non-overlapping role spans in ascending order.
class StyledText {
public:
  void append(std::string_view text, ListingRole role,
              std::optional<std::uint64_t> address = std::nullopt);
  /// Pad with spaces to \p column, or add one space when already past it.
  void padTo(std::size_t column);
  void append(const StyledText &other);
  const std::string &text() const { return text_; }
  const std::vector<TextSpan> &spans() const { return spans_; }
  bool empty() const { return text_.empty(); }

private:
  std::string text_;
  std::vector<TextSpan> spans_;
};

/// How an operand reaches a location; it selects an automatic name when the
/// location has none.
enum class NameUse : std::uint8_t { Transfer, Data, Address, Slot };

struct LocationName {
  std::string text;
  ListingRole role = ListingRole::Plain;
  std::uint64_t address = 0;
};

/// Names a location for display.  \p SizeKeyword is the operand size keyword
/// (for example "qword") when the operand states one.
using LocationNamer = std::function<LocationName(
    std::uint64_t Address, NameUse Use, std::string_view SizeKeyword)>;

/// A stack variable that a frame-relative operand falls in.
struct FrameSlot {
  std::string name;
  /// Offset of the operand inside the variable.
  std::int64_t delta = 0;
  /// Bytes the variable spans.
  unsigned size = 0;
};

/// Names the frame variable at an offset from the frame register.
using FrameNamer = std::function<std::optional<FrameSlot>(std::int64_t)>;

struct OperandFacts {
  OperandDialect dialect = OperandDialect::Generic;
  std::string_view mnemonic;
  std::uint64_t address = 0;
  std::uint32_t size = 0;
  bool wide = true;
  /// Engine flow kind: "", "call", "icall", "jump", "cjump", "ijump", "ret".
  std::string_view flow;
  std::optional<std::uint64_t> target;
  /// Constant locations the instruction reads, writes or takes the address of.
  std::vector<std::uint64_t> references;
  /// Instruction bytes as lowercase hexadecimal, or empty.
  std::string_view bytes;
  /// The variables of the function's frame and the frame register, when its
  /// prologue sets one up.
  std::string_view frameRegister;
  const FrameNamer *frame = nullptr;
  /// The stack register and its distance below the frame's base at this
  /// instruction, when known: operands through it name frame variables too.
  std::string_view stackRegister;
  std::optional<std::int64_t> stackDepth;
};

/// A memory operand relative to a frame register.
struct FrameAccess {
  std::int64_t offset = 0;
  /// Bytes accessed, or zero when only the address is taken.
  unsigned size = 0;
};

/// The memory operand of x86 operand text based on \p frameRegister.
std::optional<FrameAccess> x86FrameAccess(std::string_view operands,
                                          std::string_view frameRegister);

/// The x86 size keyword of \p bytes ("qword" for 8), or empty.
std::string_view x86SizeKeyword(unsigned bytes);

/// An instruction's mnemonic and operand text as classic listings spell it.
struct ClassicInstruction {
  std::string mnemonic, operands;
};

/// Respell engine disassembler text for \p dialect: classic mnemonics, hidden
/// prefixes, implicit string operands and folded operand forms.  The
/// instruction is the same; only its spelling changes.
ClassicInstruction classicInstruction(OperandDialect dialect,
                                      std::string_view mnemonic,
                                      std::string_view operands,
                                      std::string_view bytes);

/// IDA-compatible x86 numeric spelling: decimal below ten, otherwise uppercase
/// hexadecimal with an `h` suffix and a leading zero before a letter.
std::string x86Number(std::uint64_t value);

/// Format disassembler operand text, replacing exact transfer targets and
/// PC-relative or absolute memory locations with their names.
StyledText formatOperands(std::string_view operands, const OperandFacts &facts,
                          const LocationNamer &namer);

/// Classify one identifier of operand text for \p dialect.
bool isRegisterName(OperandDialect dialect, std::string_view name);
bool isOperandKeyword(std::string_view name);
/// Automatic data-name prefix for a size keyword, or empty.
std::string_view dataNamePrefix(std::string_view sizeKeyword);
/// Parse an automatic name such as `loc_F2329` back to its address.
std::optional<std::uint64_t> parseDummyName(std::string_view name);
bool isPaddingMnemonic(std::string_view mnemonic);

} // namespace neverd::worker
