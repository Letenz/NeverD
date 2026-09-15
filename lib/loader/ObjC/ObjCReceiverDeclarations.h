#ifndef NEVERD_LOADER_OBJC_RECEIVER_DECLARATIONS_H
#define NEVERD_LOADER_OBJC_RECEIVER_DECLARATIONS_H

#include "neverd/ir/SourceTypeHint.h"

#include "llvm/ADT/StringRef.h"

#include <optional>

namespace neverd {
struct BinaryImage;

namespace objc {
struct ReceiverMemberDeclaration {
  std::optional<SourceFunctionTypeHint> Signature;
  std::string ReturnClass;
  bool ReturnsReceiverType = false;
};

/// Facts for one declared class or protocol, including its categories.
/// An empty superclass denotes a declared root; no value means that no class
/// interface contributed hierarchy evidence. Unsupported members stay present.
struct ReceiverDeclarations {
  bool Present = false;
  bool Complete = true;
  std::optional<std::string> Superclass;
  std::vector<std::string> Protocols;
  std::vector<ReceiverMemberDeclaration> Members;
};

ReceiverDeclarations sdkReceiverDeclarations(const BinaryImage &Image,
                                             llvm::StringRef Name,
                                             bool Protocol, bool ClassMethod,
                                             llvm::StringRef Selector);
std::vector<std::string> sdkReceiverSubclasses(const BinaryImage &Image,
                                               llvm::StringRef Name);
} // namespace objc
} // namespace neverd
#endif
