#pragma once
namespace llvm {
class Module;
}
namespace neverd {
/// Restore exact private frame pointer provenance. This shares the ordinary
/// optimizer's bounds/escape proof with analyst source storage preservation.
bool canonicalizePrivateFrameAccesses(llvm::Module &);
} // namespace neverd
