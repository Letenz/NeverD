//===- ARM32_StatusRegisterTests.cpp - MRS, MSR and CPS -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The lift keeps N, Z, C, V and GE as flags and the machine keeps Q, the mode
// and the interrupt masks (test_status_registers_arm.s).  An MSR writes only
// the fields of its mask, so a mode change leaves the flags a comparison set;
// an MRS merges the flags the function set into the machine's word; both C
// routes run each instruction as itself, and the C builds for ARM and Thumb.
//
//===----------------------------------------------------------------------===//

#include "CSourceText.h"
#include "NeverDLiftFixture.h"

#include <fstream>
#include <sstream>

namespace {

fs::path statusObj() {
  return fs::path(TEST_OBJ_DIR) / "test_status_registers_arm.o";
}

std::string readText(const fs::path &Path) {
  std::ifstream In(Path);
  std::ostringstream Text;
  Text << In.rdbuf();
  return Text.str();
}

/// The definition of \p Name in C source \p C, from its return type.
std::string definition(const std::string &C, const std::string &Name) {
  const size_t At = neverd::test::cDefinitionAt(C, Name);
  if (At == std::string::npos)
    return {};
  const size_t Begin = C.rfind('\n', At) + 1;
  return C.substr(Begin, C.find("\n}", At) - Begin);
}

/// The text of \p Definition after its first \p Marker.
std::string after(const std::string &Definition, const std::string &Marker) {
  const size_t At = Definition.find(Marker);
  return At == std::string::npos ? std::string() : Definition.substr(At);
}

} // namespace

class ARM32_StatusRegister : public NeverDLiftTest {
protected:
  /// The unit a C route writes for the fixture.
  std::string source(bool LLVM) {
    const auto R =
        LLVM ? decompileToC(statusObj()) : decompileToHighC(statusObj());
    EXPECT_EQ(R.exitCode, 0) << R.err;
    return readText(tmpFile(LLVM ? "decompiled.c" : "decompiled_high.c"));
  }
};

TEST_F(ARM32_StatusRegister, FieldsGoWhereTheMaskSays) {
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const std::string C = source(LLVM);
    // A mode change keeps the flags the comparison set.
    const std::string Mode = definition(C, "flags_survive_mode_write");
    EXPECT_NE(Mode.find("\"msr cpsr_c, %0\""), std::string::npos) << C;
    EXPECT_NE(after(Mode, "msr cpsr_c").find("arg0"), std::string::npos) << C;
    // N, Z, C and V come from bits 31:28 of the word; the machine takes Q.
    const std::string Restore = definition(C, "restore_flags");
    EXPECT_NE(Restore.find("\"msr cpsr_f, %0\""), std::string::npos) << C;
    EXPECT_NE(after(Restore, "msr cpsr_f").find("arg0"), std::string::npos)
        << C;
    // GE is lifted: SEL picks from the word's bits 19:16 with no `asm`.
    const std::string Select = definition(C, "select_by_ge");
    EXPECT_EQ(Select.find("__asm__"), std::string::npos) << C;
    EXPECT_NE(Select.find("arg1"), std::string::npos) << C;
    EXPECT_NE(Select.find("arg2"), std::string::npos) << C;
    // The machine keeps an SPSR, the mode and the interrupt masks.
    EXPECT_NE(definition(C, "read_spsr").find("\"mrs %0, spsr\""),
              std::string::npos)
        << C;
    EXPECT_NE(definition(C, "write_spsr").find("\"msr spsr_fsxc, %0\""),
              std::string::npos)
        << C;
    EXPECT_NE(definition(C, "irq_restore").find("\"msr cpsr_c, %0\""),
              std::string::npos)
        << C;
    EXPECT_NE(definition(C, "to_svc_thumb").find("\"cps #0x13\""),
              std::string::npos)
        << C;
    // The flags a comparison set replace the machine's.
    const std::string Compare = definition(C, "flags_after_compare");
    EXPECT_NE(Compare.find("\"mrs %0, apsr\""), std::string::npos) << C;
    EXPECT_NE(Compare.find("arg1"), std::string::npos) << C;
    // The flags a function never set are the machine's, as the procedure
    // call standard leaves them on entry: a kernel's local_irq_save reads
    // CPSR whole, then masks IRQs.
    const std::string Save = definition(C, "irq_save");
    const size_t Read = Save.find("\"mrs %0, apsr\"");
    EXPECT_NE(Read, std::string::npos) << C;
    EXPECT_LT(Read, Save.find("\"cpsid i\"")) << C;
    EXPECT_NE(definition(C, "read_apsr").find("\"mrs %0, apsr\""),
              std::string::npos)
        << C;
    EXPECT_EQ(C.find("__builtin_trap"), std::string::npos) << C;
    // CPSID with no mask to change has no typed lift.
    EXPECT_NE(C.find("/* neverd.not-decompiled: cps_without_masks at 0x"),
              std::string::npos)
        << C;
    // The assembler accepts each instruction in ARM and in Thumb.
    if (!hasCrossTargetClang())
      continue;
    const auto File = tmpFile(LLVM ? "status_llvm.c" : "status.c");
    std::ofstream(File) << C;
    for (const char *Set : {"-marm", "-mthumb"}) {
      SCOPED_TRACE(Set);
      const auto Built = exec(
          "clang", {"--target=armv7a-linux-gnueabihf", "-mcpu=cortex-a15", Set,
                    "-ffreestanding", "-c", File.string(), "-o",
                    tmpFile(LLVM ? "status_llvm.o" : "status.o").string()});
      EXPECT_EQ(Built.exitCode, 0) << Built.err << "\n" << C;
    }
  }
}
