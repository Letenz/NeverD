//===- neverd-bytecode.cpp - External-profile source recovery -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/support/AtomicOutput.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

namespace {
llvm::cl::opt<std::string> Input(llvm::cl::Positional, llvm::cl::Required,
                                 llvm::cl::desc("<raw bytecode>"));
llvm::cl::opt<std::string>
    ProfilePath("profile", llvm::cl::Required,
                llvm::cl::desc("External decoder profile JSON"));
llvm::cl::opt<std::string>
    FunctionsPath("functions", llvm::cl::Required,
                  llvm::cl::desc("Function ranges JSON"));
llvm::cl::opt<std::string>
    BindingsPath("bindings", llvm::cl::init(""),
                 llvm::cl::desc("External state-call declarations JSON"));
llvm::cl::opt<std::string> Output("o", llvm::cl::init("-"),
                                  llvm::cl::desc("C output path"));
llvm::cl::opt<uint64_t>
    Base("base", llvm::cl::init(0),
         llvm::cl::desc("Logical address of the first byte"));
llvm::cl::opt<bool> LLVMRoute("llvm",
                              llvm::cl::desc("Use the LLVM-to-C route"));
llvm::cl::opt<bool> Optimize(
    "optimize",
    llvm::cl::desc("Optimize LLVM IR before C emission (requires --llvm)"));
llvm::cl::opt<bool> UnalignedPointers(
    "unaligned-pointers",
    llvm::cl::desc("Use Clang/GCC unaligned aliasing scalar pointers in C"));
llvm::cl::opt<bool>
    Check("check", llvm::cl::desc("Check CFG decoding without emitting C"));

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}

llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> read(llvm::StringRef Path) {
  auto Buffer = llvm::MemoryBuffer::getFile(Path);
  if (!Buffer)
    return llvm::errorCodeToError(Buffer.getError());
  if ((*Buffer)->getBufferSize() > 64 * 1024 * 1024)
    return invalid("input exceeds the 64 MiB limit");
  return std::move(*Buffer);
}

llvm::Error write(llvm::StringRef Text) {
  if (Output == "-") {
    llvm::outs() << Text;
    return llvm::Error::success();
  }
  auto File = llvm::sys::fs::TempFile::create(Output + ".%%%%%%.tmp");
  if (!File)
    return File.takeError();
  llvm::raw_fd_ostream Stream(File->FD, false);
  Stream << Text;
  Stream.flush();
  if (Stream.has_error()) {
    auto Error = llvm::errorCodeToError(Stream.error());
    Stream.clear_error();
    return llvm::joinErrors(
        std::move(Error),
        neverd::support::atomic_output::discardTemporaryOutput(*File));
  }
  return neverd::support::atomic_output::closeAndCommitTemporaryOutput(*File,
                                                                       Output);
}

llvm::Error run() {
  if (Optimize && (!LLVMRoute || Check))
    return invalid("--optimize requires --llvm source emission");
  llvm::json::Object Request{
      {"schemaVersion", 1},
      {"base", Base.getValue()},
      {"output", Check ? "check" : (LLVMRoute ? "llvmc" : "highc")},
      {"optimize", Optimize.getValue()},
      {"unaligned_pointers", UnalignedPointers.getValue()}};
  for (const auto &Field : {std::pair{"profile", ProfilePath.getValue()},
                            std::pair{"functions", FunctionsPath.getValue()},
                            std::pair{"bindings", BindingsPath.getValue()}}) {
    if (Field.first == llvm::StringRef("bindings") && Field.second.empty())
      continue;
    auto Buffer = read(Field.second);
    if (!Buffer)
      return Buffer.takeError();
    auto JSON = llvm::json::parse((*Buffer)->getBuffer());
    if (!JSON)
      return JSON.takeError();
    Request[Field.first] = std::move(*JSON);
  }
  auto Code = read(Input);
  if (!Code)
    return Code.takeError();
  auto Bytes = (*Code)->getBuffer();
  std::string RequestText;
  llvm::raw_string_ostream(RequestText)
      << llvm::json::Value(std::move(Request));
  const char *Owned = neverd_bytecode_recover_json_v1(
      reinterpret_cast<const unsigned char *>(Bytes.data()), Bytes.size(),
      RequestText.data(), RequestText.size());
  if (!Owned)
    return invalid("bytecode recovery could not allocate its result");
  auto Parsed = llvm::json::parse(Owned);
  neverd_free_string(Owned);
  if (!Parsed)
    return Parsed.takeError();
  const auto *Result = Parsed->getAsObject();
  if (!Result || Result->getInteger("schemaVersion") != 1)
    return invalid("invalid bytecode recovery response");
  if (Result->getBoolean("ok") != true)
    return invalid(
        Result->getString("error").value_or("bytecode recovery failed"));
  llvm::errs() << "Decoded " << *Result->getInteger("functions")
               << " functions, " << *Result->getInteger("blocks") << " blocks, "
               << *Result->getInteger("decoded_bytes") << " reachable bytes of "
               << Bytes.size() << " input bytes\n";
  if (Check)
    return llvm::Error::success();
  auto Source = Result->getString("source");
  if (!Source)
    return invalid("bytecode recovery returned no source");
  return write(*Source);
}
} // namespace

int main(int Argc, char **Argv) {
  llvm::InitLLVM Init(Argc, Argv);
  llvm::cl::ParseCommandLineOptions(
      Argc, Argv, "NeverD external-profile bytecode source recovery\n");
  try {
    if (auto Error = run()) {
      llvm::errs() << llvm::toString(std::move(Error)) << '\n';
      return 1;
    }
  } catch (const std::exception &Error) {
    llvm::errs() << "bytecode source conversion failed: " << Error.what()
                 << '\n';
    return 1;
  }
  return 0;
}
