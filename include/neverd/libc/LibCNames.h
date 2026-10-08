//===- LibCNames.h - libc/POSIX registry and traits ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIBC_LIBCNAMES_H
#define NEVERD_LIBC_LIBCNAMES_H

// Shared libc/POSIX symbol tables for C emission, MIR stub resolution, etc.
// Per-header name lists live beside this file; registry lookup and call-trait
// APIs are implemented in LibCNames.cpp and LibCCallTraits.cpp respectively.

#include "neverd/Common.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace neverd {
struct BinaryImage;
}

namespace neverd::libc {

/// Integer/pointer and floating-point argument counts of a non-variadic libc
/// function with a fixed, well-known signature.
struct LibCArity {
  int IntArgs = 0;        ///< Integer/pointer arguments (register + stack).
  int FpArgs = 0;         ///< Floating-point (float/double) arguments.
  bool FpIsFloat = false; ///< FP args/return are 32-bit float (the `f` math
                          ///< variants), else 64-bit double.
  bool FpFirst = false;   ///< Mixed int+FP libm whose real signature is the FP
                        ///< args first then the int/pointer args (ldexp/frexp/
                        ///< modf/scalbn): the FP arg(s) precede the int/ptr,
                        ///< and the return is FP.  Lets the emitter reorder the
                        ///< (int-then-FP modelled) recovered arguments back to
                        ///< the real positional order.
  bool FpRet = false; ///< Return value is floating-point (width per FpIsFloat)
                      ///< even when there are NO FP arguments — the
                      ///< integer/pointer-arg, FP-return forms (atof/strtod/
                      ///< strtof/difftime/nan).  Without it the emitter
                      ///< declares an i64 return and reads the result from the
                      ///< integer return register instead of d0/xmm0/s0, so a
                      ///< patched `atof("3.14")` yields 0.
  bool FpRetLongDouble =
      false; ///< Return is `long double`.  Only ABI-equivalent
             ///< to `double` where long double is 64-bit (Apple
             ///< AArch64); the emitter models it as a double return
             ///< only there and leaves other targets (x87 80-bit /
             ///< binary128) to the conservative fallback.
  bool FpRetComplex =
      false; ///< Return is `_Complex float`/`_Complex double` — a
             ///< homogeneous FP aggregate of two elements returned in
             ///< two FP registers (d0/d1, s0/s1, xmm0/xmm1) on the
             ///< 64-bit ABIs (csqrt/cexp/clog/cpow/...).  The FP-arg
             ///< count still counts each complex element separately
             ///< (one complex arg == 2 FP args).  The emitter declares
             ///< the callee with a `{fp,fp}` struct return so the
             ///< backend reads both result registers; without it the
             ///< conservative `(...)->i64` fallback reads only the
             ///< integer return register, so the real part is garbage
             ///< and the imaginary part is 0.
};

/// A single (name, arity) row of a per-header arity table (kStdioArity,
/// kStringArity, ...).  These tables live beside the function-name lists in the
/// libc_*.h headers; libcArity assembles them into one lookup map.
struct LibCArityEntry {
  std::string_view Name;
  LibCArity Arity;
};

/// The C declaration of a routine no standard header declares, such as the one
/// the start files enter a program through (LibCStartup.h): its return type
/// and each parameter's C type.  Its arity is derived from it.
struct LibCPrototype {
  /// The name without leading underscores, as the arity tables key it.
  std::string_view Name;
  std::string_view Return;
  std::array<std::string_view, 8> Params{};
  uint8_t ParamCount = 0;
};

/// Whether a parameter of C type \p Type takes a pointer: an object or a
/// function pointer.
constexpr bool isPointerParameter(std::string_view Type) {
  return !Type.empty() &&
         (Type.back() == '*' || Type.find("(*)") != std::string_view::npos);
}

/// The prototype of the routine a symbol \p Name links to, leading
/// underscores ignored, or null.
const LibCPrototype *libcPrototypeForSymbol(std::string_view Name);

/// The fixed argument arity of a known NON-variadic libc function (e.g. fputs
/// -> {2,0}, sqrt -> {0,1}), used to bound the heuristic argument recovery for
/// an external call whose true signature is otherwise unknown.  Returns nullopt
/// for an unknown function, a variadic function (use varArgFixedCount instead),
/// or a function whose arity is intentionally not modelled (mixed int/FP forms
/// like ldexp/frexp).  Name must have its leading underscores already stripped.
std::optional<LibCArity> libcArity(std::string_view Name);

/// A Windows kernel routine's documented parameters
/// (WindowsKernelRoutines.inc): the bytes the Win64 ABI passes for each, in
/// order.
struct WindowsKernelPrototype {
  std::string_view Name;
  std::array<uint8_t, 16> ArgWidths{};
  uint8_t ArgCount = 0;
};

/// The WDK prototype of \p Name, or null.  Leading underscores are ignored.
const WindowsKernelPrototype *windowsKernelPrototype(std::string_view Name);

/// True for an indirect-call dispatcher such as MSVC Control Flow Guard's
/// `_guard_dispatch_icall`, which jumps to RAX with the caller's arguments.
bool isIndirectCallDispatchThunk(std::string_view Name);

/// The fixed arity for a symbol name as it appears in an object or executable.
/// This preserves platform-decorated spellings whose leading underscores are
/// semantically significant before falling back to the canonical registry.
std::optional<LibCArity> libcArityForSymbol(std::string_view Name);

/// True if Name is a known libc/POSIX library function.
bool isKnownFunction(std::string_view Name);

/// Returns e.g. "stdlib.h", or nullptr if not a known libc function.
const char *headerFor(std::string_view Name);

/// If Name is a known variadic C function (printf, fprintf, etc.),
/// returns the number of fixed parameters before the '...'. Returns 0
/// if the function is not variadic.
unsigned varArgFixedCount(std::string_view Name);

/// ABI category of one fixed parameter in a known variadic function.  Pointer
/// parameters must be symbolized before code generation; integer parameters
/// must remain scalar values even when a small constant happens to overlap a
/// low-address data segment in a relocatable image.  Unknown preserves the
/// recovered call-site type (used for user functions and selector arguments
/// whose source-level type is not encoded in the symbol name).
enum class VarArgFixedParamKind {
  Unknown,
  Integer,
  Pointer,
};

/// Return the ABI category of fixed parameter \p Index for a known variadic
/// libc/POSIX function.  Names may retain platform-leading underscores.
VarArgFixedParamKind varArgFixedParamKind(std::string_view Name,
                                          unsigned Index);

/// True if Name is a known va_list-consuming function: the v-prefixed printf /
/// scanf family (vprintf, vfprintf, vsnprintf, vscanf, ...) and their fortified
/// __v*_chk variants, plus vsyslog/verr/vwarn.  These take a trailing `va_list`
/// argument rather than `...`; a function that forwards its own varargs into
/// one of them (the classic `void log(const char*fmt,...){ ...;
/// vfprintf(f,fmt,ap); }` wrapper) is therefore itself variadic.  Name must
/// have leading underscores already stripped.
bool isVaListConsumer(std::string_view Name);

/// True if parameter \p Index of the standard C or POSIX function \p Name is
/// a narrow, NUL-terminated `char` string.  Leading underscores are ignored.
bool isCStringParameter(std::string_view Name, unsigned Index);

/// What a compiler stack-probe helper does (StackProbeRoutines.inc).
enum class StackProbeEffect : uint8_t {
  /// Touches the frame's pages and changes nothing the program observes.
  Probe,
  /// Also moves the stack pointer by the size it receives.
  Allocate,
};

/// The effect of the stack-probe helper whose entry is \p Target in \p Img,
/// or nullopt when \p Target is not one for the image's format and
/// architecture.  The helper is known by a name the binary or its debug
/// information states, never by one an analysis guessed.
std::optional<StackProbeEffect> stackProbeEffect(const BinaryImage &Img,
                                                 va_t Target);

/// True if Name is a libc/POSIX function that never returns to its caller
/// (abort / exit / _exit / _Exit / quick_exit, longjmp / siglongjmp,
/// pthread_exit / thrd_exit, the err / errx family, and the internal assert /
/// stack-check / C++ unwind failure handlers).  A direct call to one is a
/// control-flow terminator: at -O2 the compiler emits nothing after such a
/// call, so the bytes that follow belong to the NEXT function — the CFG builder
/// must stop exploring rather than fall through and swallow them (a leaf `bl
/// _longjmp` would otherwise absorb the whole function laid out after it).
/// Leading platform underscores (the Mach-O `_` prefix) are stripped
/// internally.
bool isNoReturnFunction(std::string_view Name);

/// True if \p Target resolves through \p Img to a known no-return import
/// veneer or statically linked function symbol.  This is the image-aware
/// counterpart of isNoReturnFunction() shared by discovery and CFG recovery.
bool isNoReturnTarget(const BinaryImage &Img, va_t Target);

/// Exact no-return name lookup for one operation on an unchanged image.
/// The owner must outlive its readers; a different image uses the live lookup.
class NoReturnTargetIndex {
public:
  /// Names a function address that a restricted (`--func`) load has not
  /// ingested, such as a PDB public loaded on demand.
  using NameResolver = std::function<std::optional<std::string>(va_t)>;

  explicit NoReturnTargetIndex(const BinaryImage &Img,
                               NameResolver ResolveName = {});
  bool contains(const BinaryImage &Img, va_t Target) const;

private:
  const BinaryImage *Image;
  std::set<va_t> Targets;
  /// A restricted load skips the whole-image symbol walk.  Its targets are
  /// answered one at a time, the same way, and remembered: CFG workers
  /// share one index.
  bool SymbolsIndexed = true;
  NameResolver ResolveName;
  mutable std::mutex LazyMutex;
  mutable std::map<va_t, bool> LazyAnswers;
};

/// isNoReturnTarget() answered through \p Index when one is available, so
/// every layer that asks about one image gets the same answer.
bool isNoReturnTarget(const BinaryImage &Img, va_t Target,
                      const NoReturnTargetIndex *Index);

/// True if Name requires returns-twice register semantics
/// (setjmp / _setjmp / sigsetjmp / vfork). For setjmp, control re-enters when a
/// matching longjmp restores the saved context.  The emitter marks such a
/// callee `returns_twice` so a value live across the call is reloaded after it
/// instead of being stranded in a caller-saved register longjmp does not
/// restore (which would make the longjmp-return path read garbage).  Leading
/// platform underscores are stripped internally.
bool isReturnsTwiceFunction(std::string_view Name);

/// True if \p Name names a memory-copy libc routine (memcpy/memmove and their
/// fortified forms).  Leading platform underscores are stripped internally.
bool isMemCopyName(std::string_view Name);

/// True if \p Name names a memory-set libc routine (memset and its fortified
/// form).  Leading platform underscores are stripped internally.
bool isMemSetName(std::string_view Name);

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCNAMES_H
