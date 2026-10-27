//===- OperandFormatDetail.h - Shared operand text helpers -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Operand text helpers the dialect formatters share: OperandFormat.cpp
/// tokenizes and formats x86 and generic operands, OperandFormatARM.cpp the
/// ARM family's.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_WORKER_OPERANDFORMATDETAIL_H
#define NEVERD_TOOLS_NEVERD_WORKER_OPERANDFORMATDETAIL_H

#include "OperandFormat.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neverd::worker::operand_format_detail {

enum class TokenKind : std::uint8_t { Identifier, Number, Punct, Space };
struct Token {
  TokenKind kind = TokenKind::Punct;
  std::string_view text;
  std::uint64_t value = 0;
  bool valid = false;
};
/// Tokens of one operand, without surrounding spaces.
using OperandTokens = std::vector<Token>;

std::vector<Token> tokenize(std::string_view text);
/// Operands of \p tokens: commas outside brackets, braces and parentheses
/// separate them.
std::vector<OperandTokens> splitOperands(const std::vector<Token> &tokens);
/// The value of an operand that is a plain number, `5` or `-5`, unsigned in
/// \p mask's width.
std::optional<std::uint64_t> plainNumber(const OperandTokens &tokens,
                                         std::uint64_t mask);
std::string lower(std::string_view text);
ListingRole identifierRole(OperandDialect dialect, std::string_view text);
bool isTransfer(std::string_view flow);
/// \p value as characters, most significant byte first (`'ABCD'`); none
/// unless every byte but leading zeros prints.
std::optional<std::string> characterLiteral(std::uint64_t value);
/// The binary digits of \p value without leading zeros.
std::string binaryDigits(std::uint64_t value);
/// Operand text as the decoder spells it, with exact transfer targets and
/// referenced locations named.
void formatGenericOperand(StyledText &out, const OperandTokens &tokens,
                          const OperandFacts &facts,
                          const LocationNamer &namer);

/// Whether \p dialect spells operands as the ARM family does: `#0x1a`,
/// `[x20, #0x10]`, `{r4, lr}`.
bool armDialect(OperandDialect dialect);
/// The value of an ARM-family immediate, `#5`, `#-5` or `#0x1a`, unsigned in
/// \p mask's width; none for anything else, a floating constant included.
std::optional<std::uint64_t> armImmediate(const OperandTokens &tokens,
                                          std::uint64_t mask);
/// Whether a structure offset can name an ARM-family operand: an immediate,
/// or a memory operand of a base register and an immediate displacement.
bool armStructOperand(const OperandTokens &tokens, OperandDialect dialect);
/// What an ARM-family instruction's formats need: the width its immediates
/// count in, and the bytes a load or store moves through each register.
struct ArmWidths {
  unsigned immediateBits = 0;
  unsigned accessBytes = 0;
};
ArmWidths armWidths(OperandDialect dialect, std::string_view mnemonic,
                    const std::vector<OperandTokens> &operands);
/// Format an ARM-family operand as the user's \p format shows it; without
/// one, or when the format cannot show it, as formatGenericOperand does.
void formatArmOperand(StyledText &out, const OperandTokens &tokens,
                      const OperandFacts &facts, const LocationNamer &namer,
                      const NumberFormat *format, const ArmWidths &widths);

} // namespace neverd::worker::operand_format_detail

#endif // NEVERD_TOOLS_NEVERD_WORKER_OPERANDFORMATDETAIL_H
