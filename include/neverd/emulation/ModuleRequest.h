//===- ModuleRequest.h - Versioned execution resource requests ---*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Versioned execution resource requests.
///

#ifndef NEVERD_EMULATION_MODULEREQUEST_H
#define NEVERD_EMULATION_MODULEREQUEST_H

#include "neverd/emulation/AddressSpace.h"

#include <optional>
#include <string>
#include <utility>

namespace neverd::emulation {
enum class ModuleRequestKind {
  DeclaredModule,
  MissingDependency,
  UnmappedCandidate
};
enum class ModuleAccess { Read, Write, Fetch };
/// A pure environment mapping/linking plan needs another explicit resource.
/// InstanceID, when present, identifies an existing declaration in the same
/// address space. It authorizes no search, CPU recovery or implicit entry call.
class ModuleDependencyError : public llvm::ErrorInfo<ModuleDependencyError> {
public:
  static char ID;
  ModuleDependencyError(std::string Dependency,
                        std::optional<std::string> InstanceID = {},
                        std::string Detail = {})
      : Dependency(std::move(Dependency)), InstanceID(std::move(InstanceID)),
        Detail(std::move(Detail)) {}
  void log(llvm::raw_ostream &) const override;
  std::error_code convertToErrorCode() const override;
  std::string Dependency;
  std::optional<std::string> InstanceID;
  std::string Detail;
};
struct ModuleRequestToken {
  uint64_t Owner = 0, Version = 0, Sequence = 0;
  bool operator==(const ModuleRequestToken &) const = default;
};
/// Records a resource question, not permission to restart a faulted CPU. The
/// execution owner must separately prove a resumable instruction boundary.
struct ModuleRequest {
  ModuleRequestToken Token;
  ModuleRequestKind Kind = ModuleRequestKind::UnmappedCandidate;
  std::shared_ptr<AddressSpace> Space;
  std::optional<std::string> InstanceID, ModuleID;
  std::string Dependency;
  uint64_t PC = 0, Address = 0, Size = 0;
  ModuleAccess Access = ModuleAccess::Read;
  unsigned RequiredPermissions = 0;
};
} // namespace neverd::emulation
#endif
