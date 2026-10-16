//===- Runtime.cpp - Integral Dalvik method execution ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit, bounded value semantics over the shared DEX decoder's model.
//===----------------------------------------------------------------------===//
#include "neverd/dalvik/Runtime.h"

#include <bit>
#include <limits>
#include <type_traits>

namespace neverd::mobile::dalvik {
namespace {
bool integral(std::string_view type) {
  return type.size() == 1 &&
         std::string_view("ZBCSIJ").find(type[0]) != std::string_view::npos;
}
uint64_t narrow(uint64_t value, std::string_view type) {
  if (type == "J")
    return value;
  if (type == "B")
    return uint32_t(int32_t(std::bit_cast<int8_t>(uint8_t(value))));
  if (type == "S")
    return uint32_t(int32_t(std::bit_cast<int16_t>(uint16_t(value))));
  if (type == "C")
    return uint16_t(value);
  if (type == "Z")
    return uint8_t(value);
  return uint32_t(value);
}
struct Halt {
  ExecutionResult result;
};
[[noreturn]] void halt(ExecutionStop stop, const Method &method, uint32_t pc,
                       std::string message, std::string exception = {}) {
  throw Halt{{stop, 0, std::move(exception),
              method.reference.identity() + " at code-unit " +
                  std::to_string(pc) + ": " + message}};
}
struct Register {
  enum Kind { Empty, Int, WideLow, WideHigh } kind = Empty;
  uint32_t bits = 0;
};
struct Frame {
  const Method &method;
  uint32_t pc = 0;
  std::vector<Register> registers;
  explicit Frame(const Method &method)
      : method(method), registers(method.registers) {}
  [[noreturn]] void invalid(const char *message) const {
    halt(ExecutionStop::Invalid, method, pc, message);
  }
  void range(unsigned index, bool wide) const {
    if (index >= registers.size() || (wide && index + 1 >= registers.size()))
      invalid("register outside frame");
  }
  uint64_t get(unsigned index, bool wide = false) const {
    range(index, wide);
    if (registers[index].kind != (wide ? Register::WideLow : Register::Int) ||
        (wide && registers[index + 1].kind != Register::WideHigh))
      invalid("uninitialized or incompatible register value");
    return registers[index].bits |
           (wide ? uint64_t(registers[index + 1].bits) << 32 : 0);
  }
  void erase(unsigned index) {
    if (registers[index].kind == Register::WideLow)
      registers[index + 1].kind = Register::Empty;
    if (registers[index].kind == Register::WideHigh)
      registers[index - 1].kind = Register::Empty;
    registers[index].kind = Register::Empty;
  }
  void put(unsigned index, uint64_t bits, bool wide = false) {
    range(index, wide);
    erase(index);
    if (wide)
      erase(index + 1);
    registers[index] = {wide ? Register::WideLow : Register::Int,
                        uint32_t(bits)};
    if (wide)
      registers[index + 1] = {Register::WideHigh, uint32_t(bits >> 32)};
  }
};
template <class U> U arithmeticShift(U value, unsigned shift) {
  shift &= sizeof(U) * 8 - 1;
  if (!shift)
    return value;
  U high =
      value >> (sizeof(U) * 8 - 1) ? U(~U(0)) << (sizeof(U) * 8 - shift) : 0;
  return (value >> shift) | high;
}
template <class U>
U binary(std::string_view op, U left, U right, const Frame &frame) {
  using S = std::make_signed_t<U>;
  unsigned shift = unsigned(right) & (sizeof(U) * 8 - 1);
  if (op == "add")
    return left + right;
  if (op == "sub")
    return left - right;
  if (op == "rsub")
    return right - left;
  if (op == "mul")
    return left * right;
  if (op == "and")
    return left & right;
  if (op == "or")
    return left | right;
  if (op == "xor")
    return left ^ right;
  if (op == "shl")
    return left << shift;
  if (op == "ushr")
    return left >> shift;
  if (op == "shr")
    return arithmeticShift(left, shift);
  if (op == "div" || op == "rem") {
    if (!right)
      halt(ExecutionStop::Thrown, frame.method, frame.pc, "division by zero",
           "java/lang/ArithmeticException");
    S a = std::bit_cast<S>(left), b = std::bit_cast<S>(right);
    if (a == std::numeric_limits<S>::min() && b == -1)
      return op == "div" ? left : 0;
    return op == "div" ? U(a / b) : U(a % b);
  }
  halt(ExecutionStop::Unsupported, frame.method, frame.pc,
       "unsupported integer operation " + std::string(op));
}
} // namespace

struct Runtime::State {
  const Program &program;
  struct ClassState {
    enum Status {
      Prepared,
      Initializing,
      Initialized,
      Erroneous
    } status = Prepared;
    std::map<const Field *, uint64_t> fields;
  };
  std::map<const Class *, ClassState> classes;
  std::optional<ExecutionResult> stopped;
  struct Engine;
  ExecutionResult invoke(const std::function<uint64_t()> &body) {
    if (stopped)
      return *stopped;
    try {
      return {ExecutionStop::Returned, body(), {}, {}};
    } catch (const Halt &halt) {
      if (halt.result.stop != ExecutionStop::Thrown)
        stopped = halt.result;
      return halt.result;
    }
  }
};

struct Runtime::State::Engine {
  State &state;
  const Program &program;
  const AdmitInstruction &admit;
  uint64_t remaining_stack;
  Engine(State &state, const AdmitInstruction &admit, uint64_t stack)
      : state(state), program(state.program), admit(admit),
        remaining_stack(stack) {
    if (!admit)
      throw Error("DEX execution requires an instruction budget");
  }
  bool accessible(const std::string &owner, const Access &access,
                  const std::string &caller) const {
    if (owner == caller)
      return true;
    const auto *klass = program.findClass(owner);
    auto package = [](std::string_view name) {
      const auto slash = name.rfind('/');
      return slash == name.npos ? std::string_view() : name.substr(0, slash);
    };
    // Package/protected/private member resolution remains outside this subset;
    // a public member still cannot expose an inaccessible declaring class.
    return klass && has(access, "public") &&
           (has(klass->access, "public") || package(owner) == package(caller));
  }
  void initialize(std::string_view name, unsigned depth) {
    auto fail = [&](ExecutionStop stop, std::string message,
                    std::string exception = {}) {
      throw Halt{
          {stop, 0, std::move(exception), std::string(name) + ": " + message}};
    };
    if (auto reason = program.unsupportedClass(name))
      fail(ExecutionStop::Unsupported, *reason);
    const auto *klass = program.findClass(name);
    auto [entry, inserted] = state.classes.try_emplace(klass);
    auto &storage = entry->second;
    if (inserted) {
      // DEX encoded values are installed before <clinit>, including when a
      // recursive initialization reads this class's partially initialized
      // state.
      for (const auto &field : klass->fields) {
        uint64_t value = 0;
        const auto &type = field.reference.type;
        if (auto integer = std::get_if<int64_t>(&field.value)) {
          value = narrow(uint64_t(*integer), type);
          int64_t canonical =
              type == "J" ? std::bit_cast<int64_t>(value)
              : type == "C" || type == "Z"
                  ? int64_t(value)
                  : int64_t(std::bit_cast<int32_t>(uint32_t(value)));
          if (canonical != *integer || (type == "Z" && value > 1))
            fail(ExecutionStop::Invalid, "noncanonical static field value");
        } else if (auto boolean = std::get_if<bool>(&field.value)) {
          if (type != "Z")
            fail(ExecutionStop::Invalid, "boolean value in nonboolean field");
          value = *boolean;
        } else if (!std::holds_alternative<std::monostate>(field.value))
          fail(ExecutionStop::Invalid, "incompatible static field value");
        storage.fields.emplace(&field, value);
      }
    }
    if (storage.status == ClassState::Initialized ||
        storage.status == ClassState::Initializing)
      return;
    if (storage.status == ClassState::Erroneous)
      fail(ExecutionStop::Thrown, "class initialization previously failed",
           "java/lang/NoClassDefFoundError");
    storage.status = ClassState::Initializing;
    try {
      for (const auto &method : klass->methods)
        if (method.reference.name == "<clinit>") {
          if (!method.reference.parameters.empty() ||
              method.reference.returns != "V")
            fail(ExecutionStop::Invalid, "malformed class initializer");
          run(method, {}, depth, true);
        }
      storage.status = ClassState::Initialized;
    } catch (Halt &halt) {
      if (halt.result.stop == ExecutionStop::Thrown) {
        storage.status = ClassState::Erroneous;
        // ArithmeticException is the only non-Error Java throwable produced
        // by the integral executor. Nested initialization Errors propagate.
        if (halt.result.exception == "java/lang/ArithmeticException")
          halt.result.exception = "java/lang/ExceptionInInitializerError";
      }
      throw;
    }
  }
  uint64_t run(const Method &method, std::span<const uint64_t> args,
               unsigned depth, bool initializer = false) {
    if (auto reason = program.unsupported(method))
      halt(ExecutionStop::Unsupported, method, 0, *reason);
    if (method.reference.name.starts_with('<') && !initializer)
      halt(ExecutionStop::Invalid, method, 0,
           "explicit initializer invocation");
    initialize(method.reference.owner, depth);
    const uint64_t bytes = uint64_t(method.registers) * sizeof(Register) +
                           method.instructions.size() * 64 + 256;
    if (depth >= 64 || bytes > remaining_stack)
      halt(ExecutionStop::StackLimit, method, 0, "DEX frame budget exhausted");
    remaining_stack -= bytes;
    Frame frame(method);
    if (args.size() != method.reference.parameters.size() ||
        method.incomingWords() > method.registers)
      frame.invalid("argument/frame shape mismatch");
    unsigned input = method.registers - method.incomingWords();
    for (size_t i = 0; i < args.size(); ++i) {
      const auto &type = method.reference.parameters[i];
      auto bits = narrow(args[i], type);
      if (type == "Z" && bits > 1)
        frame.invalid("noncanonical boolean argument");
      frame.put(input, bits, type == "J");
      input += width(type);
    }
    std::map<uint32_t, size_t> positions;
    for (size_t i = 0; i < method.instructions.size(); ++i)
      if (!positions.emplace(method.instructions[i].pc, i).second)
        frame.invalid("duplicate instruction address");
    std::optional<uint64_t> pending;
    std::string pending_type;
    for (size_t index = 0; index < method.instructions.size();) {
      const auto &insn = method.instructions[index++];
      frame.pc = insn.pc;
      auto budget = admit();
      if (budget != ExecutionStop::Returned)
        halt(budget, method, frame.pc, "DEX execution budget exhausted");
      const auto &r = insn.registers;
      const std::string_view op = insn.opcode;
      auto count = [&](size_t n) {
        if (r.size() != n)
          frame.invalid("operand shape mismatch");
      };
      auto branch = [&](std::optional<uint32_t> target) {
        if (!target || !positions.contains(*target))
          frame.invalid("branch does not target an instruction");
        index = positions.at(*target);
      };
      if (op == "move-result" || op == "move-result-wide") {
        count(1);
        bool wide = op == "move-result-wide";
        if (!pending || wide != (pending_type == "J"))
          frame.invalid("move-result without compatible invocation");
        frame.put(r[0], *pending, wide);
        pending.reset();
        continue;
      }
      pending.reset();
      if (op == "nop")
        continue;
      if (op == "const" || op == "const/4" || op == "const/16" ||
          op == "const/high16" || op == "const-wide" || op == "const-wide/16" ||
          op == "const-wide/32" || op == "const-wide/high16") {
        count(1);
        auto literal = std::get_if<int64_t>(&insn.literal);
        if (!literal)
          frame.invalid("missing integer constant");
        frame.put(r[0], uint64_t(*literal), op.starts_with("const-wide"));
      } else if (op == "move" || op == "move/from16" || op == "move/16" ||
                 op == "move-wide" || op == "move-wide/from16" ||
                 op == "move-wide/16") {
        count(2);
        bool wide = op.starts_with("move-wide");
        frame.put(r[0], frame.get(r[1], wide), wide);
      } else if (op == "return" || op == "return-wide" || op == "return-void") {
        bool empty = op == "return-void", wide = op == "return-wide";
        count(empty ? 0 : 1);
        if (empty != (method.reference.returns == "V") ||
            wide != (method.reference.returns == "J"))
          frame.invalid("return type mismatch");
        auto value = empty ? 0 : frame.get(r[0], wide);
        if (!empty && value != narrow(value, method.reference.returns))
          frame.invalid("return value is not canonical for its declared type");
        if (method.reference.returns == "Z" && value > 1)
          frame.invalid("noncanonical boolean return");
        remaining_stack += bytes;
        return value;
      } else if (op == "goto" || op == "goto/16" || op == "goto/32") {
        count(0);
        branch(insn.target);
      } else if (op.starts_with("if-")) {
        bool zero = op.ends_with('z');
        count(zero ? 1 : 2);
        int32_t a = std::bit_cast<int32_t>(uint32_t(frame.get(r[0])));
        int32_t b =
            zero ? 0 : std::bit_cast<int32_t>(uint32_t(frame.get(r[1])));
        auto relation = op.substr(3, 2);
        bool take;
        if (relation == "eq")
          take = a == b;
        else if (relation == "ne")
          take = a != b;
        else if (relation == "lt")
          take = a < b;
        else if (relation == "ge")
          take = a >= b;
        else if (relation == "gt")
          take = a > b;
        else if (relation == "le")
          take = a <= b;
        else
          frame.invalid("unknown branch relation");
        if (take)
          branch(insn.target);
      } else if (op == "packed-switch" || op == "sparse-switch") {
        count(1);
        if (insn.keys.size() != insn.targets.size())
          frame.invalid("switch shape mismatch");
        int32_t key = std::bit_cast<int32_t>(uint32_t(frame.get(r[0])));
        for (size_t i = 0; i < insn.keys.size(); ++i)
          if (key == insn.keys[i]) {
            branch(insn.targets[i]);
            break;
          }
      } else if (op == "invoke-static" || op == "invoke-static/range") {
        auto reference = std::get_if<MethodRef>(&insn.reference);
        if (!reference)
          frame.invalid("missing method reference");
        auto callee = program.findStatic(*reference);
        if (!callee)
          halt(ExecutionStop::Unsupported, method, frame.pc,
               "unmodeled static method " + reference->identity());
        if (auto reason = program.unsupported(*callee))
          halt(ExecutionStop::Unsupported, method, frame.pc, *reason);
        if (!accessible(callee->reference.owner, callee->access,
                        method.reference.owner))
          halt(ExecutionStop::Unsupported, method, frame.pc,
               "inaccessible cross-class call");
        std::vector<uint64_t> values;
        size_t word = 0;
        for (const auto &type : reference->parameters) {
          bool wide = type == "J";
          if (word >= r.size() ||
              (wide && (word + 1 >= r.size() || r[word + 1] != r[word] + 1)))
            frame.invalid("invoke argument word mismatch");
          auto bits = frame.get(r[word], wide);
          if (bits != narrow(bits, type) || (type == "Z" && bits > 1))
            frame.invalid(
                "invoke argument is not canonical for its declared type");
          values.push_back(bits);
          word += wide ? 2 : 1;
        }
        if (word != r.size())
          frame.invalid("invoke argument count mismatch");
        auto value = run(*callee, values, depth + 1);
        pending_type = reference->returns;
        if (pending_type != "V")
          pending = value;
      } else if (op.starts_with("sget") || op.starts_with("sput")) {
        count(1);
        auto reference = std::get_if<FieldRef>(&insn.reference);
        if (!reference)
          frame.invalid("missing field reference");
        auto field = program.findStaticField(*reference);
        if (!field)
          halt(ExecutionStop::Unsupported, method, frame.pc,
               "unmodeled static field");
        if (!accessible(reference->owner, field->access,
                        method.reference.owner))
          halt(ExecutionStop::Unsupported, method, frame.pc,
               "inaccessible cross-class field");
        auto suffix = op.substr(4);
        std::string_view type = suffix.empty()         ? "I"
                                : suffix == "-wide"    ? "J"
                                : suffix == "-boolean" ? "Z"
                                : suffix == "-byte"    ? "B"
                                : suffix == "-char"    ? "C"
                                : suffix == "-short"   ? "S"
                                                       : "";
        if (type.empty())
          halt(ExecutionStop::Unsupported, method, frame.pc,
               "unsupported static field opcode");
        if (reference->type != type)
          frame.invalid("static field opcode/type mismatch");
        bool write = op.starts_with("sput");
        if (write && has(field->access, "final") &&
            (method.reference.owner != reference->owner ||
             method.reference.name != "<clinit>"))
          frame.invalid("final static field write outside its initializer");
        auto bits = write ? frame.get(r[0], type == "J") : 0;
        initialize(reference->owner, depth + 1);
        auto &value = state.classes.at(program.findClass(reference->owner))
                          .fields.at(field);
        if (write) {
          // The field instruction narrows at the store, unlike a method's
          // typed return/argument boundary. Boolean registers must be 0 or 1.
          if (type == "Z" && bits > 1)
            frame.invalid("noncanonical boolean field store");
          value = narrow(bits, type);
        } else
          frame.put(r[0], value, type == "J");
      } else if (op == "cmp-long") {
        count(3);
        auto a = std::bit_cast<int64_t>(frame.get(r[1], true));
        auto b = std::bit_cast<int64_t>(frame.get(r[2], true));
        frame.put(r[0], uint32_t(a < b ? -1 : a > b ? 1 : 0));
      } else if (op == "int-to-long" || op == "long-to-int" ||
                 op == "int-to-byte" || op == "int-to-char" ||
                 op == "int-to-short") {
        count(2);
        auto a = frame.get(r[1], op == "long-to-int");
        if (op == "int-to-long")
          a = uint64_t(int64_t(std::bit_cast<int32_t>(uint32_t(a))));
        else if (op == "int-to-byte")
          a = narrow(a, "B");
        else if (op == "int-to-char")
          a = narrow(a, "C");
        else if (op == "int-to-short")
          a = narrow(a, "S");
        frame.put(r[0], a, op == "int-to-long");
      } else if (op == "neg-int" || op == "not-int" || op == "neg-long" ||
                 op == "not-long") {
        count(2);
        bool wide = op.ends_with("long");
        auto a = frame.get(r[1], wide);
        frame.put(r[0], op.starts_with("neg") ? uint64_t(0) - a : ~a, wide);
      } else {
        size_t dash = op.find('-');
        auto type = dash == op.npos ? std::string_view() : op.substr(dash + 1);
        auto base_type = type.substr(0, type.find('/'));
        if (base_type != "int" && base_type != "long")
          halt(ExecutionStop::Unsupported, method, frame.pc,
               "unsupported opcode " + insn.opcode);
        bool wide = base_type == "long", inplace = type.ends_with("/2addr");
        bool literal = type.ends_with("/lit8") || type.ends_with("/lit16") ||
                       op == "rsub-int";
        auto operation = op.substr(0, dash);
        bool shift =
            operation == "shl" || operation == "shr" || operation == "ushr";
        count(inplace || literal ? 2 : 3);
        uint64_t left = frame.get(r[inplace ? 0 : 1], wide), right;
        if (literal) {
          auto value = std::get_if<int64_t>(&insn.literal);
          if (!value || wide)
            frame.invalid("integer literal mismatch");
          right = uint32_t(*value);
        } else
          right = frame.get(r[inplace ? 1 : 2], wide && !shift);
        frame.put(
            r[0],
            wide ? binary(operation, left, right, frame)
                 : binary(operation, uint32_t(left), uint32_t(right), frame),
            wide);
      }
    }
    frame.invalid("execution fell off method body");
  }
};

Program::Program(std::vector<Class> declarations) {
  for (auto &item : declarations) {
    if (item.name.starts_with("Ljava/") || item.name.starts_with("Ljavax/") ||
        item.name.starts_with("Landroid/") || item.name.starts_with("Ldalvik/"))
      throw Error("DEX cannot replace platform classes");
    auto name = item.name;
    if (!classes.emplace(std::move(name), std::move(item)).second)
      throw Error("duplicate supplied DEX class");
  }
}
bool Program::containsClass(std::string_view name) const {
  return findClass(name) != nullptr;
}
const Class *Program::findClass(std::string_view name) const {
  auto it = classes.find(std::string(name));
  return it == classes.end() ? nullptr : &it->second;
}
const Field *Program::findStaticField(const FieldRef &reference) const {
  if (auto klass = findClass(reference.owner))
    for (const auto &field : klass->fields)
      if (field.reference == reference && has(field.access, "static"))
        return &field;
  return nullptr;
}
const Method *Program::findStatic(const MethodRef &reference) const {
  auto owner = classes.find(reference.owner);
  if (owner == classes.end())
    return nullptr;
  for (const auto &method : owner->second.methods)
    if (method.reference == reference && has(method.access, "static"))
      return &method;
  return nullptr;
}
std::optional<std::string>
Program::unsupportedClass(std::string_view name) const {
  auto owner = classes.find(std::string(name));
  if (owner == classes.end())
    return "class is not supplied by DEX";
  const auto &klass = owner->second;
  if (klass.superclass != "Ljava/lang/Object;" || !klass.interfaces.empty() ||
      has(klass.access, "interface"))
    return "DEX class requires unmodeled hierarchy";
  for (const auto &field : klass.fields)
    if (!has(field.access, "static") || !integral(field.reference.type) ||
        has(field.access, "volatile"))
      return "DEX instance, object, floating or volatile field is unsupported";
  return std::nullopt;
}
std::optional<std::string> Program::unsupported(const Method &method) const {
  auto owner = classes.find(method.reference.owner);
  if (owner == classes.end() || findStatic(method.reference) != &method)
    return "method is not an admitted static DEX body";
  if (auto reason = unsupportedClass(method.reference.owner))
    return reason;
  if (has(method.access, "native") || has(method.access, "abstract") ||
      has(method.access, "synchronized") ||
      has(method.access, "declared-synchronized") ||
      method.instructions.empty() || !method.tries.empty())
    return "DEX native, abstract, synchronized or exception-handler body is "
           "unsupported";
  if (method.reference.returns != "V" && !integral(method.reference.returns))
    return "DEX object or floating return is unsupported";
  for (const auto &type : method.reference.parameters)
    if (!integral(type))
      return "DEX object or floating argument is unsupported";
  return std::nullopt;
}
ExecutionResult Program::execute(const Method &method,
                                 std::span<const uint64_t> args,
                                 const AdmitInstruction &admit,
                                 uint64_t stack_bytes) const {
  Runtime runtime(*this);
  return runtime.execute(method, args, admit, stack_bytes);
}
Runtime::Runtime(const Program &program)
    : state(std::make_unique<State>(program)) {}
Runtime::~Runtime() = default;
ExecutionResult Runtime::initialize(std::string_view descriptor,
                                    const AdmitInstruction &admit,
                                    uint64_t stack_bytes) {
  return state->invoke([&] {
    State::Engine(*state, admit, stack_bytes).initialize(descriptor, 0);
    return uint64_t(0);
  });
}
ExecutionResult Runtime::execute(const Method &method,
                                 std::span<const uint64_t> args,
                                 const AdmitInstruction &admit,
                                 uint64_t stack_bytes) {
  return state->invoke([&] {
    return State::Engine(*state, admit, stack_bytes).run(method, args, 0);
  });
}
} // namespace neverd::mobile::dalvik
