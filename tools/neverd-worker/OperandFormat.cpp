#include "OperandFormat.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <string>
#include <unordered_set>

namespace neverd::worker {
namespace {
struct RegisterTable {
  std::unordered_set<std::string> x86, aarch64, arm;
};

void addRange(std::unordered_set<std::string> &set, std::string_view prefix,
              int first, int last, std::string_view suffix) {
  for (int index = first; index <= last; ++index)
    set.insert(std::string(prefix) + std::to_string(index) +
               std::string(suffix));
}

const RegisterTable &registerTable() {
  static const RegisterTable table = [] {
    RegisterTable result;
    const auto family =
        [&](std::string_view name) -> std::unordered_set<std::string> & {
      if (name == "X86")
        return result.x86;
      if (name == "AArch64")
        return result.aarch64;
      return result.arm;
    };
#define NEVERD_REGISTER_NAME(Family, Name) family(#Family).insert(Name);
#define NEVERD_REGISTER_RANGE(Family, Prefix, First, Last, Suffix)             \
  addRange(family(#Family), Prefix, First, Last, Suffix);
#include "ListingVocabulary.def"
    return result;
  }();
  return table;
}

bool isIdentifierStart(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$' ||
         c == '%' || c == '@' || c == '?';
}
bool isIdentifierPart(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' ||
         c == '@' || c == '?' || c == '.';
}
std::string lower(std::string_view text) {
  std::string result(text);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return result;
}

enum class TokenKind : std::uint8_t { Identifier, Number, Punct, Space };
struct Token {
  TokenKind kind = TokenKind::Punct;
  std::string_view text;
  std::uint64_t value = 0;
  bool valid = false;
};

std::vector<Token> tokenize(std::string_view text) {
  std::vector<Token> tokens;
  std::size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    const std::size_t start = i;
    Token token;
    if (std::isspace(static_cast<unsigned char>(c))) {
      while (i < text.size() &&
             std::isspace(static_cast<unsigned char>(text[i])))
        ++i;
      token.kind = TokenKind::Space;
    } else if (std::isdigit(static_cast<unsigned char>(c))) {
      token.kind = TokenKind::Number;
      int base = 10;
      std::size_t digits = i;
      if (c == '0' && i + 1 < text.size() &&
          (text[i + 1] == 'x' || text[i + 1] == 'X')) {
        base = 16;
        digits = i + 2;
      }
      i = digits;
      while (i < text.size() &&
             std::isxdigit(static_cast<unsigned char>(text[i])) &&
             (base == 16 || std::isdigit(static_cast<unsigned char>(text[i]))))
        ++i;
      const auto parsed = std::from_chars(text.data() + digits, text.data() + i,
                                          token.value, base);
      token.valid = parsed.ec == std::errc{} && parsed.ptr == text.data() + i &&
                    i > digits;
      // A digit run fused to letters (an AArch64 arrangement such as `16b`)
      // is one identifier, not a number.
      if (i < text.size() && isIdentifierPart(text[i]) && text[i] != '.') {
        while (i < text.size() && isIdentifierPart(text[i]))
          ++i;
        token.kind = TokenKind::Identifier;
        token.valid = false;
      }
    } else if (isIdentifierStart(c)) {
      token.kind = TokenKind::Identifier;
      while (i < text.size() && isIdentifierPart(text[i]))
        ++i;
    } else {
      token.kind = TokenKind::Punct;
      ++i;
    }
    token.text = text.substr(start, i - start);
    tokens.push_back(token);
  }
  return tokens;
}

ListingRole identifierRole(OperandDialect dialect, std::string_view text) {
  if (isRegisterName(dialect, text))
    return ListingRole::Register;
  if (isOperandKeyword(lower(text)))
    return ListingRole::Keyword;
  return ListingRole::Plain;
}

bool isTransfer(std::string_view flow) {
  return flow == "call" || flow == "jump" || flow == "cjump";
}

/// Tokens of one operand, without surrounding spaces.
using OperandTokens = std::vector<Token>;

std::vector<OperandTokens> splitOperands(const std::vector<Token> &tokens) {
  std::vector<OperandTokens> operands(1);
  int depth = 0;
  for (const Token &token : tokens) {
    if (token.kind == TokenKind::Punct) {
      if (token.text == "[" || token.text == "{" || token.text == "(")
        ++depth;
      else if ((token.text == "]" || token.text == "}" || token.text == ")") &&
               depth > 0)
        --depth;
      else if (token.text == "," && depth == 0) {
        operands.emplace_back();
        continue;
      }
    }
    operands.back().push_back(token);
  }
  for (auto &operand : operands) {
    while (!operand.empty() && operand.front().kind == TokenKind::Space)
      operand.erase(operand.begin());
    while (!operand.empty() && operand.back().kind == TokenKind::Space)
      operand.pop_back();
  }
  return operands;
}

void appendX86Number(StyledText &out, const Token &token) {
  if (token.valid)
    out.append(x86Number(token.value), ListingRole::Number);
  else
    out.append(token.text, ListingRole::Number);
}

/// Effective address of an x86 memory operand that names one fixed location:
/// a RIP/EIP base without index, or a bare absolute displacement.
std::optional<std::uint64_t> fixedX86Location(const std::vector<Token> &inner,
                                              const OperandFacts &facts,
                                              bool &pcRelative) {
  std::string base;
  int registers = 0;
  std::uint64_t displacement = 0;
  bool negative = false, sawNumber = false;
  for (const Token &token : inner) {
    if (token.kind == TokenKind::Identifier) {
      if (!isRegisterName(OperandDialect::X86, token.text))
        return std::nullopt;
      base = lower(token.text);
      ++registers;
    } else if (token.kind == TokenKind::Number) {
      if (!token.valid || sawNumber)
        return std::nullopt;
      displacement = token.value;
      sawNumber = true;
    } else if (token.kind == TokenKind::Punct) {
      if (token.text == "-")
        negative = true;
      else if (token.text != "+")
        return std::nullopt;
    }
  }
  const std::uint64_t signedDisplacement =
      negative ? ~displacement + 1 : displacement;
  const std::uint64_t next = facts.address + facts.size;
  if (registers == 1 && (base == "rip" || base == "eip")) {
    pcRelative = true;
    std::uint64_t location = next + signedDisplacement;
    if (base == "eip")
      location &= 0xffffffffu;
    return location;
  }
  if (registers == 0 && sawNumber && !negative) {
    pcRelative = false;
    return displacement;
  }
  return std::nullopt;
}

void formatX86Operand(StyledText &out, const OperandTokens &tokens,
                      const OperandFacts &facts, const LocationNamer &namer) {
  const auto open =
      std::find_if(tokens.begin(), tokens.end(), [](const Token &t) {
        return t.kind == TokenKind::Punct && t.text == "[";
      });
  if (open != tokens.end()) {
    const auto close = std::find_if(open, tokens.end(), [](const Token &t) {
      return t.kind == TokenKind::Punct && t.text == "]";
    });
    const std::vector<Token> prefix(tokens.begin(), open);
    const std::vector<Token> inner(
        open + 1, close == tokens.end() ? tokens.end() : close);
    std::string sizeKeyword, segment;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
      if (prefix[i].kind != TokenKind::Identifier)
        continue;
      const auto word = lower(prefix[i].text);
      if (!dataNamePrefix(word).empty())
        sizeKeyword = word;
      else if (i + 1 < prefix.size() && prefix[i + 1].text == ":")
        segment = word;
    }
    bool pcRelative = false;
    const auto location = fixedX86Location(inner, facts, pcRelative);
    if (location) {
      const bool address = facts.mnemonic == "lea";
      const NameUse use = address ? NameUse::Address
                          : (facts.flow == "icall" || facts.flow == "ijump")
                              ? NameUse::Slot
                              : NameUse::Data;
      const LocationName name = namer(*location, use, sizeKeyword);
      if (!name.text.empty()) {
        if (!address) {
          out.append(segment.empty() ? (pcRelative ? "cs" : "ds") : segment,
                     ListingRole::Register);
          out.append(":", ListingRole::Punctuation);
        }
        out.append(name.text, name.role, name.address);
        return;
      }
    }
    // Keep the size keywords; tighten the address expression.
    for (const Token &token : prefix) {
      if (token.kind == TokenKind::Space)
        out.append(" ", ListingRole::Plain);
      else if (token.kind == TokenKind::Identifier)
        out.append(token.text, identifierRole(OperandDialect::X86, token.text));
      else if (token.kind == TokenKind::Number)
        appendX86Number(out, token);
      else
        out.append(token.text, ListingRole::Punctuation);
    }
    out.append("[", ListingRole::Punctuation);
    for (const Token &token : inner) {
      if (token.kind == TokenKind::Space)
        continue;
      if (token.kind == TokenKind::Identifier)
        out.append(token.text, identifierRole(OperandDialect::X86, token.text));
      else if (token.kind == TokenKind::Number)
        appendX86Number(out, token);
      else
        out.append(token.text, ListingRole::Punctuation);
    }
    if (close != tokens.end()) {
      out.append("]", ListingRole::Punctuation);
      for (auto it = close + 1; it != tokens.end(); ++it) {
        if (it->kind == TokenKind::Identifier)
          out.append(it->text, identifierRole(OperandDialect::X86, it->text));
        else if (it->kind == TokenKind::Number)
          appendX86Number(out, *it);
        else if (it->kind != TokenKind::Space)
          out.append(it->text, ListingRole::Punctuation);
      }
    }
    return;
  }
  for (const Token &token : tokens) {
    switch (token.kind) {
    case TokenKind::Space:
      out.append(" ", ListingRole::Plain);
      break;
    case TokenKind::Identifier:
      out.append(token.text, identifierRole(OperandDialect::X86, token.text));
      break;
    case TokenKind::Number:
      if (token.valid && facts.target && token.value == *facts.target &&
          isTransfer(facts.flow)) {
        if (facts.flow != "call" && facts.size == 2)
          out.append("short ", ListingRole::Keyword);
        const LocationName name = namer(token.value, NameUse::Transfer, {});
        if (!name.text.empty()) {
          out.append(name.text, name.role, name.address);
          break;
        }
      }
      appendX86Number(out, token);
      break;
    case TokenKind::Punct:
      out.append(token.text, ListingRole::Punctuation);
      break;
    }
  }
}

void formatGenericOperand(StyledText &out, const OperandTokens &tokens,
                          const OperandFacts &facts,
                          const LocationNamer &namer) {
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const Token &token = tokens[i];
    // `#0x1000` naming an exact transfer target or referenced location.
    if (token.kind == TokenKind::Punct && token.text == "#" &&
        i + 1 < tokens.size() && tokens[i + 1].kind == TokenKind::Number &&
        tokens[i + 1].valid) {
      const std::uint64_t value = tokens[i + 1].value;
      const bool transfer =
          facts.target && value == *facts.target && isTransfer(facts.flow);
      const bool referenced =
          std::find(facts.references.begin(), facts.references.end(), value) !=
          facts.references.end();
      if (transfer || referenced) {
        const LocationName name =
            namer(value, transfer ? NameUse::Transfer : NameUse::Data, {});
        if (!name.text.empty()) {
          out.append(name.text, name.role, name.address);
          ++i;
          continue;
        }
      }
    }
    switch (token.kind) {
    case TokenKind::Space:
      out.append(token.text, ListingRole::Plain);
      break;
    case TokenKind::Identifier:
      out.append(token.text, identifierRole(facts.dialect, token.text));
      break;
    case TokenKind::Number:
      if (token.valid && facts.target && token.value == *facts.target &&
          isTransfer(facts.flow)) {
        const LocationName name = namer(token.value, NameUse::Transfer, {});
        if (!name.text.empty()) {
          out.append(name.text, name.role, name.address);
          break;
        }
      }
      out.append(token.text, ListingRole::Number);
      break;
    case TokenKind::Punct:
      out.append(token.text, token.text == "#" ? ListingRole::Number
                                               : ListingRole::Punctuation);
      break;
    }
  }
}
} // namespace

OperandDialect operandDialect(std::string_view architecture) {
  const auto name = lower(architecture);
  if (name == "x86_64" || name == "x86-64" || name == "x86" || name == "x64" ||
      name == "i386" || name == "amd64")
    return OperandDialect::X86;
  if (name == "aarch64" || name == "arm64")
    return OperandDialect::AArch64;
  if (name.starts_with("arm") || name.starts_with("thumb"))
    return OperandDialect::ARM;
  return OperandDialect::Generic;
}

void StyledText::append(std::string_view text, ListingRole role,
                        std::optional<std::uint64_t> address) {
  if (text.empty())
    return;
  if (role != ListingRole::Plain || address) {
    if (!address && !spans_.empty() && !spans_.back().address &&
        spans_.back().role == role &&
        spans_.back().start + spans_.back().length == text_.size())
      spans_.back().length += static_cast<std::uint32_t>(text.size());
    else
      spans_.push_back({static_cast<std::uint32_t>(text_.size()),
                        static_cast<std::uint32_t>(text.size()), role,
                        address});
  }
  text_ += text;
}

void StyledText::padTo(std::size_t column) {
  if (text_.size() < column)
    text_.append(column - text_.size(), ' ');
  else if (!text_.empty())
    text_ += ' ';
}

void StyledText::append(const StyledText &other) {
  const auto offset = static_cast<std::uint32_t>(text_.size());
  text_ += other.text_;
  for (TextSpan span : other.spans_) {
    span.start += offset;
    spans_.push_back(span);
  }
}

std::string x86Number(std::uint64_t value) {
  if (value < 10)
    return std::to_string(value);
  char buffer[24];
  const auto end = std::to_chars(buffer, buffer + sizeof buffer, value, 16).ptr;
  std::string digits(buffer, end);
  std::transform(digits.begin(), digits.end(), digits.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  if (std::isalpha(static_cast<unsigned char>(digits.front())))
    digits.insert(digits.begin(), '0');
  return digits + "h";
}

StyledText formatOperands(std::string_view operands, const OperandFacts &facts,
                          const LocationNamer &namer) {
  StyledText out;
  const auto pieces = splitOperands(tokenize(operands));
  bool first = true;
  for (const auto &operand : pieces) {
    if (operand.empty())
      continue;
    if (!first)
      out.append(", ", ListingRole::Punctuation);
    first = false;
    if (facts.dialect == OperandDialect::X86)
      formatX86Operand(out, operand, facts, namer);
    else
      formatGenericOperand(out, operand, facts, namer);
  }
  return out;
}

bool isRegisterName(OperandDialect dialect, std::string_view name) {
  const auto key = lower(name);
  const auto &table = registerTable();
  switch (dialect) {
  case OperandDialect::X86:
    return table.x86.contains(key) ||
           (key.starts_with("st(") && key.size() == 5 && key[4] == ')');
  case OperandDialect::AArch64: {
    // Vector registers may carry an arrangement (`v0.16b`) or lane (`v1.s`).
    const auto dot = key.find('.');
    return table.aarch64.contains(
        dot == std::string::npos ? key : key.substr(0, dot));
  }
  case OperandDialect::ARM:
    return table.arm.contains(key);
  case OperandDialect::Generic:
    return !key.empty() && key.front() == '$';
  }
  return false;
}

bool isOperandKeyword(std::string_view name) {
  static const std::unordered_set<std::string_view> keywords = {
#define NEVERD_OPERAND_KEYWORD(Spelling) Spelling,
#include "ListingVocabulary.def"
  };
  return keywords.contains(name);
}

std::string_view dataNamePrefix(std::string_view sizeKeyword) {
#define NEVERD_DATA_NAME_PREFIX(SizeKeyword, Prefix)                           \
  if (sizeKeyword == SizeKeyword)                                              \
    return Prefix;
#include "ListingVocabulary.def"
  return {};
}

std::optional<std::uint64_t> parseDummyName(std::string_view name) {
  static constexpr std::array prefixes = {
#define NEVERD_DUMMY_NAME_PREFIX(Prefix) std::string_view(Prefix),
#include "ListingVocabulary.def"
  };
  for (std::string_view prefix : prefixes) {
    if (!name.starts_with(prefix) || name.size() == prefix.size() ||
        name.size() - prefix.size() > 16)
      continue;
    std::uint64_t value = 0;
    const auto *begin = name.data() + prefix.size();
    const auto *end = name.data() + name.size();
    const auto parsed = std::from_chars(begin, end, value, 16);
    if (parsed.ec == std::errc{} && parsed.ptr == end)
      return value;
  }
  return std::nullopt;
}

bool isPaddingMnemonic(std::string_view mnemonic) {
  static const std::unordered_set<std::string_view> padding = {
#define NEVERD_PADDING_MNEMONIC(Spelling) Spelling,
#include "ListingVocabulary.def"
  };
  return padding.contains(mnemonic);
}

} // namespace neverd::worker
