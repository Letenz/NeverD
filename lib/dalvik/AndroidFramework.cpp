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

namespace neverd::mobile::dalvik {
void Program::addAndroidFramework() {
  // Only the hierarchy needed by this projection. Unmodeled members are not
  // supplied as empty executable declarations.
  for (auto [name, base] :
       {std::pair{"android/app/ClientTransactionHandler", "java/lang/Object"},
        {"android/app/ActivityThread", "android/app/ClientTransactionHandler"},
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
    classes.emplace(type.name, std::move(type));
  }
}

AndroidFramework::AndroidFramework(
    std::optional<AndroidApplicationSnapshot> input)
    : snapshot(std::move(input)) {
  if (!snapshot)
    return;
  using Phase = AndroidApplicationSnapshot::Phase;
  if (snapshot->phase != Phase::Unattached &&
      snapshot->phase != Phase::Attached &&
      snapshot->phase != Phase::Application)
    throw Error("invalid Android application phase");
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

ExecutionResult AndroidFramework::execute(Heap &heap, const Program &program,
                                          const LibraryMethod &method,
                                          uint64_t receiver,
                                          std::span<const uint64_t> arguments,
                                          const AdmitInstruction &admit) {
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
    if (snapshot->phase != AndroidApplicationSnapshot::Phase::Application)
      unsupported("Android application class loader is not established");
    return identity(app_loader, "dalvik/system/PathClassLoader");
  };
  auto app = [&]() -> uint32_t {
    if (snapshot->phase != AndroidApplicationSnapshot::Phase::Application)
      return 0;
    return identity(application, "android/app/Application");
  };
  auto platform = [&](std::string_view type) {
    return type.starts_with("Ljava/") || type.starts_with("Ljavax/") ||
           type.starts_with("Landroid/") || type.starts_with("Ldalvik/");
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
      bool array = type.starts_with('[');
      if (!array)
        type = "L" + type + ";";
      while (type.starts_with('['))
        type.erase(type.begin());
      // Class objects representing primitive arrays have the boot loader;
      // this profile does not manufacture primitive Class objects.
      if (array && type.size() == 1 &&
          std::string_view("ZBCSIJFD").find(type[0]) != std::string_view::npos)
        return boot();
      if (!program.knownReference(type))
        return unsupported("unmodeled class loader ownership");
      return platform(type) ? boot() : appLoader();
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
      // Only known binary names, without initialization. Array/descriptor
      // spellings, missing classes and other namespaces remain unknown, not
      // fabricated ClassNotFoundException results or host classpath queries.
      if (text.empty() || text.front() == '[' ||
          std::find(name->Bytes.begin(), name->Bytes.end(), '/') !=
              name->Bytes.end())
        return unsupported("Android class lookup requires a known binary name");
      const auto type = "L" + text + ";";
      if (!program.knownReference(type) ||
          (!platform(type) && receiver != app_loader))
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
