//===- LLVMFunctionView.cpp - One function as an LLVM module --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LLVMFunctionView.h"

#include "ImportSlots.h"
#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/loader/StringLiteralSymbols.h"
#include "neverd/sdk/Limits.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugProgramInstruction.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <map>
#include <optional>
#include <set>

using namespace neverd;
using namespace neverd::sdk;

namespace {

#define NEVERD_IMPORT_SLOT_SUFFIX(Suffix)                                      \
  constexpr llvm::StringLiteral ImportSlotSuffix(Suffix);
#include "neverd/DataNames.def"

/// \p Text as a comment can hold it: valid UTF-8 on the comment's own line.
std::string commentText(llvm::StringRef Text) {
  std::string Safe = jsonSafeText(Text);
  for (char &C : Safe)
    if (static_cast<unsigned char>(C) < 0x20)
      C = ' ';
  return Safe;
}

/// The first image byte \p Variable holds, for one of NeverD's image mirrors
/// (`__nd_data_<VA>`, `__nd_codeptr_<VA>`).
std::optional<va_t> mirroredAddress(const llvm::GlobalVariable &Variable) {
  if (auto First = parseNdDataSymbol(Variable.getName()))
    return First;
  return parseNdCodePtrSymbol(Variable.getName());
}

/// The image address \p Global stands for: an image mirror's first byte or a
/// lifted function's entry.
std::optional<va_t> imageAddress(const llvm::GlobalValue &Global) {
  if (const auto *Variable = llvm::dyn_cast<llvm::GlobalVariable>(&Global))
    return mirroredAddress(*Variable);
  const auto *Function = llvm::dyn_cast<llvm::Function>(&Global);
  if (!Function)
    return std::nullopt;
  auto Entry = rewrite_source::getOriginalVA(*Function);
  if (!Entry) {
    llvm::consumeError(Entry.takeError());
    return std::nullopt;
  }
  return *Entry;
}

/// The globals \p F refers to, in the order first met: through its own
/// operands, its instructions and the metadata attached to either, then
/// through the metadata attached to each global met.  A global this misses
/// would stay behind in the original module when the function moves out.
class ReferencedGlobals {
public:
  explicit ReferencedGlobals(const llvm::Function &F) {
    if (F.hasPersonalityFn())
      value(F.getPersonalityFn());
    if (F.hasPrefixData())
      value(F.getPrefixData());
    if (F.hasPrologueData())
      value(F.getPrologueData());
    attachments(F);
    for (const llvm::Instruction &I : llvm::instructions(F)) {
      for (const llvm::Value *Operand : I.operand_values())
        value(Operand);
      attachments(I);
      for (const llvm::DbgRecord &Record : I.getDbgRecordRange())
        record(Record);
    }
    for (size_t I = 0; I < Order.size(); ++I)
      if (const auto *Object = llvm::dyn_cast<llvm::GlobalObject>(Order[I]))
        attachments(*Object);
  }

  llvm::ArrayRef<const llvm::GlobalValue *> globals() const { return Order; }

private:
  void value(const llvm::Value *V) {
    if (!V || !Values.insert(V).second)
      return;
    if (const auto *Global = llvm::dyn_cast<llvm::GlobalValue>(V))
      Order.push_back(Global);
    else if (const auto *Wrapped = llvm::dyn_cast<llvm::MetadataAsValue>(V))
      metadata(Wrapped->getMetadata());
    else if (const auto *C = llvm::dyn_cast<llvm::Constant>(V))
      for (const llvm::Value *Operand : C->operand_values())
        value(Operand);
  }

  void metadata(const llvm::Metadata *MD) {
    if (!MD || !Nodes.insert(MD).second)
      return;
    if (const auto *Wrapped = llvm::dyn_cast<llvm::ValueAsMetadata>(MD))
      value(Wrapped->getValue());
    else if (const auto *Node = llvm::dyn_cast<llvm::MDNode>(MD))
      for (const llvm::MDOperand &Operand : Node->operands())
        metadata(Operand.get());
    else if (const auto *Arguments = llvm::dyn_cast<llvm::DIArgList>(MD))
      for (const llvm::ValueAsMetadata *Argument : Arguments->getArgs())
        metadata(Argument);
  }

  template <typename Owner> void attachments(const Owner &Object) {
    llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> Attached;
    Object.getAllMetadata(Attached);
    for (const auto &Entry : Attached)
      metadata(Entry.second);
  }

  void record(const llvm::DbgRecord &Record) {
    metadata(Record.getDebugLoc().getAsMDNode());
    if (const auto *Label = llvm::dyn_cast<llvm::DbgLabelRecord>(&Record)) {
      metadata(Label->getRawLabel());
      return;
    }
    const auto &Variable = llvm::cast<llvm::DbgVariableRecord>(Record);
    metadata(Variable.getRawLocation());
    metadata(Variable.getRawVariable());
    metadata(Variable.getRawExpression());
    if (Variable.isDbgAssign()) {
      metadata(Variable.getRawAddress());
      metadata(Variable.getRawAssignID());
      metadata(Variable.getRawAddressExpression());
    }
  }

  llvm::SmallPtrSet<const llvm::Value *, 32> Values;
  llvm::SmallPtrSet<const llvm::Metadata *, 32> Nodes;
  std::vector<const llvm::GlobalValue *> Order;
};

/// \p Global as a module that refers to it without defining it declares it.
llvm::GlobalValue *declareIn(llvm::Module &Slice,
                             const llvm::GlobalValue &Global) {
  const auto Linkage = Global.hasExternalWeakLinkage()
                           ? llvm::GlobalValue::ExternalWeakLinkage
                           : llvm::GlobalValue::ExternalLinkage;
  llvm::GlobalValue *Declared = nullptr;
  if (const auto *Function = llvm::dyn_cast<llvm::Function>(&Global)) {
    auto *Copy = llvm::Function::Create(Function->getFunctionType(), Linkage,
                                        Function->getAddressSpace(),
                                        Function->getName(), &Slice);
    Copy->copyAttributesFrom(Function);
    // What only a definition carries.
    Copy->setPersonalityFn(nullptr);
    Copy->setPrefixData(nullptr);
    Copy->setPrologueData(nullptr);
    Declared = Copy;
  } else if (const auto *Variable =
                 llvm::dyn_cast<llvm::GlobalVariable>(&Global)) {
    auto *Copy = new llvm::GlobalVariable(
        Slice, Variable->getValueType(), Variable->isConstant(), Linkage,
        /*Initializer=*/nullptr, Variable->getName(), /*InsertBefore=*/nullptr,
        Variable->getThreadLocalMode(), Variable->getAddressSpace());
    Copy->copyAttributesFrom(Variable);
    Declared = Copy;
  } else if (auto *Type =
                 llvm::dyn_cast<llvm::FunctionType>(Global.getValueType())) {
    // An alias or an ifunc: what it stands for stays behind.
    Declared = llvm::Function::Create(Type, Linkage, Global.getAddressSpace(),
                                      Global.getName(), &Slice);
  } else {
    Declared = new llvm::GlobalVariable(
        Slice, Global.getValueType(), /*isConstant=*/false, Linkage,
        /*Initializer=*/nullptr, Global.getName(), /*InsertBefore=*/nullptr,
        Global.getThreadLocalMode(), Global.getAddressSpace());
  }
  // A declaration exports nothing.
  if (Declared->hasDLLExportStorageClass())
    Declared->setDLLStorageClass(llvm::GlobalValue::DefaultStorageClass);
  return Declared;
}

/// \p F moved into a module of its own, as `llvm-extract` moves it: \p F
/// defined, every global it refers to declared, and the original module's
/// flags.  The original module's other named metadata stays behind.
std::unique_ptr<llvm::Module> sliceFunction(const llvm::Function &F) {
  const llvm::Module &Original = *F.getParent();
  auto Slice = std::make_unique<llvm::Module>(Original.getModuleIdentifier(),
                                              Original.getContext());
  Slice->setSourceFileName(Original.getSourceFileName());
  Slice->setDataLayout(Original.getDataLayout());
  Slice->setTargetTriple(Original.getTargetTriple());
  Slice->setModuleInlineAsm(Original.getModuleInlineAsm());
  llvm::ValueToValueMapTy Map;
  const ReferencedGlobals Referenced(F);
  for (const llvm::GlobalValue *Global : Referenced.globals())
    if (Global != &F)
      Map[Global] = declareIn(*Slice, *Global);
  // The declarations' own metadata, once every global it names is declared.
  // A debug attachment stays behind: it would need its compile unit.
  for (const llvm::GlobalValue *Global : Referenced.globals()) {
    const auto *Object = llvm::dyn_cast<llvm::GlobalObject>(Global);
    if (!Object || Global == &F)
      continue;
    auto *Declared =
        llvm::cast<llvm::GlobalObject>(static_cast<llvm::Value *>(Map[Global]));
    llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> Attached;
    Object->getAllMetadata(Attached);
    for (const auto &[Kind, Node] : Attached)
      if (Kind != llvm::LLVMContext::MD_dbg)
        Declared->addMetadata(Kind, *llvm::MapMetadata(Node, Map));
  }
  auto *Copy =
      llvm::Function::Create(F.getFunctionType(), F.getLinkage(),
                             F.getAddressSpace(), F.getName(), Slice.get());
  Map[&F] = Copy;
  auto Argument = Copy->arg_begin();
  for (const llvm::Argument &Parameter : F.args()) {
    Argument->setName(Parameter.getName());
    Map[&Parameter] = &*Argument++;
  }
  llvm::SmallVector<llvm::ReturnInst *, 8> Returns;
  llvm::CloneFunctionInto(
      Copy, &F, Map, llvm::CloneFunctionChangeType::DifferentModule, Returns);
  if (const llvm::Comdat *Group = F.getComdat()) {
    llvm::Comdat *Copied = Slice->getOrInsertComdat(Group->getName());
    Copied->setSelectionKind(Group->getSelectionKind());
    Copy->setComdat(Copied);
  }
  // Cloning lists the compile units the function brings, which code without
  // debug information has none of.
  if (llvm::NamedMDNode *Units = Slice->getNamedMetadata("llvm.dbg.cu");
      Units && Units->getNumOperands() == 0)
    Slice->eraseNamedMetadata(Units);
  if (const llvm::NamedMDNode *Flags = Original.getModuleFlagsMetadata()) {
    llvm::NamedMDNode *Copied = Slice->getOrInsertModuleFlagsMetadata();
    for (const llvm::MDNode *Flag : Flags->operands())
      Copied->addOperand(
          llvm::MapMetadata(Flag, Map, llvm::RF_NullMapMissingGlobalValues));
  }
  return Slice;
}

/// A global an instruction names, with the image address it reaches there.
struct Occurrence {
  const llvm::GlobalValue *Global = nullptr;
  std::optional<va_t> Address;
  /// Whether \p Global is an image mirror, whose address is data.
  bool Mirror = false;
};

/// The image mirror and address \p C reaches by a constant offset from the
/// mirror's first byte, through casts.
std::optional<std::pair<const llvm::GlobalVariable *, va_t>>
mirrorOffset(const llvm::Constant &C, const llvm::DataLayout &Layout) {
  if (const auto *Mirror = llvm::dyn_cast<llvm::GlobalVariable>(&C)) {
    const auto First = mirroredAddress(*Mirror);
    if (!First)
      return std::nullopt;
    return std::pair{Mirror, *First};
  }
  const auto *Expression = llvm::dyn_cast<llvm::ConstantExpr>(&C);
  if (!Expression)
    return std::nullopt;
  if (const auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(Expression)) {
    const auto Base = mirrorOffset(
        *llvm::cast<llvm::Constant>(GEP->getPointerOperand()), Layout);
    llvm::APInt Offset(Layout.getIndexSizeInBits(GEP->getPointerAddressSpace()),
                       0);
    if (!Base || !GEP->accumulateConstantOffset(Layout, Offset))
      return std::nullopt;
    return std::pair{Base->first,
                     Base->second + static_cast<va_t>(Offset.getSExtValue())};
  }
  switch (Expression->getOpcode()) {
  case llvm::Instruction::BitCast:
  case llvm::Instruction::AddrSpaceCast:
  case llvm::Instruction::IntToPtr:
  case llvm::Instruction::PtrToInt:
    return mirrorOffset(*Expression->getOperand(0), Layout);
  case llvm::Instruction::Add:
  case llvm::Instruction::Sub: {
    const auto *Delta =
        llvm::dyn_cast<llvm::ConstantInt>(Expression->getOperand(1));
    if (!Delta || Delta->getBitWidth() > 64)
      return std::nullopt;
    const auto Base = mirrorOffset(*Expression->getOperand(0), Layout);
    if (!Base)
      return std::nullopt;
    const auto Step = static_cast<va_t>(Delta->getSExtValue());
    return std::pair{Base->first,
                     Expression->getOpcode() == llvm::Instruction::Add
                         ? Base->second + Step
                         : Base->second - Step};
  }
  default:
    return std::nullopt;
  }
}

/// Append the globals \p V names to \p Named, in the order the IR printer
/// spells them.
void collectNamed(const llvm::Value *V, const llvm::DataLayout &Layout,
                  std::vector<Occurrence> &Named) {
  if (const auto *Wrapped = llvm::dyn_cast<llvm::MetadataAsValue>(V)) {
    if (const auto *Local =
            llvm::dyn_cast<llvm::ValueAsMetadata>(Wrapped->getMetadata()))
      collectNamed(Local->getValue(), Layout, Named);
    else if (const auto *Arguments =
                 llvm::dyn_cast<llvm::DIArgList>(Wrapped->getMetadata()))
      for (const llvm::ValueAsMetadata *Argument : Arguments->getArgs())
        collectNamed(Argument->getValue(), Layout, Named);
    return;
  }
  const auto *C = llvm::dyn_cast<llvm::Constant>(V);
  if (!C)
    return;
  if (const auto Reached = mirrorOffset(*C, Layout)) {
    Named.push_back({Reached->first, Reached->second, /*Mirror=*/true});
    return;
  }
  if (const auto *Global = llvm::dyn_cast<llvm::GlobalValue>(C)) {
    Named.push_back({Global, imageAddress(*Global), /*Mirror=*/false});
    return;
  }
  for (const llvm::Value *Operand : C->operand_values())
    collectNamed(Operand, Layout, Named);
}

/// The globals \p I names, in the order the IR printer spells them: a call
/// spells its callee before its arguments and operand bundles, every other
/// instruction its operands in order.
std::vector<Occurrence> namedGlobals(const llvm::Instruction &I,
                                     const llvm::DataLayout &Layout) {
  std::vector<Occurrence> Named;
  if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I)) {
    collectNamed(Call->getCalledOperand(), Layout, Named);
    for (const llvm::Use &Argument : Call->args())
      collectNamed(Argument.get(), Layout, Named);
    for (unsigned Bundle = 0; Bundle < Call->getNumOperandBundles(); ++Bundle)
      for (const llvm::Use &Input : Call->getOperandBundleAt(Bundle).Inputs)
        collectNamed(Input.get(), Layout, Named);
    return Named;
  }
  for (const llvm::Value *Operand : I.operand_values())
    collectNamed(Operand, Layout, Named);
  return Named;
}

/// Whether \p C continues an unquoted LLVM name.
bool continuesName(char C) {
  return llvm::isAlnum(C) || C == '-' || C == '$' || C == '.' || C == '_';
}

/// The global names a line of IR spells outside strings and its comment, in
/// order: `@name` or `@"name"`.
llvm::SmallVector<llvm::StringRef, 4> spelledGlobals(llvm::StringRef Line) {
  llvm::SmallVector<llvm::StringRef, 4> Names;
  for (size_t I = 0; I < Line.size();) {
    const char C = Line[I];
    if (C == ';')
      break;
    // LLVM escapes a quote in a string as `\22`: the next quote ends it.
    if (C == '"') {
      const size_t Close = Line.find('"', I + 1);
      if (Close == llvm::StringRef::npos)
        break;
      I = Close + 1;
      continue;
    }
    if (C != '@') {
      ++I;
      continue;
    }
    size_t End = I + 1;
    if (End < Line.size() && Line[End] == '"') {
      const size_t Close = Line.find('"', End + 1);
      if (Close == llvm::StringRef::npos)
        break;
      End = Close + 1;
    } else {
      while (End < Line.size() && continuesName(Line[End]))
        ++End;
    }
    if (End > I + 1)
      Names.push_back(Line.slice(I, End));
    I = End;
  }
  return Names;
}

/// The name a spelled global (`@name`, `@"a\22b"`) has in its module.
std::string unspelledName(llvm::StringRef Spelled) {
  llvm::StringRef Name = Spelled.drop_front();
  if (!Name.consume_front("\"") || !Name.consume_back("\""))
    return Name.str();
  std::string Unescaped;
  for (size_t I = 0; I < Name.size(); ++I) {
    if (Name[I] == '\\' && I + 2 < Name.size() &&
        llvm::isHexDigit(Name[I + 1]) && llvm::isHexDigit(Name[I + 2])) {
      Unescaped +=
          static_cast<char>(llvm::hexFromNibbles(Name[I + 1], Name[I + 2]));
      I += 2;
      continue;
    }
    Unescaped += Name[I];
  }
  return Unescaped;
}

/// An instruction's text and the globals it names.
struct InstructionText {
  const llvm::Instruction *Instruction = nullptr;
  size_t Begin = 0;
  size_t End = 0;
  std::vector<Occurrence> Named;
};

/// Comments that give the image behind what the IR names, and where the
/// shown function and each instruction fall in the text.
class ImageNotes final : public llvm::AssemblyAnnotationWriter {
public:
  ImageNotes(const llvm::Function &Shown, LLVMViewAddressNote DataNote,
             LLVMViewAddressNote FunctionNote)
      : Shown(Shown), DataNote(DataNote), FunctionNote(FunctionNote),
        Layout(Shown.getDataLayout()) {}

  /// `; Address: 0x117F check`: the function's entry, and the name the
  /// listing gives it where the IR names it otherwise.
  void emitFunctionAnnot(const llvm::Function *F,
                         llvm::formatted_raw_ostream &OS) override {
    if (F == &Shown)
      DefinitionBegin = OS.tell();
    const auto Entry = imageAddress(*F);
    if (!Entry)
      return;
    OS << "; Address: 0x" << llvm::utohexstr(*Entry);
    if (const std::string Name = FunctionNote(*Entry);
        !Name.empty() && Name != F->getName())
      OS << ' ' << Name;
    OS << '\n';
  }

  void emitInstructionAnnot(const llvm::Instruction *I,
                            llvm::formatted_raw_ostream &OS) override {
    Instructions.push_back({I, OS.tell(), OS.tell(), namedGlobals(*I, Layout)});
  }

  void printInfoComment(const llvm::Value &V,
                        llvm::formatted_raw_ostream &OS) override {
    if (!Instructions.empty() && Instructions.back().Instruction == &V) {
      InstructionText &Text = Instructions.back();
      Text.End = OS.tell();
      noteReachedData(Text.Named, OS);
    } else if (const auto *Variable = llvm::dyn_cast<llvm::GlobalVariable>(&V);
               Variable && mirroredAddress(*Variable)) {
      Mirrors.emplace_back(Variable, OS.tell());
    }
  }

  size_t definitionBegin() const { return DefinitionBegin; }
  llvm::ArrayRef<InstructionText> instructions() const { return Instructions; }
  /// Each image mirror declared, with where its declaration's line ends.
  llvm::ArrayRef<std::pair<const llvm::GlobalVariable *, size_t>>
  mirrors() const {
    return Mirrors;
  }

private:
  /// ` ; 0x3FD8 __libc_start_main_ptr`: each image address the instruction
  /// reaches through a mirror, once, with what is there.
  void noteReachedData(llvm::ArrayRef<Occurrence> Named,
                       llvm::formatted_raw_ostream &OS) {
    llvm::SmallVector<va_t, 4> Reached;
    for (const Occurrence &Each : Named)
      if (Each.Mirror && Each.Address &&
          !llvm::is_contained(Reached, *Each.Address))
        Reached.push_back(*Each.Address);
    for (size_t I = 0; I < Reached.size(); ++I) {
      OS << (I ? ", " : " ; ") << "0x" << llvm::utohexstr(Reached[I]);
      if (const std::string Text = DataNote(Reached[I]); !Text.empty())
        OS << ' ' << Text;
    }
  }

  const llvm::Function &Shown;
  LLVMViewAddressNote DataNote;
  LLVMViewAddressNote FunctionNote;
  const llvm::DataLayout &Layout;
  size_t DefinitionBegin = 0;
  std::vector<InstructionText> Instructions;
  std::vector<std::pair<const llvm::GlobalVariable *, size_t>> Mirrors;
};

/// The image addresses \p F reaches through each image mirror, in order.
std::map<const llvm::GlobalVariable *, std::vector<va_t>>
mirrorReaches(const llvm::Function &F) {
  std::map<const llvm::GlobalVariable *, std::set<va_t>> Found;
  for (const llvm::Instruction &I : llvm::instructions(F))
    for (const Occurrence &Named : namedGlobals(I, F.getDataLayout()))
      if (Named.Mirror && Named.Address)
        Found[llvm::cast<llvm::GlobalVariable>(Named.Global)].insert(
            *Named.Address);
  std::map<const llvm::GlobalVariable *, std::vector<va_t>> Reaches;
  for (const auto &[Mirror, Addresses] : Found)
    Reaches[Mirror].assign(Addresses.begin(), Addresses.end());
  return Reaches;
}

/// \p Text broken at its spaces into lines of at most \p Width characters;
/// a longer word takes a line of its own.
std::vector<std::string> wrapWords(llvm::StringRef Text, size_t Width) {
  std::vector<std::string> Lines;
  std::string Line;
  for (llvm::StringRef Rest = Text; !Rest.empty();) {
    auto [Word, Next] = Rest.split(' ');
    Rest = Next;
    if (Word.empty())
      continue;
    if (!Line.empty() && Line.size() + 1 + Word.size() > Width)
      Lines.push_back(std::exchange(Line, std::string()));
    if (!Line.empty())
      Line += ' ';
    Line += Word;
  }
  if (!Line.empty())
    Lines.push_back(std::move(Line));
  return Lines;
}

/// The comment over an image mirror's declaration: the image bytes it stands
/// for and the sections they lie in, how an offset into it reads, and each
/// address in \p Reached with what \p DataNote says is there:
///
///   ; NeverD's mirror of the 624 image bytes at 0x3DB0 (.init_array, ...),
///   ; which hold pointers the loader relocates. An offset into it is an
///   ; image address, 0x3DB0 + offset. This function uses:
///   ;   +552 = 0x3FD8 __libc_start_main_ptr
///
/// It does not spell the mirror's name, which a reader finds below it.
std::string mirrorComment(const llvm::GlobalVariable &Mirror,
                          const BinaryImage &Image,
                          llvm::ArrayRef<va_t> Reached,
                          LLVMViewAddressNote DataNote) {
  const va_t First = *mirroredAddress(Mirror);
  llvm::Type *Type = Mirror.getValueType();
  const uint64_t Size =
      Type->isSized()
          ? Mirror.getDataLayout().getTypeAllocSize(Type).getFixedValue()
          : 0;
  std::string Sentence;
  llvm::raw_string_ostream OS(Sentence);
  OS << "NeverD's mirror of the ";
  if (Size)
    OS << Size << (Size == 1 ? " image byte" : " image bytes");
  else
    OS << "image bytes";
  OS << " at 0x" << llvm::utohexstr(First);
  std::vector<const Section *> Spanned;
  for (const Section &Sec : Image.Sections)
    if (Size && Sec.isReadable() && !Sec.isDebugInfo() && Sec.Size &&
        Sec.VA < First + Size && First < Sec.VA + Sec.Size)
      Spanned.push_back(&Sec);
  llvm::sort(Spanned, [](const Section *Left, const Section *Right) {
    return Left->VA < Right->VA;
  });
  for (size_t I = 0; I < Spanned.size() && I < kMaxLLVMCommentSections; ++I)
    OS << (I ? ", " : " (") << commentText(Spanned[I]->Name);
  if (Spanned.size() > kMaxLLVMCommentSections)
    OS << ", ...";
  if (!Spanned.empty())
    OS << ")";
  if (parseNdCodePtrSymbol(Mirror.getName()))
    OS << ", which hold pointers the loader relocates";
  OS << ". An offset into it is an image address, 0x" << llvm::utohexstr(First)
     << " + offset.";
  if (!Reached.empty())
    OS << " This function uses:";
  std::string Block;
  for (const std::string &Line : wrapWords(Sentence, kLLVMCommentWidth))
    Block += "; " + Line + "\n";
  for (size_t I = 0; I < Reached.size() && I < kMaxLLVMCommentReaches; ++I) {
    const auto Offset = static_cast<int64_t>(Reached[I] - First);
    Block += ";   " + std::string(Offset < 0 ? "-" : "+") +
             std::to_string(Offset < 0 ? -static_cast<uint64_t>(Offset)
                                       : static_cast<uint64_t>(Offset)) +
             " = 0x" + llvm::utohexstr(Reached[I]);
    if (const std::string Note = DataNote(Reached[I]); !Note.empty())
      Block += " " + Note;
    Block += "\n";
  }
  if (Reached.size() > kMaxLLVMCommentReaches)
    Block += ";   ... and " +
             std::to_string(Reached.size() - kMaxLLVMCommentReaches) +
             " more\n";
  return Block;
}

/// The image address each global name on each line of \p Text stands for.
/// An instruction's name of a mirror stands for the byte its constant offset
/// reaches there; a mirror the instruction reaches at several offsets maps
/// its names to them in order when it spells as many, else to its first
/// byte.  Elsewhere a name stands for its global's own address.
std::vector<LLVMViewReference>
referencesIn(llvm::StringRef Text, llvm::ArrayRef<InstructionText> Instructions,
             const llvm::Module &Names) {
  std::vector<size_t> Starts{0};
  for (size_t I = 0; I + 1 < Text.size(); ++I)
    if (Text[I] == '\n')
      Starts.push_back(I + 1);
  std::map<size_t, const InstructionText *> ByLine;
  for (const InstructionText &Instruction : Instructions)
    ByLine[static_cast<size_t>(
        std::upper_bound(Starts.begin(), Starts.end(), Instruction.Begin) -
        Starts.begin() - 1)] = &Instruction;
  std::vector<LLVMViewReference> References;
  for (size_t Line = 0; Line < Starts.size(); ++Line) {
    const auto Found = ByLine.find(Line);
    const InstructionText *Instruction =
        Found == ByLine.end() ? nullptr : Found->second;
    const size_t Begin = Instruction ? Instruction->Begin : Starts[Line];
    const size_t End =
        Instruction ? Instruction->End
                    : std::min(Text.size(), Text.find('\n', Starts[Line]));
    const auto Spelled = spelledGlobals(Text.slice(Begin, End));
    if (Spelled.empty())
      continue;
    std::vector<const llvm::GlobalValue *> Globals;
    for (llvm::StringRef Name : Spelled)
      Globals.push_back(Names.getNamedValue(unspelledName(Name)));
    std::vector<std::optional<va_t>> Addresses(Spelled.size());
    for (size_t I = 0; I < Spelled.size(); ++I) {
      if (!Globals[I] || Addresses[I])
        continue;
      if (!Instruction) {
        Addresses[I] = imageAddress(*Globals[I]);
        continue;
      }
      llvm::SmallVector<size_t, 2> Spellings;
      for (size_t J = I; J < Spelled.size(); ++J)
        if (Globals[J] == Globals[I])
          Spellings.push_back(J);
      llvm::SmallVector<std::optional<va_t>, 2> Reached;
      for (const Occurrence &Named : Instruction->Named)
        if (Named.Global == Globals[I])
          Reached.push_back(Named.Address);
      for (size_t K = 0; K < Spellings.size(); ++K)
        Addresses[Spellings[K]] = Reached.size() == Spellings.size()
                                      ? Reached[K]
                                  : !Reached.empty() && llvm::all_equal(Reached)
                                      ? Reached.front()
                                      : imageAddress(*Globals[I]);
    }
    for (size_t I = 0; I < Spelled.size(); ++I)
      if (Addresses[I])
        References.push_back({Line, Spelled[I].str(), *Addresses[I]});
  }
  return References;
}

/// What a comment says of the image data at \p Address: the name the listing
/// gives it, then the string it holds.
std::string addressNote(Session &S,
                        std::optional<std::map<va_t, std::string>> &Slots,
                        va_t Address) {
  std::string Name;
  if (const auto Renamed = S.Renames.find(Address); Renamed != S.Renames.end())
    Name = Renamed->second;
  if (Name.empty()) {
    if (!Slots) {
      Slots.emplace();
      for (const Import &Imported : S.Img.Imports)
        if (Imported.IATAddr && !Imported.Name.empty())
          Slots->try_emplace(Imported.IATAddr, Imported.Name);
      for (const auto &[Slot, Stored] : importAddressSlots(S.Img))
        Slots->try_emplace(Slot, Stored.Name);
    }
    if (const auto Slot = Slots->find(Address); Slot != Slots->end())
      Name = S.Img.Format == BinaryFormat::ELF
                 ? (Slot->second + ImportSlotSuffix).str()
                 : Slot->second;
  }
  if (Name.empty() && S.Dbg)
    if (const auto Data = S.Dbg->resolveDataObject(Address);
        Data && !Data->Name.empty() && !isStringLiteralName(Data->Name))
      Name = Data->Name;
  if (Name.empty())
    if (const Symbol *Sym = S.Img.findSymbolAt(Address);
        Sym && !Sym->IsFunc && !Sym->Name.empty() &&
        !llvm::StringRef(Sym->Name).starts_with(kAutoFuncPrefix) &&
        !isStringLiteralName(Sym->Name))
      Name = Sym->Name;
  // The text as a reference's comment reads it, long text cut; a string too
  // short for that comment reads as its literal.
  std::optional<std::string> String = imageStringComment(&S.Img, Address);
  if (!String)
    if (const auto Short = imageCString(&S.Img, Address))
      String = Short->Note.empty() ? Short->Literal : Short->Note;
  std::string Text = std::move(Name);
  if (String)
    Text += (Text.empty() ? "" : " ") + *String;
  return commentText(Text);
}

/// The name the session's function list gives the function at \p Entry,
/// unless it is only the automatic name of its address (`sub_117F`).
std::string functionNote(const Session &S, va_t Entry) {
  for (const FuncInfo &Function : S.Functions) {
    if (Function.Entry != Entry)
      continue;
    llvm::StringRef Name = Function.Name;
    va_t Named = 0;
    if (Name.consume_front(kAutoFuncPrefix) && !Name.getAsInteger(16, Named) &&
        Named == Entry)
      return {};
    return commentText(Function.Name);
  }
  return {};
}

} // namespace

LLVMFunctionView neverd::sdk::renderLLVMFunctionView(
    const llvm::Function &F, const BinaryImage &Image,
    LLVMViewAddressNote DataNote, LLVMViewAddressNote FunctionNote) {
  LLVMFunctionView View;
  llvm::raw_string_ostream OS(View.Text);
  std::unique_ptr<llvm::Module> Slice = sliceFunction(F);
  std::string Problem;
  llvm::raw_string_ostream Verifier(Problem);
  const llvm::Function *Shown = Slice->getFunction(F.getName());
  if (Shown && !llvm::verifyModule(*Slice, &Verifier)) {
    const auto Reaches = mirrorReaches(*Shown);
    ImageNotes Notes(*Shown, DataNote, FunctionNote);
    Slice->print(OS, &Notes);
    OS.flush();
    View.DefinitionBegin = Notes.definitionBegin();
    std::vector<InstructionText> Instructions(Notes.instructions().begin(),
                                              Notes.instructions().end());
    // Each mirror's declaration goes under a comment that says what it stands
    // for.  The printer offers no annotation before a global, so the comments
    // go in now, the last first, moving what follows them.
    for (const auto &[Mirror, LineEnd] : llvm::reverse(Notes.mirrors())) {
      const size_t Newline =
          LineEnd ? View.Text.rfind('\n', LineEnd - 1) : std::string::npos;
      const size_t At = Newline == std::string::npos ? 0 : Newline + 1;
      const auto Found = Reaches.find(Mirror);
      const std::string Comment = mirrorComment(
          *Mirror, Image,
          Found == Reaches.end() ? llvm::ArrayRef<va_t>()
                                 : llvm::ArrayRef<va_t>(Found->second),
          DataNote);
      View.Text.insert(At, Comment);
      const auto Move = [&](size_t &Offset) {
        if (Offset >= At)
          Offset += Comment.size();
      };
      Move(View.DefinitionBegin);
      for (InstructionText &Text : Instructions) {
        Move(Text.Begin);
        Move(Text.End);
      }
    }
    const size_t Closed = View.Text.find("\n}\n", View.DefinitionBegin);
    View.DefinitionEnd =
        Closed == std::string::npos ? View.Text.size() : Closed + 3;
    View.References = referencesIn(View.Text, Instructions, *Slice);
    return View;
  }
  // Show the function alone, and say why.
  llvm::StringRef Reason = llvm::StringRef(Problem).trim();
  Reason = Reason.take_front(Reason.find('\n'));
  OS << "; NeverD shows this function without its module: "
     << (Reason.empty() ? llvm::StringRef("the function did not move") : Reason)
     << '\n';
  ImageNotes Notes(F, DataNote, FunctionNote);
  F.print(OS, &Notes);
  OS.flush();
  View.DefinitionEnd = View.Text.size();
  View.References =
      referencesIn(View.Text, Notes.instructions(), *F.getParent());
  return View;
}

const LLVMFunctionView *neverd::sdk::functionLlvmView(Session &S, va_t Entry) {
  // Inspection shows the lifted control flow, independently of LLVMC's
  // source optimization profile. Promote temporary slots to SSA only.
  const auto *Native = S.ensureFunctionLlvmModule(Entry, /*NoOpt=*/true);
  if (!Native) {
    if (S.LastError.empty())
      S.setError("failed to generate LLVM module");
    return nullptr;
  }
  if (Native->View)
    return Native->View.get();
  const llvm::Function *F = S.findNativeLlvmFunction(*Native->Module, Entry);
  if (!F)
    return nullptr;
  std::optional<std::map<va_t, std::string>> Slots;
  const bool Listed = S.synchronizeFunctions();
  Native->View =
      std::make_shared<const LLVMFunctionView>(renderLLVMFunctionView(
          *F, S.Img,
          [&](va_t Address) { return addressNote(S, Slots, Address); },
          [&](va_t Entry) {
            return Listed ? functionNote(S, Entry) : std::string();
          }));
  return Native->View.get();
}
