//===- LLVMPrivateFrameProjectionTest.h - Frame test helpers ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_LLVMPRIVATEFRAMEPROJECTIONTEST_H
#define NEVERD_UNITTESTS_LLVMPRIVATEFRAMEPROJECTIONTEST_H

#include "gtest/gtest.h"

#include "neverd/analysis/LLVMPrivateFrameProjection.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"

namespace neverd::analysis::frame_test {
inline std::string print(const llvm::Module &M) {
  std::string Text;
  llvm::raw_string_ostream Out(Text);
  M.print(Out, nullptr);
  return Text;
}

inline void replace(std::string &Text, llvm::StringRef From,
                    llvm::StringRef To) {
  auto At = Text.find(From.str());
  ASSERT_NE(At, std::string::npos) << From.str();
  Text.replace(At, From.size(), To.str());
}

inline std::unique_ptr<llvm::Module> parse(llvm::LLVMContext &C,
                                           const std::string &Text,
                                           unsigned Width = 64,
                                           bool BigEndian = false) {
  llvm::SMDiagnostic Error;
  auto M = llvm::parseAssemblyString(Text, Error, C);
  EXPECT_TRUE(M) << Error.getMessage().str();
  if (!M)
    return nullptr;
  M->setDataLayout(std::string(BigEndian ? "E" : "e") +
                   "-p:" + std::to_string(Width) + ":" + std::to_string(Width));
  EXPECT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

inline LLVMPrivateFrameContract contract(llvm::Function &F) {
  return {F.getArg(0), -64, 0, {{F.getArg(3), 48}, {F.getArg(4), 4}}};
}

// Independently authored byte overlap, two entry arms and two loop backedges.
// Both external pointers can alias; a frame address also remains visible data.
inline std::string program(unsigned Width = 64) {
  std::string IR = R"(
define WORD @f(WORD %root, i32 %x, i32 %n, ptr %out, ptr %alias) {
entry:
  %first = sub WORD %root, 48
  %p = inttoptr WORD %first to ptr
  %wide = zext i32 %x to i64
  %seed = xor i64 %wide, 619083122760
  store i64 %seed, ptr %p, align 1
  %middle = add WORD -46, %root
  %q = inttoptr WORD %middle to ptr
  %half = trunc i32 %n to i16
  store i16 %half, ptr %q, align 1
  %last = add WORD %root, -20
  %r = inttoptr WORD %last to ptr
  %bit = and i32 %n, 1
  %choose = icmp eq i32 %bit, 0
  br i1 %choose, label %left, label %right
left:
  %plus = add i32 %x, 23
  store i32 %plus, ptr %r, align 1
  br label %loop
right:
  %flip = xor i32 %x, 39
  store i32 %flip, ptr %r, align 1
  br label %loop
loop:
  %i = phi i32 [ 0, %left ], [ 0, %right ], [ %next, %even ], [ %next, %odd ]
  %bound = and i32 %n, 7
  %more = icmp ult i32 %i, %bound
  br i1 %more, label %body, label %done
body:
  %old = load i32, ptr %r, align 1
  %next = add i32 %i, 1
  %parity = and i32 %i, 1
  %isEven = icmp eq i32 %parity, 0
  br i1 %isEven, label %even, label %odd
even:
  %sum = add i32 %old, %x
  store i32 %sum, ptr %r, align 1
  br label %loop
odd:
  %mixed = xor i32 %old, %i
  store i32 %mixed, ptr %r, align 1
  br label %loop
done:
  %packed = load i64, ptr %p, align 1
  %value = load i32, ptr %r, align 1
  store WORD %root, ptr %out, align 1
  %other = load i32, ptr %alias, align 1
  %combined = xor i32 %value, %other
  %out12 = getelementptr i8, ptr %out, WORD 12
  store i32 %combined, ptr %out12, align 1
  %outNumber = ptrtoint ptr %out to WORD
  %out24 = add WORD %outNumber, 24
  %outPointer = inttoptr WORD %out24 to ptr
  store i64 %packed, ptr %outPointer, align 1
  %extend = zext i32 %combined to i64
  %both = xor i64 %packed, %extend
  RESULT
  ret WORD %answer
}
)";
  replace(IR, "RESULT",
          Width == 64 ? "%answer = xor i64 %both, %root"
                      : "%low = trunc i64 %both to i32\n"
                        "  %answer = xor i32 %low, %root");
  while (IR.find("WORD") != std::string::npos)
    replace(IR, "WORD", "i" + std::to_string(Width));
  return IR;
}
} // namespace neverd::analysis::frame_test
#endif
