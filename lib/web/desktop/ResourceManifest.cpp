//===- ResourceManifest.cpp - Supplied CEF/WebView resource index -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exact local resource declarations, never framework or runtime detection.
///
//===----------------------------------------------------------------------===//

#include "ManifestInternal.h"

#include "neverd/web/Error.h"

namespace neverd::web::desktop {
void readResourceManifest(DesktopManifest &A, const ManifestContext &Context,
                          const llvm::json::Object &O) {
  if (O.getInteger("schema_version") != 1 ||
      O.getString("profile").value_or("").str() != DesktopResourceProfile)
    throw Error("unsupported_desktop_resource_descriptor");
  for (const auto &KV : O)
    if (KV.first != "schema_version" && KV.first != "profile" &&
        KV.first != "runtime" && KV.first != "assets" &&
        KV.first != "native_helpers")
      throw Error("unsupported_desktop_resource_descriptor");
  const auto *Runtime = O.getObject("runtime");
  if (!Runtime)
    throw Error("invalid_desktop_resource_runtime");
  for (const auto &KV : *Runtime)
    if (KV.first != "file" && KV.first != "version")
      throw Error("invalid_desktop_resource_runtime");
  A.Entries.push_back(Context.entry("runtime", Runtime->get("file")));
  A.Declarations["runtime_version"] = shape(Runtime->get("version"));
  A.Declarations["runtime_identity"] = "supplied_declaration_unverified";
  auto Entries = [&](llvm::StringRef Key, uint64_t Maximum) {
    const auto *V = O.get(Key);
    if (!V && Key == "native_helpers")
      return;
    const auto *List = V ? V->getAsArray() : nullptr;
    if (!List || List->size() > Maximum)
      throw Error("desktop_resource_entry_budget_or_shape");
    for (uint32_t I = 0; I < List->size(); ++I) {
      auto E = Context.entry(Key.str(), &(*List)[I]);
      E.Ordinal = I;
      A.Entries.push_back(std::move(E));
    }
  };
  Entries("assets", 512);
  Entries("native_helpers", 128);
  A.Layout = Context.DirectoryNamespace ? "supplied_local_resource_index"
                                        : "no_directory_namespace";
}
} // namespace neverd::web::desktop
