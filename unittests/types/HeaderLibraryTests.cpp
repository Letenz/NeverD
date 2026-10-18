//===- HeaderLibraryTests.cpp - Type libraries of system headers ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/libc/LibCNames.h"
#include "neverd/types/HeaderLibrary.h"
#include "neverd/types/TypeParse.h"
#include "neverd/types/TypeRender.h"

using namespace neverd::types;

namespace {

std::optional<uint64_t> sizeOf(const TypeLibrary &Library,
                               llvm::StringRef Name) {
  const LocalType *Type = Library.find(Name);
  return Type ? Library.sizeOf(*makeNamed(Name.str(), Type->Kind))
              : std::nullopt;
}

/// The bytes the Win64 convention passes a parameter of \p Type in: a
/// scalar's own, a record's of 1, 2, 4 or 8 bytes, and a pointer's for a
/// larger record, which goes by its address, or an array, which decays.
std::optional<uint64_t> win64Bytes(const TypeLibrary &Library,
                                   const TypeExpr &Type) {
  const TypeExpr *Named = &Type;
  for (unsigned Depth = 0; Depth < 32 && Named->Kind == TypeExprKind::Named;
       ++Depth) {
    const LocalType *Typedef = Library.find(Named->Name);
    if (!Typedef || Typedef->Kind != TypeKind::Typedef || !Typedef->Target)
      break;
    Named = Typedef->Target.get();
  }
  if (Named->Kind == TypeExprKind::Array)
    return 8;
  const std::optional<uint64_t> Size = Library.sizeOf(*Named);
  const bool Record = Named->Kind == TypeExprKind::Record ||
                      (Named->Kind == TypeExprKind::Named &&
                       (Named->NamedKind == TypeKind::Struct ||
                        Named->NamedKind == TypeKind::Union));
  if (Record && Size && *Size != 1 && *Size != 2 && *Size != 4 && *Size != 8)
    return 8;
  return Size;
}

} // namespace

TEST(HeaderLibrary, TheKernelsReadsAsADriverDeclaresIt) {
  std::string Error;
  const TypeLibrary *Kernel =
      headerLibrary(HeaderLibrary::WindowsKernelX64, &Error);
  ASSERT_NE(Kernel, nullptr) << Error;
  EXPECT_EQ(Kernel->dataModel(), DataModel::LLP64);
  // Read once: the same library every time.
  EXPECT_EQ(headerLibrary(HeaderLibrary::WindowsKernelX64), Kernel);
  EXPECT_GT(Kernel->size(), 3000u);
  EXPECT_GT(Kernel->prototypes().size(), 1500u);
  for (const LocalType *Type : Kernel->types())
    EXPECT_EQ(Type->Origin, TypeOrigin::Header) << Type->Name;

  // The sizes x64 Windows gives them.
  EXPECT_EQ(sizeOf(*Kernel, "UNICODE_STRING"), 16u);
  EXPECT_EQ(sizeOf(*Kernel, "OBJECT_ATTRIBUTES"), 48u);
  EXPECT_EQ(sizeOf(*Kernel, "IO_STATUS_BLOCK"), 16u);
  EXPECT_EQ(sizeOf(*Kernel, "LIST_ENTRY"), 16u);
  EXPECT_EQ(sizeOf(*Kernel, "NTSTATUS"), 4u);
  EXPECT_EQ(sizeOf(*Kernel, "HANDLE"), 8u);
  EXPECT_EQ(Kernel->alignOf(*makeNamed("SLIST_ENTRY", TypeKind::Typedef)), 16u);

  // Prototypes as the WDK declares them, parameters named.
  const FunctionDef *Create = Kernel->findPrototype("NtCreateFile");
  ASSERT_NE(Create, nullptr);
  ASSERT_EQ(Create->Params.size(), 11u);
  EXPECT_EQ(Create->Params[0].Name, "FileHandle");
  EXPECT_EQ(Create->Params[10].Name, "EaLength");
  EXPECT_EQ(Create->Return->Name, "NTSTATUS");
  ASSERT_NE(Kernel->findPrototype("KeBugCheckEx"), nullptr);
  EXPECT_TRUE(Kernel->findPrototype("KeBugCheckEx")->NoReturn);
  EXPECT_FALSE(Kernel->findPrototype("IoCreateDevice")->NoReturn);
}

TEST(HeaderLibrary, EveryLibraryReadsLaidOutForItsDataModel) {
  // A pointer, and a SIZE_T, take what each data model gives them: a 32-bit
  // library's handles and records are a 32-bit program's.
  const HeaderLibrary Libraries[] = {
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling)            \
  HeaderLibrary::Id,
#include "neverd/types/HeaderLibraries.def"
  };
  for (const HeaderLibrary Library : Libraries) {
    SCOPED_TRACE(headerLibraryName(Library).str());
    std::string Error;
    const TypeLibrary *Types = headerLibrary(Library, &Error);
    ASSERT_NE(Types, nullptr) << Error;
    EXPECT_EQ(Types->dataModel(), headerLibraryModel(Library));
    EXPECT_EQ(Types->recordABI(), headerLibraryRecordABI(Library));
    EXPECT_GT(Types->size(), 300u);
    EXPECT_GT(Types->prototypes().size(), 1000u);
    const bool Wide = headerLibraryModel(Library) != DataModel::ILP32;
    if (headerLibrarySpelling(Library) == BuiltinSpelling::Windows) {
      EXPECT_EQ(sizeOf(*Types, "HANDLE"), Wide ? 8u : 4u);
      EXPECT_EQ(sizeOf(*Types, "SIZE_T"), Wide ? 8u : 4u);
      EXPECT_EQ(sizeOf(*Types, "_UNICODE_STRING"), Wide ? 16u : 8u);
    } else if (Types->find("EFI_GUID")) {
      // UEFI's own names, which hold no socket.
      EXPECT_EQ(sizeOf(*Types, "UINTN"), Wide ? 8u : 4u);
      EXPECT_EQ(sizeOf(*Types, "EFI_HANDLE"), Wide ? 8u : 4u);
      EXPECT_EQ(sizeOf(*Types, "EFI_GUID"), 16u);
    } else {
      EXPECT_EQ(sizeOf(*Types, "size_t"), Wide ? 8u : 4u);
      EXPECT_EQ(sizeOf(*Types, "sockaddr_in"), 16u);
    }
  }
}

TEST(HeaderLibrary, TheNoReturnTablesHoldEveryNoReturnRoutine) {
  // Function discovery and the call analysis read the tables, before any
  // header: a routine the headers declare noreturn ends the code that calls
  // it with or without them.
  const HeaderLibrary Libraries[] = {
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling)            \
  HeaderLibrary::Id,
#include "neverd/types/HeaderLibraries.def"
  };
  for (const HeaderLibrary Library : Libraries) {
    const TypeLibrary *Types = headerLibrary(Library);
    ASSERT_NE(Types, nullptr);
    for (const auto &Prototype : Types->prototypes())
      if (Prototype.getValue()->NoReturn)
        EXPECT_TRUE(neverd::libc::isNoReturnFunction(Prototype.getKey()))
            << headerLibraryName(Library).str() << " "
            << Prototype.getKey().str();
  }
}

TEST(HeaderLibrary, TheUserLibrariesDeclareNtdllsNativeAPI) {
  // phnt's native API: the routines ntdll exports, and the records a
  // program reaches from its TEB, laid out as each architecture's ntdll
  // reads them (ARM64's as x64's).
  struct Offset {
    const char *Record;
    const char *Member;
    uint64_t Wide, Narrow;
  };
  const Offset Offsets[] = {
      {"TEB", "ProcessEnvironmentBlock", 0x60, 0x30},
      {"TEB", "LastErrorValue", 0x68, 0x34},
      {"PEB", "Ldr", 0x18, 0x0c},
      {"PEB", "ProcessParameters", 0x20, 0x10},
      {"PEB_LDR_DATA", "InMemoryOrderModuleList", 0x20, 0x14},
      {"LDR_DATA_TABLE_ENTRY", "DllBase", 0x30, 0x18},
      {"RTL_USER_PROCESS_PARAMETERS", "ImagePathName", 0x60, 0x38},
  };
  for (const HeaderLibrary Library :
       {HeaderLibrary::WindowsUserX64, HeaderLibrary::WindowsUserX86,
        HeaderLibrary::WindowsUserARM64}) {
    SCOPED_TRACE(headerLibraryName(Library).str());
    std::string Error;
    const TypeLibrary *Types = headerLibrary(Library, &Error);
    ASSERT_NE(Types, nullptr) << Error;
    const FunctionDef *Load = Types->findPrototype("LdrLoadDll");
    ASSERT_NE(Load, nullptr);
    ASSERT_EQ(Load->Params.size(), 4u);
    EXPECT_EQ(Load->Params[3].Name, "DllHandle");
    const FunctionDef *Query = Types->findPrototype("NtQuerySystemInformation");
    ASSERT_NE(Query, nullptr);
    EXPECT_EQ(Query->Params.size(), 4u);
    EXPECT_EQ(Query->Return->Name, "NTSTATUS");
    // phnt's <ntzwapi.h> declares the Zw routines <winternl.h> does not.
    const FunctionDef *Close = Types->findPrototype("ZwClose");
    ASSERT_NE(Close, nullptr);
    EXPECT_EQ(Close->Params.size(), 1u);

    const bool Wide = headerLibraryModel(Library) != DataModel::ILP32;
    std::string Assertions;
    for (const Offset &O : Offsets)
      Assertions += std::string("_Static_assert(__builtin_offsetof(") +
                    O.Record + ", " + O.Member +
                    ") == " + std::to_string(Wide ? O.Wide : O.Narrow) +
                    ", \"" + O.Record + "." + O.Member + "\");\n";
    // A copy, which the assertions read without changing the library.
    TypeLibrary Layout = *Types;
    const ParseResult Laid = parseCTypes(Assertions, Layout);
    EXPECT_TRUE(Laid.Problems.empty()) << Laid.Problems.front().Message;
  }
}

TEST(HeaderLibrary, TheLinuxLibrariesLayRecordsOutAsGlibcDoes) {
  // glibc's records as each architecture's compilers lay them out, as clang
  // measures them against the same headers: i386's System V ABI aligns
  // FILE's 64-bit _offset to four and AAPCS32 to eight, and a 32-bit
  // program's time_t is 32 bits.
  struct Expected {
    HeaderLibrary Library;
    uint64_t Stat, StatSize, Timespec, Mutex, File, FileOffset, Sigaction,
        Dirent;
  };
  const Expected Libraries[] = {
      {HeaderLibrary::LinuxX64, 144, 48, 16, 40, 216, 144, 152, 280},
      {HeaderLibrary::LinuxX86, 88, 44, 8, 24, 148, 76, 140, 268},
      {HeaderLibrary::LinuxARM64, 128, 48, 16, 48, 216, 144, 152, 280},
      {HeaderLibrary::LinuxARM, 88, 44, 8, 24, 152, 80, 140, 268},
  };
  for (const Expected &E : Libraries) {
    SCOPED_TRACE(headerLibraryName(E.Library).str());
    std::string Error;
    const TypeLibrary *Types = headerLibrary(E.Library, &Error);
    ASSERT_NE(Types, nullptr) << Error;
    const auto Check = [](const char *What, uint64_t Value) {
      return "_Static_assert(" + std::string(What) +
             " == " + std::to_string(Value) + ", \"" + What + "\");\n";
    };
    const std::string Assertions =
        Check("sizeof(struct stat)", E.Stat) +
        Check("__builtin_offsetof(struct stat, st_size)", E.StatSize) +
        Check("sizeof(struct timespec)", E.Timespec) +
        Check("sizeof(pthread_mutex_t)", E.Mutex) +
        Check("sizeof(FILE)", E.File) +
        Check("__builtin_offsetof(FILE, _offset)", E.FileOffset) +
        Check("sizeof(struct sigaction)", E.Sigaction) +
        Check("sizeof(struct dirent)", E.Dirent);
    // A copy, which the assertions read without changing the library.
    TypeLibrary Layout = *Types;
    const ParseResult Laid = parseCTypes(Assertions, Layout);
    EXPECT_TRUE(Laid.Problems.empty()) << Laid.Problems.front().Message;

    const FunctionDef *Read = Types->findPrototype("read");
    ASSERT_NE(Read, nullptr);
    ASSERT_EQ(Read->Params.size(), 3u);
    EXPECT_EQ(Read->Params[2].Name, "__nbytes");
    // glibc 2.38 names strtol __isoc23_strtol for _GNU_SOURCE: a program
    // imports that name.
    for (const char *Name : {"strtol", "__isoc23_strtol"}) {
      const FunctionDef *Number = Types->findPrototype(Name);
      ASSERT_NE(Number, nullptr) << Name;
      EXPECT_EQ(Number->Params.size(), 3u) << Name;
    }
    ASSERT_NE(Types->findPrototype("exit"), nullptr);
    EXPECT_TRUE(Types->findPrototype("exit")->NoReturn);
  }
}

TEST(HeaderLibrary, TheAndroidLibrariesLayRecordsOutAsBionicDoes) {
  // bionic's records as each architecture's compilers lay them out, as clang
  // measures them against the same headers for API level 35 and the NDK's:
  // not glibc's, whose 32-bit pthread_mutex_t is 24 bytes and whose x64
  // sigaction 152.
  struct Expected {
    HeaderLibrary Library;
    uint64_t Stat, StatSize, Timespec, Mutex, Sigaction, Dirent;
  };
  const Expected Libraries[] = {
      {HeaderLibrary::AndroidX64, 144, 48, 16, 40, 32, 280},
      {HeaderLibrary::AndroidX86, 96, 44, 8, 4, 16, 276},
      {HeaderLibrary::AndroidARM64, 128, 48, 16, 40, 32, 280},
      {HeaderLibrary::AndroidARM, 104, 48, 8, 4, 16, 280},
  };
  for (const Expected &E : Libraries) {
    SCOPED_TRACE(headerLibraryName(E.Library).str());
    std::string Error;
    const TypeLibrary *Types = headerLibrary(E.Library, &Error);
    ASSERT_NE(Types, nullptr) << Error;
    const auto Check = [](const char *What, uint64_t Value) {
      return "_Static_assert(" + std::string(What) +
             " == " + std::to_string(Value) + ", \"" + What + "\");\n";
    };
    const std::string Assertions =
        Check("sizeof(struct stat)", E.Stat) +
        Check("__builtin_offsetof(struct stat, st_size)", E.StatSize) +
        Check("sizeof(struct timespec)", E.Timespec) +
        Check("sizeof(pthread_mutex_t)", E.Mutex) +
        Check("sizeof(struct sigaction)", E.Sigaction) +
        Check("sizeof(struct dirent)", E.Dirent);
    // A copy, which the assertions read without changing the library.
    TypeLibrary Layout = *Types;
    const ParseResult Laid = parseCTypes(Assertions, Layout);
    EXPECT_TRUE(Laid.Problems.empty()) << Laid.Problems.front().Message;

    const FunctionDef *Init = Types->findPrototype("pthread_mutex_init");
    ASSERT_NE(Init, nullptr);
    EXPECT_EQ(Init->Params.size(), 2u);
    const FunctionDef *Read = Types->findPrototype("read");
    ASSERT_NE(Read, nullptr);
    ASSERT_EQ(Read->Params.size(), 3u);
    EXPECT_EQ(Read->Params[2].Name, "__count");
    ASSERT_NE(Types->findPrototype("__assert2"), nullptr);
    EXPECT_TRUE(Types->findPrototype("__assert2")->NoReturn);
    // bionic's strtol_l, which takes a locale, is strtol's symbol: strtol's
    // own declaration describes strtol.
    for (const char *Name : {"strtol", "strtoul", "wcstol"}) {
      const FunctionDef *Number = Types->findPrototype(Name);
      ASSERT_NE(Number, nullptr) << Name;
      EXPECT_EQ(Number->Params.size(), 3u) << Name;
    }

    // The NDK's <jni.h> and <android/log.h>: JNIEnv's function table, a
    // pointer for each of its 233 entries, and the log.
    const bool Wide = headerLibraryModel(E.Library) != DataModel::ILP32;
    const std::string JNI =
        Check("sizeof(struct JNINativeInterface)", Wide ? 1864 : 932) +
        Check("sizeof(JNIEnv)", Wide ? 8 : 4) +
        Check("sizeof(JavaVMInitArgs)", Wide ? 24 : 16) +
        Check("sizeof(jvalue)", 8);
    const ParseResult Native = parseCTypes(JNI, Layout);
    EXPECT_TRUE(Native.Problems.empty()) << Native.Problems.front().Message;
    const FunctionDef *Create = Types->findPrototype("JNI_CreateJavaVM");
    ASSERT_NE(Create, nullptr);
    EXPECT_EQ(Create->Params.size(), 3u);
    const FunctionDef *Print = Types->findPrototype("__android_log_print");
    ASSERT_NE(Print, nullptr);
    EXPECT_EQ(Print->Params.size(), 3u);
    EXPECT_TRUE(Print->Variadic);
    ASSERT_NE(Types->findPrototype("__android_log_assert"), nullptr);
    EXPECT_TRUE(Types->findPrototype("__android_log_assert")->NoReturn);
  }
}

TEST(HeaderLibrary, TheMuslLibrariesLayRecordsOutAsMuslDoes) {
  // musl's records as each architecture's compilers lay them out, as clang
  // measures them against the headers musl installs for it: a 32-bit
  // program's time_t is 64 bits, which makes its stat and timespec larger
  // than glibc's and bionic's, and its stat is __stat_time64.
  struct Expected {
    HeaderLibrary Library;
    uint64_t Stat, StatSize, Timespec, Mutex, Sigaction, Dirent;
    bool Time64;
  };
  const Expected Libraries[] = {
      {HeaderLibrary::MuslX64, 144, 48, 16, 40, 152, 280, false},
      {HeaderLibrary::MuslX86, 144, 44, 16, 24, 140, 276, true},
      {HeaderLibrary::MuslARM64, 128, 48, 16, 40, 152, 280, false},
      {HeaderLibrary::MuslARM, 152, 48, 16, 24, 140, 280, true},
  };
  for (const Expected &E : Libraries) {
    SCOPED_TRACE(headerLibraryName(E.Library).str());
    std::string Error;
    const TypeLibrary *Types = headerLibrary(E.Library, &Error);
    ASSERT_NE(Types, nullptr) << Error;
    const auto Check = [](const char *What, uint64_t Value) {
      return "_Static_assert(" + std::string(What) +
             " == " + std::to_string(Value) + ", \"" + What + "\");\n";
    };
    const std::string Assertions =
        Check("sizeof(struct stat)", E.Stat) +
        Check("__builtin_offsetof(struct stat, st_size)", E.StatSize) +
        Check("sizeof(struct timespec)", E.Timespec) +
        Check("sizeof(pthread_mutex_t)", E.Mutex) +
        Check("sizeof(struct sigaction)", E.Sigaction) +
        Check("sizeof(struct dirent)", E.Dirent);
    // A copy, which the assertions read without changing the library.
    TypeLibrary Layout = *Types;
    const ParseResult Laid = parseCTypes(Assertions, Layout);
    EXPECT_TRUE(Laid.Problems.empty()) << Laid.Problems.front().Message;

    const FunctionDef *Read = Types->findPrototype("read");
    ASSERT_NE(Read, nullptr);
    EXPECT_EQ(Read->Params.size(), 3u);
    const FunctionDef *Stat = Types->findPrototype("__stat_time64");
    EXPECT_EQ(Stat != nullptr, E.Time64);
    if (Stat)
      EXPECT_EQ(Stat->Params.size(), 2u);
    ASSERT_NE(Types->findPrototype("exit"), nullptr);
    EXPECT_TRUE(Types->findPrototype("exit")->NoReturn);
  }
}

TEST(HeaderLibrary, TheDarwinLibrariesLayRecordsOutAsLibSystemDoes) {
  // libSystem's records as clang lays them out against the headers Apple's
  // projects install, for each platform; macOS and iOS share them, but for
  // the registers jmp_buf saves.  statfs's 2168 bytes are what the SDK's
  // encoding of fstatfs$INODE64 says too (DarwinSourceDeclarations.inc).
  struct Expected {
    HeaderLibrary Library;
    uint64_t JmpBuf;
    // An x64 program imports stat as stat$INODE64; ARM64 has only 64-bit
    // inodes.
    bool Inode64;
    // macOS names syslog syslog$DARWIN_EXTSN; iOS's is the plain one.
    bool ExtendedSyslog;
  };
  const Expected Libraries[] = {
      {HeaderLibrary::MacOSX64, 148, true, true},
      {HeaderLibrary::MacOSARM64, 192, false, true},
      {HeaderLibrary::IOSARM64, 192, false, false},
  };
  for (const Expected &E : Libraries) {
    SCOPED_TRACE(headerLibraryName(E.Library).str());
    std::string Error;
    const TypeLibrary *Types = headerLibrary(E.Library, &Error);
    ASSERT_NE(Types, nullptr) << Error;
    const auto Check = [](const char *What, uint64_t Value) {
      return "_Static_assert(" + std::string(What) +
             " == " + std::to_string(Value) + ", \"" + What + "\");\n";
    };
    const std::string Assertions =
        Check("sizeof(struct stat)", 144) +
        Check("__builtin_offsetof(struct stat, st_size)", 96) +
        Check("sizeof(struct timespec)", 16) +
        Check("sizeof(pthread_mutex_t)", 64) + Check("sizeof(FILE)", 152) +
        Check("__builtin_offsetof(FILE, _offset)", 144) +
        Check("sizeof(struct sigaction)", 16) +
        Check("sizeof(struct dirent)", 1048) +
        Check("sizeof(struct statfs)", 2168) +
        Check("sizeof(jmp_buf)", E.JmpBuf);
    // A copy, which the assertions read without changing the library.
    TypeLibrary Layout = *Types;
    const ParseResult Laid = parseCTypes(Assertions, Layout);
    EXPECT_TRUE(Laid.Problems.empty()) << Laid.Problems.front().Message;

    const FunctionDef *Stat = Types->findPrototype("stat");
    ASSERT_NE(Stat, nullptr);
    EXPECT_EQ(Stat->Params.size(), 2u);
    const FunctionDef *Inode64 = Types->findPrototype("stat$INODE64");
    EXPECT_EQ(Inode64 != nullptr, E.Inode64);
    if (Inode64)
      EXPECT_EQ(Inode64->Params.size(), 2u);
    ASSERT_NE(Types->findPrototype("realpath$DARWIN_EXTSN"), nullptr);
    EXPECT_EQ(Types->findPrototype("syslog$DARWIN_EXTSN") != nullptr,
              E.ExtendedSyslog);
    ASSERT_NE(Types->findPrototype("_dyld_image_count"), nullptr);
    ASSERT_NE(Types->findPrototype("os_unfair_lock_lock"), nullptr);
    ASSERT_NE(Types->findPrototype("exit"), nullptr);
    EXPECT_TRUE(Types->findPrototype("exit")->NoReturn);
  }
}

TEST(HeaderLibrary, TheUefiLibrariesLayOutTheFirmwaresTables) {
  // The tables a UEFI module is given and the protocols it locates, as clang
  // lays them out against MdePkg for each architecture.  The library classes
  // of one kind of module are left out: each declares _ModuleEntryPoint its
  // way, and the specification's EFI_IMAGE_ENTRY_POINT types an
  // application's entry instead.
  struct Expected {
    HeaderLibrary Library;
    uint64_t SystemTable, BootServicesAt, BootServices, LocateProtocolAt,
        RuntimeServices, LoadedImage, ImageSizeAt, FileProtocol, OutputStringAt;
  };
  const Expected Libraries[] = {
      {HeaderLibrary::UefiX64, 120, 96, 376, 320, 136, 96, 72, 120, 8},
      {HeaderLibrary::UefiX86, 72, 60, 200, 172, 80, 64, 40, 64, 4},
      {HeaderLibrary::UefiARM64, 120, 96, 376, 320, 136, 96, 72, 120, 8},
  };
  for (const Expected &E : Libraries) {
    SCOPED_TRACE(headerLibraryName(E.Library).str());
    std::string Error;
    const TypeLibrary *Types = headerLibrary(E.Library, &Error);
    ASSERT_NE(Types, nullptr) << Error;
    const auto Check = [](const char *What, uint64_t Value) {
      return "_Static_assert(" + std::string(What) +
             " == " + std::to_string(Value) + ", \"" + What + "\");\n";
    };
    const std::string Assertions =
        Check("sizeof(EFI_SYSTEM_TABLE)", E.SystemTable) +
        Check("__builtin_offsetof(EFI_SYSTEM_TABLE, BootServices)",
              E.BootServicesAt) +
        Check("sizeof(EFI_BOOT_SERVICES)", E.BootServices) +
        Check("__builtin_offsetof(EFI_BOOT_SERVICES, LocateProtocol)",
              E.LocateProtocolAt) +
        Check("sizeof(EFI_RUNTIME_SERVICES)", E.RuntimeServices) +
        Check("sizeof(EFI_LOADED_IMAGE_PROTOCOL)", E.LoadedImage) +
        Check("__builtin_offsetof(EFI_LOADED_IMAGE_PROTOCOL, ImageSize)",
              E.ImageSizeAt) +
        Check("sizeof(EFI_FILE_PROTOCOL)", E.FileProtocol) +
        Check("__builtin_offsetof(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL, "
              "OutputString)",
              E.OutputStringAt) +
        Check("sizeof(EFI_GUID)", 16) +
        Check("sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION)", 36) +
        Check("sizeof(SMBIOS_TABLE_ENTRY_POINT)", 31);
    // A copy, which the assertions read without changing the library.
    TypeLibrary Layout = *Types;
    const ParseResult Laid = parseCTypes(Assertions, Layout);
    EXPECT_TRUE(Laid.Problems.empty()) << Laid.Problems.front().Message;

    const LocalType *Entry = Types->find("EFI_IMAGE_ENTRY_POINT");
    ASSERT_NE(Entry, nullptr);
    EXPECT_EQ(Entry->Kind, TypeKind::Typedef);
    EXPECT_EQ(Types->findPrototype("_ModuleEntryPoint"), nullptr);
    const FunctionDef *Copy = Types->findPrototype("CopyMem");
    ASSERT_NE(Copy, nullptr);
    EXPECT_EQ(Copy->Params.size(), 3u);
    const FunctionDef *Print = Types->findPrototype("AsciiPrint");
    ASSERT_NE(Print, nullptr);
    EXPECT_TRUE(Print->Variadic);
  }
}

TEST(HeaderLibrary, JoinsWhereTheProgramHasNoTypeOfTheName) {
  TypeLibrary Headers;
  ParseOptions Options;
  Options.Origin = TypeOrigin::Header;
  ASSERT_TRUE(parseCTypes(R"(
    typedef struct _OBJ { ULONG A; } OBJ, *POBJ;
    typedef struct _HOLDER { struct _OBJ Obj; ULONG Tail; } HOLDER;
    typedef struct _USER { struct _HOLDER Holder; } USER;
    typedef struct _POINTS { POBJ First; } POINTS;
    VOID Use(POBJ Object);
  )",
                          Headers, Options)
                  .ok());
  // The program's _OBJ, of another size, and a prototype of its own.
  TypeLibrary Program;
  ASSERT_TRUE(parseCTypes("struct _OBJ { ULONGLONG A, B; };\n"
                          "ULONG Use(void *Object);",
                          Program)
                  .ok());
  const HeaderMerge Merge = mergeHeaderTypes(Program, Headers);
  // _HOLDER was laid out with the headers' _OBJ, and _USER holds it: both
  // go, with the typedefs that name them, though a record stays declared
  // for what points to it.  OBJ names the program's _OBJ, and _POINTS only
  // points to it.
  EXPECT_EQ(Merge.Conflicts,
            (std::vector<std::string>{"_HOLDER", "HOLDER", "_USER", "USER"}));
  const LocalType *Holder = Program.find("_HOLDER");
  ASSERT_NE(Holder, nullptr);
  ASSERT_TRUE(Holder->Record);
  EXPECT_FALSE(Holder->Record->Defined);
  EXPECT_EQ(Program.find("HOLDER"), nullptr);
  EXPECT_EQ(sizeOf(Program, "OBJ"), 16u);
  EXPECT_NE(Program.find("_POINTS"), nullptr);
  EXPECT_EQ(Program.find("_POINTS")->Origin, TypeOrigin::Header);
  EXPECT_EQ(Program.find("_OBJ")->Origin, TypeOrigin::User);
  // The program's own prototype stays.
  EXPECT_EQ(Program.findPrototype("Use")->Return->TheBuiltin, Builtin::ULong);
  EXPECT_EQ(Merge.Prototypes, 0u);
}

TEST(HeaderLibrary, DeclaresTheKernelRoutinesTheCallAnalysisFixes) {
  // WindowsKernelRoutines.inc fixes the bytes each parameter of a routine
  // takes, which the call analysis reads before any prototype; the kernel's
  // headers declare the same routines and must agree.
  const TypeLibrary *Kernel = headerLibrary(HeaderLibrary::WindowsKernelX64);
  ASSERT_NE(Kernel, nullptr);
  struct Routine {
    llvm::StringRef Name;
    std::vector<uint64_t> Bytes;
  };
  const Routine Routines[] = {
#define ND_WINDOWS_DISPATCH_THUNK(Name)
#define ND_WINDOWS_PROTO(Name, ...) {Name, {__VA_ARGS__}},
#include "neverd/libc/WindowsKernelRoutines.inc"
#undef ND_WINDOWS_PROTO
#undef ND_WINDOWS_DISPATCH_THUNK
  };
  size_t Declared = 0;
  for (const Routine &R : Routines) {
    const FunctionDef *Function = Kernel->findPrototype(R.Name);
    if (!Function)
      continue;
    ++Declared;
    ASSERT_EQ(Function->Params.size(), R.Bytes.size()) << R.Name.str();
    for (size_t I = 0; I < R.Bytes.size(); ++I)
      EXPECT_EQ(win64Bytes(*Kernel, *Function->Params[I].Type), R.Bytes[I])
          << R.Name.str() << " parameter " << I;
  }
  EXPECT_GT(Declared, 0u);
}
