//===- Rules.cpp - Exact Node 22.15 API member tables -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Classify documented import-rooted syntax; retain unknown module values.
///
//===----------------------------------------------------------------------===//

#include "Rules.h"

#include "neverd/web/Error.h"
#include "neverd/web/NodeBehavior.h"

#include <algorithm>
#include <span>

namespace neverd::web::node_behavior {

void charge(uint64_t &Steps, uint64_t Count) {
  if (Steps > MaxNodeBehaviorSteps || Count > MaxNodeBehaviorSteps - Steps)
    throw Error("node_behavior_work_budget_exceeded");
  Steps += Count;
}

namespace {
struct Entry {
  std::u16string_view Member;
  std::string_view API;
};
constexpr Entry Processes[] = {{u"exec", "child_process.exec"},
                               {u"execSync", "child_process.execSync"},
                               {u"execFile", "child_process.execFile"},
                               {u"execFileSync", "child_process.execFileSync"},
                               {u"spawn", "child_process.spawn"},
                               {u"spawnSync", "child_process.spawnSync"},
                               {u"fork", "child_process.fork"}};
constexpr Entry Files[] = {{u"write", "fs.write"},
                           {u"writeSync", "fs.writeSync"},
                           {u"writev", "fs.writev"},
                           {u"writevSync", "fs.writevSync"},
                           {u"writeFile", "fs.writeFile"},
                           {u"writeFileSync", "fs.writeFileSync"},
                           {u"appendFile", "fs.appendFile"},
                           {u"appendFileSync", "fs.appendFileSync"},
                           {u"mkdir", "fs.mkdir"},
                           {u"mkdirSync", "fs.mkdirSync"},
                           {u"rename", "fs.rename"},
                           {u"renameSync", "fs.renameSync"},
                           {u"copyFile", "fs.copyFile"},
                           {u"copyFileSync", "fs.copyFileSync"},
                           {u"cp", "fs.cp"},
                           {u"cpSync", "fs.cpSync"},
                           {u"rm", "fs.rm"},
                           {u"rmSync", "fs.rmSync"},
                           {u"unlink", "fs.unlink"},
                           {u"unlinkSync", "fs.unlinkSync"},
                           {u"truncate", "fs.truncate"},
                           {u"truncateSync", "fs.truncateSync"},
                           {u"chmod", "fs.chmod"},
                           {u"chmodSync", "fs.chmodSync"},
                           {u"chown", "fs.chown"},
                           {u"chownSync", "fs.chownSync"},
                           {u"createWriteStream", "fs.createWriteStream"}};
// Promise APIs have their own closed method set. There is no module-level
// promises.write, promises.createWriteStream or synchronous promise method.
constexpr Entry Promises[] = {{u"writeFile", "fs.promises.writeFile"},
                              {u"appendFile", "fs.promises.appendFile"},
                              {u"mkdir", "fs.promises.mkdir"},
                              {u"rename", "fs.promises.rename"},
                              {u"copyFile", "fs.promises.copyFile"},
                              {u"cp", "fs.promises.cp"},
                              {u"rm", "fs.promises.rm"},
                              {u"unlink", "fs.promises.unlink"},
                              {u"truncate", "fs.promises.truncate"},
                              {u"chmod", "fs.promises.chmod"},
                              {u"chown", "fs.promises.chown"}};
constexpr Entry HTTP[] = {{u"get", "http.get"}, {u"request", "http.request"}};
constexpr Entry HTTPS[] = {{u"get", "https.get"},
                           {u"request", "https.request"}};

Rule table(std::span<const Entry> Entries, std::u16string_view Name,
           std::string_view Kind, uint64_t &Steps) {
  for (const auto &E : Entries) {
    charge(Steps, 1 + std::min(Name.size(), E.Member.size()));
    if (Name == E.Member)
      return {E.API, Kind};
  }
  return {};
}
} // namespace

Rule match(const SourceOrigin &O, const SourceModuleAnalysis &M,
           uint64_t &Steps) {
  charge(Steps);
  if (O.Request >= M.Requests.size())
    throw Error("invalid_node_behavior_origin");
  const auto &R = M.Requests[O.Request];
  if (R.SpecifierStatus != "literal" || R.Specifier >= M.Names.size())
    throw Error("invalid_node_behavior_request");
  if (O.Construction != NoSourceIndex)
    return {};
  std::u16string_view Module = M.Names[R.Specifier];
  if (Module.starts_with(u"node:"))
    Module.remove_prefix(5);
  auto Path = std::span<const std::u16string>(O.Members);
  if (O.Import != NoSourceIndex) {
    if (O.Import >= M.Imports.size() ||
        M.Imports[O.Import].Request != O.Request)
      throw Error("invalid_node_behavior_import");
    const auto &Import = M.Imports[O.Import];
    // Only a retained default import (including {default as name}) authorizes
    // this first projection. CJS/namespace .default is not normalized.
    if (Import.Kind != "namespace") {
      if (Import.ImportedName >= M.Names.size())
        throw Error("invalid_node_behavior_import");
      if (M.Names[Import.ImportedName] == u"default") {
        if (Path.empty() || Path.front() != u"default")
          throw Error("invalid_node_behavior_import");
        Path = Path.subspan(1);
      }
    }
  }
  // Both documented spellings have the same fixed promise-API vocabulary.
  if (Module == u"fs" && Path.size() == 2 && Path[0] == u"promises")
    return table(Promises, Path[1], "filesystem_mutation_call", Steps);
  if (Module == u"buffer" && Path.size() == 2 && Path[0] == u"Buffer" &&
      Path[1] == u"from")
    return {"buffer.Buffer.from", "buffer_conversion_call", true};
  if (Path.size() != 1)
    return {};
  if (Module == u"child_process")
    return table(Processes, Path[0], "process_creation_call", Steps);
  if (Module == u"fs")
    return table(Files, Path[0], "filesystem_mutation_call", Steps);
  if (Module == u"fs/promises")
    return table(Promises, Path[0], "filesystem_mutation_call", Steps);
  if (Module == u"http")
    return table(HTTP, Path[0], "outbound_request_call", Steps);
  if (Module == u"https")
    return table(HTTPS, Path[0], "outbound_request_call", Steps);
  if (Module == u"process" && Path[0] == u"dlopen")
    return {"process.dlopen", "native_load_call"};
  return {};
}

} // namespace neverd::web::node_behavior
