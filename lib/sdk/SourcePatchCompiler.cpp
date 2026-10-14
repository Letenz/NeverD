//===- SourcePatchCompiler.cpp - Explicit source compiler adapters --------===//

#include "SourcePatchCompiler.h"

#include "neverd/ArchSupport.h"
#include "neverd/backend/codegen/SourcePatch.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <optional>

namespace neverd::sdk {
namespace {

llvm::Error compilerError(const llvm::Twine &Message) {
  return llvm::make_error<llvm::StringError>("source compiler: " + Message,
                                             llvm::inconvertibleErrorCode());
}

std::string readText(const std::filesystem::path &Path) {
  auto Buffer = llvm::MemoryBuffer::getFile(Path.string());
  return Buffer ? (*Buffer)->getBuffer().str() : std::string();
}

llvm::Error execute(const std::vector<std::string> &Arguments,
                    const std::filesystem::path &Out,
                    const std::filesystem::path &Err, unsigned Timeout) {
  // LLVM's process redirect may reopen an existing file without truncating it
  // on some hosts. Every probe must read only this invocation's output.
  for (const auto &Path : {Out, Err}) {
    std::error_code EC;
    llvm::raw_fd_ostream Clear(Path.string(), EC);
    if (EC)
      return compilerError("cannot reset diagnostic file: " + EC.message());
    Clear.close();
    if (Clear.has_error())
      return compilerError("cannot finish resetting diagnostic file");
  }
  std::vector<llvm::StringRef> Args(Arguments.begin(), Arguments.end());
  const std::string OutPath = Out.string(), ErrPath = Err.string();
  const std::array<std::optional<llvm::StringRef>, 3> Redirects = {
      llvm::StringRef(), OutPath, ErrPath};
  std::string Detail;
  bool LaunchFailed = false;
  int Code =
      llvm::sys::ExecuteAndWait(Args.front(), Args, std::nullopt, Redirects,
                                Timeout, 0, &Detail, &LaunchFailed);
  if (Code != 0 || LaunchFailed)
    return compilerError("process failed (" + llvm::Twine(Code) +
                         "): " + Detail + "\n" + readText(Err));
  return llvm::Error::success();
}

llvm::Error appendUserArguments(std::vector<std::string> &Arguments,
                                const neverd_source_patch_options_v1 &Options) {
  // Compiler control, target identity and output modes belong to the adapter.
  // Accept ordinary source configuration without permitting @response files
  // or driver escape options to silently change that contract.
  // Drivers expand response files before interpreting option values, so also
  // reject response tokens consumed by a separate -I, -include, -D, etc.
  for (size_t I = 0; I < Options.compiler_arg_count; ++I)
    if (llvm::StringRef(Options.compiler_args[I]).starts_with("@"))
      return compilerError("response files are not supported in compiler "
                           "arguments");
  for (size_t I = 0; I < Options.compiler_arg_count; ++I) {
    llvm::StringRef Arg(Options.compiler_args[I]);
    const bool Separate = Arg == "-I" || Arg == "-isystem" ||
                          Arg == "-iquote" || Arg == "-include" ||
                          Arg == "-D" || Arg == "-U" || Arg == "-isysroot";
    if (Separate) {
      if (++I == Options.compiler_arg_count)
        return compilerError("missing value after " + Arg);
      Arguments.push_back(Arg.str());
      Arguments.emplace_back(Options.compiler_args[I]);
      continue;
    }
    if (Arg.starts_with("-fpatchable-function-entry=")) {
      if (Options.artifact != NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT)
        return compilerError(
            "machine entry padding requires the native object profile");
      auto Parts = Arg.drop_front(27).split(',');
      unsigned Count = 0, Before = 0;
      if (Parts.first.getAsInteger(10, Count) || Count > 1024 ||
          (!Parts.second.empty() && Parts.second.getAsInteger(10, Before)) ||
          Before > Count)
        return compilerError("invalid machine entry padding");
      Arguments.push_back(Arg.str());
      continue;
    }
    if (Arg.starts_with("-I") || Arg.starts_with("-D") ||
        Arg.starts_with("-U") || Arg.starts_with("-std=") ||
        Arg.starts_with("--sysroot=") || Arg == "-O0" || Arg == "-O1" ||
        Arg == "-O2" || Arg == "-O3" || Arg == "-Os" || Arg == "-Oz" ||
        Arg == "-g" || Arg == "-g0" || Arg == "-fno-builtin" ||
        Arg == "-ffreestanding" || Arg == "-fno-omit-frame-pointer" ||
        Arg == "-fstack-protector-strong" || Arg == "-fexceptions" ||
        Arg == "-fno-exceptions" || Arg == "-frtti" || Arg == "-fno-rtti" ||
        Arg == "-Wall" || Arg == "-Wextra" || Arg == "-Werror") {
      Arguments.push_back(Arg.str());
      continue;
    }
    return compilerError("unsupported source option: " + Arg);
  }
  return llvm::Error::success();
}

std::string digest(llvm::StringRef Bytes) {
  return llvm::toHex(
      llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(
          reinterpret_cast<const uint8_t *>(Bytes.data()), Bytes.size())),
      true);
}

llvm::Expected<std::filesystem::path>
translateCpp(SourcePatchCompilation &Result, llvm::StringRef Program,
             const std::filesystem::path &Input,
             const std::filesystem::path &Directory, llvm::StringRef Target,
             const neverd_source_patch_options_v1 &Options,
             std::vector<std::string> &CodegenArgs, unsigned Timeout) {
  const auto Output = Directory / "translated.nc";
  const auto Out = Directory / "translate.stdout";
  const auto Err = Directory / "translate.stderr";
  auto &Args = Result.TranslationArguments;
  Args = {Program.str(),  "translate", "--from",        "cpp",
          Input.string(), "--profile", "cpp-core-v2",   "--target",
          Target.str(),   "-o",        Output.string(), "--",
          "-std=c++17"};
  std::vector<std::string> User;
  if (auto Error = appendUserArguments(User, Options))
    return std::move(Error);
  for (size_t I = 0; I < User.size(); ++I) {
    const llvm::StringRef Arg(User[I]);
    if (Arg == "-D" || Arg == "-U") {
      Args.push_back(User[I]);
      Args.push_back(User[++I]);
    } else if (Arg.starts_with("-D") || Arg.starts_with("-U")) {
      Args.push_back(User[I]);
    } else if (Arg == "-std=c++17") {
      continue;
    } else if (Arg == "-fexceptions" || Arg == "-frtti") {
      return compilerError("NeverC cpp-core-v2 does not admit C++ exceptions "
                           "or RTTI; select the Clang C++ profile");
    } else if (Arg.starts_with("-std=") || Arg.starts_with("-I") ||
               Arg == "-isystem" || Arg == "-iquote" || Arg == "-include" ||
               Arg == "-isysroot" || Arg.starts_with("--sysroot=")) {
      return compilerError("NeverC cpp-core-v2 accepts C++17, macros and "
                           "its embedded headers; unsupported option: " +
                           Arg);
    } else {
      CodegenArgs.push_back(User[I]);
    }
  }
  if (auto Error = execute(Args, Out, Err, Timeout))
    return std::move(Error);
  Result.Diagnostics += readText(Err);
  Result.TranslationManifest = readText(Output.string() + ".manifest.json");
  Result.TranslationMap = readText(Output.string() + ".map.json");
  auto Parsed = llvm::json::parse(Result.TranslationManifest);
  if (!Parsed)
    return compilerError("invalid NeverC translation manifest: " +
                         llvm::toString(Parsed.takeError()));
  const auto *Manifest = Parsed->getAsObject();
  const auto *ABI = Manifest ? Manifest->getObject("target") : nullptr;
  const auto *Files =
      Manifest ? Manifest->getArray("generated_files") : nullptr;
  const auto *Dependencies =
      Manifest ? Manifest->getArray("dependencies") : nullptr;
  const auto *Exports = Manifest ? Manifest->getArray("exports") : nullptr;
  if (!Manifest ||
      Manifest->getString("schema") != "neverc.translate.manifest" ||
      Manifest->getInteger("version") != 1 ||
      Manifest->getInteger("protocol") != 1 ||
      Manifest->getString("profile") != "cpp-core-v2" ||
      Manifest->getInteger("profile_version") != 2 || !ABI ||
      ABI->getString("triple") != Target ||
      ABI->getInteger("pointer_bits") != 64 ||
      ABI->getInteger("int_bits") != 32 ||
      ABI->getBoolean("little_endian") != true || !Files || !Dependencies ||
      !Exports)
    return compilerError("NeverC translation manifest does not prove the "
                         "requested C++ profile/target ABI");
  auto Matches = [](const llvm::json::Array &Rows, llvm::StringRef Name,
                    llvm::StringRef Hash) {
    return llvm::count_if(Rows, [&](const auto &Value) {
             const auto *Row = Value.getAsObject();
             return Row && Row->getString("path") == Name &&
                    Row->getString("sha256") == Hash;
           }) == 1;
  };
  const std::string Generated = readText(Output);
  if (Generated.empty() || Result.TranslationMap.empty() ||
      !Matches(*Files, Output.filename().string(), digest(Generated)) ||
      !Matches(*Files, Output.filename().string() + ".map.json",
               digest(Result.TranslationMap)) ||
      !Matches(*Dependencies, Input.filename().string(),
               digest(readText(Input))))
    return compilerError("NeverC translation artifacts/source do not match "
                         "their manifest hashes");
  for (size_t I = 0; I < Options.mapping_count; ++I) {
    const llvm::StringRef Symbol =
        Options.mappings[I].symbol ? Options.mappings[I].symbol : "";
    if (!Symbol.empty() && llvm::count_if(*Exports, [&](const auto &Value) {
                             const auto *Export = Value.getAsObject();
                             return Export &&
                                    Export->getString("name") == Symbol &&
                                    Export->getBoolean("c_export") == true;
                           }) != 1)
      return compilerError("NeverC C++ patch entries require an exact "
                           "extern \"C\" export: " +
                           Symbol);
  }
  return Output;
}

} // namespace

llvm::Expected<SourcePatchCompilation>
compileSourcePatch(llvm::StringRef Source,
                   const neverd_source_patch_options_v1 &Options,
                   const BinaryImage &Image) {
  const Arch Architecture = Image.Arch;
  const BinaryFormat Format = Image.Format;
  const bool DynCode = Options.mode == NEVERD_SOURCE_PATCH_DYNCODE;
  const bool NeverC = Options.compiler == NEVERD_SOURCE_PATCH_NEVERC;
  const bool NeverCCpp = NeverC && Options.language == NEVERD_SOURCE_PATCH_CXX;
  const bool Object = Options.artifact == NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT;
  const bool GCC = Options.compiler == NEVERD_SOURCE_PATCH_GCC;
  if (GCC && !Object)
    return compilerError("GCC requires the native object artifact profile");
  if (NeverC && Architecture != Arch::X64 && Architecture != Arch::AArch64)
    return compilerError("NeverC code generation requires x64 or ARM64");
  if (DynCode && (!NeverC || !Object || Options.mapping_count != 1 ||
                  !Options.mappings || !Options.mappings[0].symbol ||
                  !*Options.mappings[0].symbol || Options.binding_count))
    return compilerError("hosted dyncode requires NeverC, a native object, "
                         "one explicit entry and no original bindings");
  if (!Object)
    if (auto Error = validateSourcePatchTarget(Architecture, Format))
      return std::move(Error);
  auto Target = sourcePatchTargetTriple(Image);
  if (!Target)
    return Target.takeError();

  const std::string Name =
      Options.compiler_path && *Options.compiler_path  ? Options.compiler_path
      : Options.compiler == NEVERD_SOURCE_PATCH_NEVERC ? "neverc"
      : GCC ? (Options.language == NEVERD_SOURCE_PATCH_CXX ? "g++" : "gcc")
      : Options.language == NEVERD_SOURCE_PATCH_CXX ? "clang++"
                                                    : "clang";
  auto Program = llvm::sys::findProgramByName(Name);
  if (!Program || !llvm::sys::fs::can_execute(*Program))
    return compilerError("selected executable not found: " + Name);
  llvm::SmallString<256> Directory;
  if (auto EC = llvm::sys::fs::createUniqueDirectory("neverd-source-compiler",
                                                     Directory))
    return compilerError("cannot create temporary directory: " + EC.message());
  const std::filesystem::path TempDir(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] {
    std::error_code Ignored;
    std::filesystem::remove_all(TempDir, Ignored);
  });
  auto Input = TempDir / (NeverCCpp ? "source.cpp" : "source");
  const auto Output = TempDir / (Object ? "output.o" : "output.ll");
  const auto Stdout = TempDir / "stdout";
  const auto Stderr = TempDir / "stderr";
  const unsigned Timeout =
      Options.timeout_seconds ? Options.timeout_seconds : 30;
  SourcePatchCompilation Result;
  Result.CompilerPath = *Program;
  if (auto Error = execute({*Program, "--version"}, Stdout, Stderr, Timeout))
    return std::move(Error);
  Result.CompilerVersion = readText(Stdout);
  const std::string Lower = llvm::StringRef(Result.CompilerVersion).lower();
  if ((Options.compiler == NEVERD_SOURCE_PATCH_NEVERC &&
       Lower.find("neverc") == std::string::npos) ||
      (Options.compiler == NEVERD_SOURCE_PATCH_CLANG &&
       (Lower.find("clang") == std::string::npos ||
        Lower.find("neverc") != std::string::npos)) ||
      (GCC && (Lower.find("clang") != std::string::npos ||
               Lower.find("neverc") != std::string::npos ||
               (Lower.find("gcc") == std::string::npos &&
                Lower.find("g++") == std::string::npos))))
    return compilerError(
        "executable identity does not match selected compiler");
  std::error_code EC;
  llvm::raw_fd_ostream OS(Input.string(), EC);
  if (EC)
    return compilerError("cannot write source: " + EC.message());
  OS << Source;
  OS.close();
  if (OS.has_error())
    return compilerError("cannot finish writing source");

  auto &Args = Result.Arguments;
  Args = {*Program, "-O2"};
  if (NeverC)
    Args.push_back("--no-default-config");
  Args.push_back("-funwind-tables");
  if (!GCC)
    Args.push_back("-fno-discard-value-names");
  if (GCC) {
    if (auto Error =
            execute({*Program, "-dumpmachine"}, Stdout, Stderr, Timeout))
      return std::move(Error);
    const llvm::Triple DriverTarget(
        llvm::Triple::normalize(llvm::StringRef(readText(Stdout)).trim()));
    if ((DriverTarget.getArch() != llvm::Triple(*Target).getArch() &&
         !((DriverTarget.isARM() || DriverTarget.isThumb()) &&
           Architecture == Arch::ARM)) ||
        !DriverTarget.isOSLinux() ||
        (Architecture == Arch::ARM &&
         DriverTarget.getEnvironment() !=
             llvm::Triple(*Target).getEnvironment()))
      return compilerError("selected GCC must match the loaded ELF "
                           "architecture and ABI; use a matching cross driver");
    if (Architecture == Arch::ARM)
      Args.push_back(llvm::Triple(*Target).isThumb() ? "-mthumb" : "-marm");
  }
#ifdef __APPLE__
  if (Format == BinaryFormat::MachO) {
    std::string SDK;
    if (const char *Value = std::getenv("SDKROOT"))
      SDK = Value;
    if (SDK.empty()) {
      if (auto Xcrun = llvm::sys::findProgramByName("xcrun")) {
        if (auto Error =
                execute({*Xcrun, "--show-sdk-path"}, Stdout, Stderr, Timeout))
          return std::move(Error);
        SDK = llvm::StringRef(readText(Stdout)).trim().str();
      }
    }
    if (!SDK.empty()) {
      Args.push_back("-isysroot");
      Args.push_back(SDK);
    }
  }
#endif
  if (!NeverCCpp && Options.source_name && *Options.source_name) {
    auto Parent = std::filesystem::absolute(Options.source_name).parent_path();
    Args.push_back("-iquote");
    Args.push_back(Parent.string());
  }
  if (NeverCCpp) {
    auto Translated = translateCpp(Result, *Program, Input, TempDir, *Target,
                                   Options, Args, Timeout);
    if (!Translated)
      return Translated.takeError();
    Input = *Translated;
    Args.push_back("-std=c23");
  } else if (auto Error = appendUserArguments(Args, Options)) {
    return std::move(Error);
  }
  if (!GCC) {
    // Darwin's driver accepts an ARM CPU spelling and selects Thumb with
    // -mthumb. The resulting IR/backend triple still carries the Thumb state.
    if (Format == BinaryFormat::MachO && Architecture == Arch::ARM &&
        llvm::StringRef(*Target).starts_with("thumb")) {
      Args.push_back("--target=arm" + Target->substr(5));
      Args.push_back("-mthumb");
    } else {
      Args.push_back("--target=" + *Target);
    }
  }
  Args.push_back("-fno-lto");
  if (Options.compiler == NEVERD_SOURCE_PATCH_NEVERC &&
      Format == BinaryFormat::COFF) {
    // A patch resolves references against the original image; it never links
    // another CRT. Ask the producer to omit automatic DEFAULTLIB directives
    // instead of stripping linker contracts from a supplied COFF object.
    Args.push_back("-fno-autolink");
  }
  if (DynCode) {
    Args.insert(Args.end(),
                {"-fdyncode", "-fdyncode-hosted",
                 "-fdyncode-entry=" + std::string(Options.mappings[0].symbol),
                 "-fdyncode-keep-obj=" + Output.string(),
                 "-fdyncode-report=" + (TempDir / "dyncode.json").string()});
  } else if (Object) {
    if (Format != BinaryFormat::COFF)
      Args.push_back("-fPIC");
    if (Format == BinaryFormat::COFF)
      Args.push_back("-ffunction-sections");
    if (Format == BinaryFormat::MachO && Architecture == Arch::ARM &&
        llvm::Triple(*Target).isWatchOS()) {
      // Watch's object producer omits unwind rows for frameless leaf functions.
      // Keep compiler-owned frames instead of inventing native unwind recipes.
      Args.push_back("-fno-omit-frame-pointer");
      Args.push_back("-mno-omit-leaf-frame-pointer");
    }
    Args.push_back("-c");
  } else {
    Args.push_back("-emit-llvm");
    Args.push_back("-S");
  }
  Args.insert(
      Args.end(),
      {"-x",
       !NeverCCpp && Options.language == NEVERD_SOURCE_PATCH_CXX ? "c++" : "c",
       "-o", (DynCode ? TempDir / "payload.bin" : Output).string(),
       Input.string()});
  if (auto Error = execute(Args, Stdout, Stderr, Timeout))
    return std::move(Error);
  Result.Diagnostics += readText(Stderr);
  auto Product = llvm::MemoryBuffer::getFile(Output.string());
  if (!Product)
    return compilerError("compiler did not produce the requested artifact");
  Result.Bytes = (*Product)->getBuffer().str();
  if (DynCode) {
    DynCodePatchEvidence Evidence;
    Evidence.Payload = readText(TempDir / "payload.bin");
    Evidence.Audit = readText(TempDir / "dyncode.json");
    if (Evidence.Payload.empty() || Evidence.Audit.empty())
      return compilerError(
          "NeverC did not produce hosted DynCode payload/audit");
    Result.DynCode = std::move(Evidence);
  }
  return Result;
}

} // namespace neverd::sdk
