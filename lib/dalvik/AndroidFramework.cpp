//===- AndroidFramework.cpp - Explicit API 28 application projection ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared JNI/DEX identities over a declared lifecycle snapshot. This does not
/// execute ART startup, application constructors or arbitrary loader code.
//===----------------------------------------------------------------------===//
#include "AndroidFramework.h"

#include <algorithm>
#include <set>

namespace neverd::mobile::dalvik {
namespace {
bool platformType(std::string_view type) {
  return type.starts_with("Ljava/") || type.starts_with("Ljavax/") ||
         type.starts_with("Landroid/") || type.starts_with("Ldalvik/");
}
} // namespace

std::optional<bool>
androidApplicationLoaderState(const AndroidApplicationSnapshot &input) {
  using Phase = AndroidApplicationSnapshot::Phase;
  if ((input.phase == Phase::Unattached &&
       input.application_loader_established == true) ||
      (input.phase == Phase::Application &&
       input.application_loader_established == false))
    throw Error("Android application loader contradicts lifecycle phase");
  if (input.phase == Phase::Unattached)
    return false;
  if (input.phase == Phase::Application)
    return true;
  return input.application_loader_established;
}

void validateAndroidClassLookupFailures(
    const AndroidApplicationSnapshot &input) {
  using Loader = AndroidApplicationSnapshot::ClassLookupFailure::Loader;
  auto loader_state = androidApplicationLoaderState(input);
  if (input.class_lookup_failures.size() > 256)
    throw Error("Android class lookup failure inventory exceeds its budget");
  std::set<std::pair<Loader, std::string>> seen;
  for (const auto &failure : input.class_lookup_failures) {
    if ((failure.loader != Loader::Boot && failure.loader != Loader::System &&
         failure.loader != Loader::Application) ||
        (failure.loader == Loader::Application && loader_state != true))
      throw Error("invalid Android class lookup failure loader");
    const auto &name = failure.binary_name;
    bool first = true;
    if (name.empty() || name.size() > 1024)
      throw Error("invalid Android class lookup failure binary name");
    for (unsigned char byte : name) {
      if (byte == '.' && !first) {
        first = true;
        continue;
      }
      if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            byte == '_' || byte == '$' ||
            (!first && byte >= '0' && byte <= '9')))
        throw Error("invalid Android class lookup failure binary name");
      first = false;
    }
    if (first || !seen.emplace(failure.loader, name).second)
      throw Error("duplicate or invalid Android class lookup failure");
    if (failure.message) {
      if (failure.message->size() > 4096)
        throw Error("Android class lookup failure message exceeds its budget");
      (void)javaStringBytes(*failure.message);
    }
  }
}

void Program::addAndroidFramework() {
  // Only the hierarchy needed by this projection. Unmodeled members are not
  // supplied as empty executable declarations.
  for (auto [name, base] :
       {std::pair{"android/app/ClientTransactionHandler", "java/lang/Object"},
        {"android/app/ActivityThread", "android/app/ClientTransactionHandler"},
        {"android/app/Instrumentation", "java/lang/Object"},
        {"android/content/ComponentCallbacks", "java/lang/Object"},
        {"android/content/ComponentCallbacks2", "java/lang/Object"},
        {"android/content/Context", "java/lang/Object"},
        {"android/content/ContextWrapper", "android/content/Context"},
        {"android/app/Application", "android/content/ContextWrapper"},
        {"android/app/ContextImpl", "android/content/Context"},
        {"java/io/File", "java/lang/Object"},
        {"java/io/Serializable", "java/lang/Object"},
        {"java/lang/Comparable", "java/lang/Object"},
        {"java/lang/ClassLoader", "java/lang/Object"},
        {"java/lang/BootClassLoader", "java/lang/ClassLoader"},
        {"dalvik/system/BaseDexClassLoader", "java/lang/ClassLoader"},
        {"dalvik/system/PathClassLoader",
         "dalvik/system/BaseDexClassLoader"}}) {
    Class type;
    type.name = "L" + std::string(name) + ";";
    type.superclass = "L" + std::string(base) + ";";
    type.access = {"public"};
    if (std::string_view(name) == "android/content/Context" ||
        std::string_view(name) == "java/lang/ClassLoader" ||
        std::string_view(name) == "android/app/ClientTransactionHandler")
      type.access.insert("abstract");
    if (std::string_view(name) == "android/app/ActivityThread")
      type.access.insert("final");
    if (std::string_view(name) == "android/app/ContextImpl" ||
        std::string_view(name) == "java/lang/BootClassLoader")
      type.access.erase("public");
    if (std::string_view(name) == "android/content/ComponentCallbacks" ||
        std::string_view(name) == "android/content/ComponentCallbacks2" ||
        std::string_view(name) == "java/io/Serializable" ||
        std::string_view(name) == "java/lang/Comparable")
      type.access.insert({"abstract", "interface"});
    if (std::string_view(name) == "android/content/ComponentCallbacks2")
      type.interfaces.push_back("Landroid/content/ComponentCallbacks;");
    if (std::string_view(name) == "android/app/Application")
      type.interfaces.push_back("Landroid/content/ComponentCallbacks2;");
    if (std::string_view(name) == "java/io/File")
      type.interfaces = {"Ljava/io/Serializable;", "Ljava/lang/Comparable;"};
    type.source_id = "android-api28-framework";
    if (std::string_view(name) == "android/app/ActivityThread") {
      Field field;
      field.reference = {type.name, "mInstrumentation",
                         "Landroid/app/Instrumentation;"};
      type.fields.push_back(std::move(field));
    }
    auto [entry, inserted] = classes.emplace(type.name, std::move(type));
    if (inserted)
      for (const auto &field : entry->second.fields)
        android_framework_fields.insert(&field);
  }
}
bool Program::androidFrameworkField(const Field &field) const {
  return android_framework_fields.contains(&field);
}

AndroidFramework::AndroidFramework(
    std::optional<AndroidApplicationSnapshot> input, const Program &program)
    : snapshot(std::move(input)) {
  if (!snapshot)
    return;
  validateAndroidClassLookupFailures(*snapshot);
  for (const auto &failure : snapshot->class_lookup_failures) {
    std::string type = "L" + failure.binary_name + ";";
    std::replace(type.begin(), type.end(), '.', '/');
    if (program.knownReference(type) &&
        (platformType(type) || failure.loader ==
                                   AndroidApplicationSnapshot::
                                       ClassLookupFailure::Loader::Application))
      throw Error("Android class lookup failure shadows a visible declaration");
  }
  using Phase = AndroidApplicationSnapshot::Phase;
  if (snapshot->phase != Phase::Unattached &&
      snapshot->phase != Phase::Attached &&
      snapshot->phase != Phase::Application)
    throw Error("invalid Android application phase");
  if (snapshot->instrumentation &&
      (snapshot->phase == Phase::Unattached ||
       (*snapshot->instrumentation !=
            AndroidApplicationSnapshot::Instrumentation::Uninitialized &&
        *snapshot->instrumentation !=
            AndroidApplicationSnapshot::Instrumentation::Platform)))
    throw Error("invalid Android instrumentation snapshot");
  if (snapshot->package_name) {
    const auto &name = *snapshot->package_name;
    if (snapshot->phase != Phase::Application || name.empty() ||
        name.size() > 1024 || name.find('\0') != std::string::npos)
      throw Error("invalid Android application package snapshot");
    (void)javaStringBytes(name);
  }
  if (snapshot->files_directory) {
    auto &path = *snapshot->files_directory;
    if (snapshot->phase != Phase::Application || path.empty() ||
        path.front() != '/' || path.size() > 4095 ||
        path.find('\0') != std::string::npos)
      throw Error("invalid Android files directory snapshot");
    (void)javaStringBytes(path);
    // File's Unix lexical normalization only. Dot components are not resolved
    // and no host filesystem or user.dir property participates.
    std::string normalized;
    for (char byte : path)
      if (byte != '/' || normalized.empty() || normalized.back() != '/')
        normalized.push_back(byte);
    if (normalized.size() > 1 && normalized.back() == '/')
      normalized.pop_back();
    path = std::move(normalized);
  }
}

ExecutionResult
AndroidFramework::instrumentationValue(Heap &heap, uint64_t receiver,
                                       std::optional<uint64_t> write,
                                       const AdmitInstruction &admit) {
  if (!snapshot || !thread || receiver != thread)
    return {ExecutionStop::Unsupported,
            0,
            {},
            "unmodeled Android ActivityThread instance"};
  auto budget = admit();
  if (budget != ExecutionStop::Returned)
    return {budget, 0, {}, "Android framework work budget exhausted"};
  if (write)
    instrumentation = *write;
  if (!instrumentation) {
    if (!snapshot->instrumentation)
      return {ExecutionStop::Unsupported,
              0,
              {},
              "Android instrumentation state is not supplied"};
    if (*snapshot->instrumentation ==
        AndroidApplicationSnapshot::Instrumentation::Uninitialized) {
      instrumentation = 0;
    } else {
      auto value =
          heap.allocate(Object::Instance, "android/app/Instrumentation", 0);
      if (!value)
        return {ExecutionStop::HeapLimit,
                0,
                {},
                "Android framework storage budget exhausted"};
      instrumentation = value->Identity;
    }
  }
  return {ExecutionStop::Returned, *instrumentation, {}, {}};
}

ExecutionResult AndroidFramework::field(Heap &heap, const Field &field,
                                        uint64_t receiver,
                                        std::optional<uint64_t> write,
                                        const AdmitInstruction &admit) {
  const auto &ref = field.reference;
  if (ref.owner == "Landroid/app/ActivityThread;" &&
      ref.name == "mInstrumentation" &&
      ref.type == "Landroid/app/Instrumentation;" &&
      !has(field.access, "static"))
    return instrumentationValue(heap, receiver, write, admit);
  return {ExecutionStop::Unsupported,
          0,
          {},
          "Android framework field is not modeled"};
}

AndroidFrameworkState AndroidFramework::inspect() const {
  AndroidFrameworkState result;
  if (snapshot) {
    result.declared_phase = snapshot->phase;
    result.application_loader_established =
        androidApplicationLoaderState(*snapshot);
  }
  result.activity_thread = thread;
  result.application = application;
  result.context = context;
  result.boot_loader = boot_loader;
  result.system_loader = system_loader;
  result.application_loader = app_loader;
  result.instrumentation = instrumentation;
  return result;
}

std::optional<AndroidApplicationSnapshot::ClassLookupFailure::Loader>
AndroidFramework::definingLoader(std::string_view type,
                                 const Program &program) const {
  using Loader = AndroidApplicationSnapshot::ClassLookupFailure::Loader;
  if (!snapshot)
    return std::nullopt;
  const bool array = type.starts_with('[');
  while (type.starts_with('['))
    type.remove_prefix(1);
  if (array && type.size() == 1 &&
      std::string_view("ZBCSIJFD").find(type[0]) != std::string_view::npos)
    return Loader::Boot;
  if (!program.knownReference(type))
    return std::nullopt;
  if (platformType(type))
    return Loader::Boot;
  if (androidApplicationLoaderState(*snapshot) == true)
    return Loader::Application;
  return std::nullopt;
}

ExecutionResult AndroidFramework::execute(Heap &heap, const Program &program,
                                          const LibraryMethod &method,
                                          uint64_t receiver,
                                          std::span<const uint64_t> arguments,
                                          const AdmitInstruction &admit) {
  if (method.Op == ThreadInstrumentation)
    return instrumentationValue(heap, receiver, {}, admit);
  struct Stop {
    ExecutionResult result;
  };
  auto stop = [](ExecutionStop reason, const char *text) -> uint64_t {
    throw Stop{{reason, 0, {}, text}};
  };
  auto unsupported = [&](const char *text) {
    return stop(ExecutionStop::Unsupported, text);
  };
  auto tick = [&] {
    auto result = admit();
    if (result != ExecutionStop::Returned)
      stop(result, "Android framework work budget exhausted");
  };
  auto ref = [&](const std::shared_ptr<Object> &object) -> uint32_t {
    if (!object)
      stop(ExecutionStop::HeapLimit,
           "Android framework storage budget exhausted");
    return object->Identity;
  };
  auto identity = [&](uint32_t &slot, const char *name) -> uint32_t {
    if (!slot) {
      tick();
      slot = ref(heap.allocate(Object::Instance, name, 0));
    }
    return slot;
  };
  auto boot = [&] {
    return identity(boot_loader, "java/lang/BootClassLoader");
  };
  auto appLoader = [&] {
    if (androidApplicationLoaderState(*snapshot) != true)
      unsupported("Android application class loader is not established");
    return identity(app_loader, "dalvik/system/PathClassLoader");
  };
  auto app = [&]() -> uint32_t {
    if (snapshot->phase != AndroidApplicationSnapshot::Phase::Application)
      return 0;
    return identity(application, "android/app/Application");
  };
  auto string = [&](std::string_view text) -> uint32_t {
    auto bytes = javaStringBytes(text);
    for (size_t i = 0; i < bytes.size(); i += 2)
      tick();
    auto object =
        heap.allocate(Object::String, "java/lang/String", bytes.size());
    auto id = ref(object);
    object->Bytes = std::move(bytes);
    object->Length = object->Bytes.size() / 2;
    return id;
  };
  auto execute = [&]() -> uint64_t {
    if (!snapshot || !program.containsClass("Landroid/app/ActivityThread;"))
      return unsupported(
          "Android application lifecycle snapshot is not supplied");
    using Phase = AndroidApplicationSnapshot::Phase;
    switch (method.Op) {
    case CurrentActivityThread:
      return snapshot->phase == Phase::Unattached
                 ? 0
                 : identity(thread, "android/app/ActivityThread");
    case CurrentApplication:
      return app();
    case ThreadApplication:
      if (receiver != thread || !thread)
        return unsupported("unmodeled Android ActivityThread instance");
      return app();
    case ApplicationContext:
    case BaseContext:
    case ContextClassLoader:
    case PackageName:
    case FilesDirectory:
      if (!receiver || (receiver != application && receiver != context))
        return unsupported("unmodeled Android Context instance");
      if (method.Op == ApplicationContext)
        return app();
      if (method.Op == BaseContext)
        return identity(context, "android/app/ContextImpl");
      if (method.Op == ContextClassLoader)
        return appLoader();
      if (method.Op == FilesDirectory) {
        if (!snapshot->files_directory)
          return unsupported("Android files directory is not supplied");
        // Existing-directory projection: no directory creation or permission
        // effects are fabricated from a package name or path spelling.
        if (!files_path)
          files_path = string(*snapshot->files_directory);
        return identity(files_directory, "java/io/File");
      }
      if (!snapshot->package_name)
        return unsupported("Android application package name is not supplied");
      if (!package) {
        auto bytes = javaStringBytes(*snapshot->package_name);
        for (size_t i = 0; i < bytes.size(); i += 2)
          tick();
        auto text =
            heap.allocate(Object::String, "java/lang/String", bytes.size());
        package = ref(text);
        text->Bytes = std::move(bytes);
        text->Length = text->Bytes.size() / 2;
      }
      return package;
    case FilePath:
    case FileName:
    case FileParent:
    case FileIsAbsolute: {
      if (!receiver || receiver != files_directory)
        return unsupported("unmodeled Java File instance");
      if (method.Op == FilePath)
        return files_path;
      if (method.Op == FileIsAbsolute)
        return 1;
      const auto &path = *snapshot->files_directory;
      const auto slash = path.rfind('/');
      if (method.Op == FileName)
        return string(std::string_view(path).substr(slash + 1));
      if (path == "/")
        return 0;
      return string(std::string_view(path).substr(0, slash ? slash : 1));
    }
    case SystemClassLoader:
      return identity(system_loader, "dalvik/system/PathClassLoader");
    case LoaderParent:
      if (receiver == boot_loader && boot_loader)
        return 0;
      if ((receiver == system_loader && system_loader) ||
          (receiver == app_loader && app_loader))
        return boot();
      return unsupported("unmodeled Android ClassLoader instance");
    case ClassLoaderOf: {
      auto type = heap.get(receiver)->ClassName;
      if (!type.starts_with('['))
        type = "L" + type + ";";
      const auto owner = definingLoader(type, program);
      if (owner == AndroidApplicationSnapshot::ClassLookupFailure::Loader::Boot)
        return boot();
      if (owner)
        return appLoader();
      while (type.starts_with('['))
        type.erase(type.begin());
      if (program.knownReference(type) && !platformType(type))
        return unsupported(
            "Android application class loader is not established");
      return unsupported("unmodeled class loader ownership");
    }
    case LoadClass: {
      if (receiver != boot_loader && receiver != system_loader &&
          receiver != app_loader)
        return unsupported("unmodeled Android ClassLoader instance");
      auto name = heap.get(arguments[0]);
      if (!name)
        throw Stop{{ExecutionStop::Thrown, 0, "java/lang/NullPointerException",
                    "null Android class name"}};
      if (name->Length > 1024)
        return unsupported("Android class name exceeds the lookup budget");
      std::string text;
      for (size_t i = 0; i < name->Bytes.size(); i += 2) {
        tick();
        if (name->Bytes[i + 1] || name->Bytes[i] >= 128)
          return unsupported("non-ASCII Android class lookup is not modeled");
        text.push_back(name->Bytes[i] == '.' ? '/' : name->Bytes[i]);
      }
      // A negative outcome requires an exact declaration for this loader.
      // Absence from supplied DEX alone never implies class absence.
      if (text.empty() || text.front() == '[' ||
          std::find(name->Bytes.begin(), name->Bytes.end(), '/') !=
              name->Bytes.end())
        return unsupported("Android class lookup requires a known binary name");
      const auto type = "L" + text + ";";
      using Loader = AndroidApplicationSnapshot::ClassLookupFailure::Loader;
      const auto loader = receiver == boot_loader     ? Loader::Boot
                          : receiver == system_loader ? Loader::System
                                                      : Loader::Application;
      std::string binary = text;
      std::replace(binary.begin(), binary.end(), '/', '.');
      for (const auto &failure : snapshot->class_lookup_failures) {
        if (failure.loader != loader || failure.binary_name != binary)
          continue;
        uint32_t message = failure.message ? string(*failure.message) : 0;
        tick();
        auto exception = heap.allocate(Object::Throwable,
                                       "java/lang/ClassNotFoundException", 0);
        auto id = ref(exception);
        exception->Message = message;
        throw Stop{{ExecutionStop::Thrown, 0,
                    "java/lang/ClassNotFoundException",
                    "explicit Android class lookup failure", id}};
      }
      if (!program.knownReference(type) ||
          (!platformType(type) && receiver != app_loader))
        return unsupported(
            "Android class lookup has no supplied declaration in this loader");
      return ref(heap.classObject(text));
    }
    default:
      return stop(ExecutionStop::Invalid,
                  "invalid Android framework operation");
    }
  };
  try {
    return {ExecutionStop::Returned, execute(), {}, {}};
  } catch (Stop &halt) {
    return std::move(halt.result);
  }
}
} // namespace neverd::mobile::dalvik
