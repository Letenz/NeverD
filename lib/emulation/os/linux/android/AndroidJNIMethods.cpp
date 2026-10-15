//===- AndroidJNIMethods.cpp - Bounded Java intrinsic execution -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// JNI method identities and value-based Java operations. Unknown method
/// bodies remain unsupported; resolving a name never supplies a canned result.
//===----------------------------------------------------------------------===//
#include "AndroidArguments.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation::android_model {
namespace {
enum Operation {
  GetClass,
  IdentityEquals,
  ClassName,
  IsArray,
  Length,
  Empty,
  CharAt,
  Hash,
  Equals,
  ToString,
  Concat,
  Substring,
  Compare,
  StartsWith,
  EndsWith,
  IndexOf,
  GetChars,
  ToChars,
  Construct,
  ValueOf
};
struct Method {
  const char *Class, *Name, *Signature;
  Operation Op;
  bool Static = false;
};
// These classes have no observable class initializer in this profile.
constexpr Method Methods[] = {
    {"java/lang/Object", "getClass", "()Ljava/lang/Class;", GetClass},
    {"java/lang/Object", "equals", "(Ljava/lang/Object;)Z", IdentityEquals},
    {"java/lang/Class", "getName", "()Ljava/lang/String;", ClassName},
    {"java/lang/Class", "isArray", "()Z", IsArray},
    {"java/lang/String", "length", "()I", Length},
    {"java/lang/String", "isEmpty", "()Z", Empty},
    {"java/lang/String", "charAt", "(I)C", CharAt},
    {"java/lang/String", "hashCode", "()I", Hash},
    {"java/lang/String", "equals", "(Ljava/lang/Object;)Z", Equals},
    {"java/lang/String", "toString", "()Ljava/lang/String;", ToString},
    {"java/lang/String", "concat", "(Ljava/lang/String;)Ljava/lang/String;",
     Concat},
    {"java/lang/String", "substring", "(I)Ljava/lang/String;", Substring},
    {"java/lang/String", "substring", "(II)Ljava/lang/String;", Substring},
    {"java/lang/String", "compareTo", "(Ljava/lang/String;)I", Compare},
    {"java/lang/String", "startsWith", "(Ljava/lang/String;)Z", StartsWith},
    {"java/lang/String", "startsWith", "(Ljava/lang/String;I)Z", StartsWith},
    {"java/lang/String", "endsWith", "(Ljava/lang/String;)Z", EndsWith},
    {"java/lang/String", "indexOf", "(Ljava/lang/String;)I", IndexOf},
    {"java/lang/String", "indexOf", "(Ljava/lang/String;I)I", IndexOf},
    {"java/lang/String", "getChars", "(II[CI)V", GetChars},
    {"java/lang/String", "toCharArray", "()[C", ToChars},
    {"java/lang/String", "<init>", "()V", Construct},
    {"java/lang/String", "<init>", "(Ljava/lang/String;)V", Construct},
    {"java/lang/String", "<init>", "([C)V", Construct},
    {"java/lang/String", "<init>", "([CII)V", Construct},
    {"java/lang/String", "valueOf", "(I)Ljava/lang/String;", ValueOf, true},
    {"java/lang/String", "valueOf", "(J)Ljava/lang/String;", ValueOf, true},
    {"java/lang/String", "valueOf", "(Z)Ljava/lang/String;", ValueOf, true},
    {"java/lang/String", "valueOf", "(C)Ljava/lang/String;", ValueOf, true},
    {"java/lang/String", "valueOf", "([C)Ljava/lang/String;", ValueOf, true},
};
constexpr uint64_t MethodBase = jni_abi::Base + 0x1000000;
std::string objectClass(const JNIObject &Object) {
  return Object.Type == JNIObject::Class ? "java/lang/Class" : Object.ClassName;
}
// Decode only the authoritative table's descriptors, not an input program.
std::vector<char> parameters(llvm::StringRef Signature) {
  std::vector<char> Types;
  for (size_t I = 1; Signature[I] != ')';) {
    char Type = Signature[I++];
    if (Type == '[') {
      while (Signature[I] == '[')
        ++I;
      Type = Signature[I++];
      if (Type == 'L')
        I = Signature.find(';', I) + 1;
      Type = 'L';
    } else if (Type == 'L') {
      I = Signature.find(';', I) + 1;
    }
    Types.push_back(Type);
  }
  return Types;
}
} // namespace

BionicResult Bionic::jniMethods(unsigned Slot, const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  auto Receiver = jniObject(A[1]);
  if (!Receiver)
    return Receiver.takeError();
  if (!*Receiver)
    return failure("JNI method operation requires a nonnull receiver/class");
  if (Slot == 33 || Slot == 113) {
    if ((*Receiver)->Type != JNIObject::Class)
      return failure("JNI method lookup requires a class reference");
    auto Name = string(A[2]);
    if (!Name)
      return Name.takeError();
    auto Signature = string(A[3]);
    if (!Signature)
      return Signature.takeError();
    // Prefer the most specific body (String.equals overrides Object.equals).
    for (unsigned I = std::size(Methods); I-- > 0;) {
      const auto &M = Methods[I];
      if (*Name != M.Name || *Signature != M.Signature ||
          M.Static != (Slot == 113) ||
          !jniAssignable((*Receiver)->ClassName, M.Class) ||
          (M.Op == Construct && (*Receiver)->ClassName != M.Class))
        continue;
      JNI.ResolvedMethods.insert(I);
      return value(MethodBase + I * 8);
    }
    auto Unsupported = unsupportedImport(Call);
    Result.Diagnostic += " (unmodeled Java method " + (*Receiver)->ClassName +
                         "." + *Name + *Signature + ")";
    return Unsupported;
  }
  const bool New = Slot >= 28 && Slot <= 30;
  const bool Nonvirtual = Slot >= 64 && Slot <= 93;
  const bool Static = Slot >= 114;
  const unsigned First = Nonvirtual ? 4 : 3;
  const uint64_t ID = A[First - 1];
  if (ID < MethodBase || (ID - MethodBase) % 8 ||
      (ID - MethodBase) / 8 >= std::size(Methods) ||
      !JNI.ResolvedMethods.contains(unsigned((ID - MethodBase) / 8)))
    return failure("JNI method ID was not resolved by this VM");
  const auto &M = Methods[(ID - MethodBase) / 8];
  llvm::StringRef Signature = M.Signature;
  const char Return = Signature[Signature.find(')') + 1];
  const unsigned Base = New ? 28 : Static ? 114 : Nonvirtual ? 64 : 34;
  const unsigned Form = (Slot - Base) % 3;
  const char RequestedReturn = New ? 'V' : "LZBCSIJFDV"[(Slot - Base) / 3];
  if (M.Static != Static || (New != (M.Op == Construct)) ||
      RequestedReturn != (Return == '[' ? 'L' : Return))
    return failure("JNI method invocation kind or return type mismatch");
  if (Static || New) {
    if ((*Receiver)->Type != JNIObject::Class ||
        (*Receiver)->ClassName != M.Class)
      return failure("JNI static method/constructor class mismatch");
  } else if (!jniAssignable(objectClass(**Receiver), M.Class)) {
    return failure("JNI method receiver type mismatch");
  }
  if (Nonvirtual) {
    auto Class = jniObject(A[2]);
    if (!Class)
      return Class.takeError();
    if (!*Class || (*Class)->Type != JNIObject::Class ||
        !jniAssignable(objectClass(**Receiver), (*Class)->ClassName) ||
        !jniAssignable((*Class)->ClassName, M.Class))
      return failure("JNI nonvirtual method class mismatch");
    // An inherited ID cannot select a different override in the named class.
    if (M.Op == IdentityEquals && (*Class)->ClassName == "java/lang/String")
      return failure("JNI nonvirtual method ID does not match class lookup");
  }
  const auto Types = parameters(Signature);
  std::vector<uint64_t> Values;
  uint64_t Remaining = 4096;
  ArgumentReader Arguments(*this, Call, First, Remaining);
  if (Form == 1 && !Types.empty())
    if (auto E = Arguments.initializeVAList(A[First]))
      return std::move(E);
  if (Form == 2 && !Types.empty())
    if (auto E = access(A[First], Types.size() * 8, Read))
      return std::move(E);
  for (unsigned I = 0; I < Types.size(); ++I) {
    uint64_t V;
    if (Form == 2) {
      uint8_t Bytes[8];
      if (auto E = CPU.read(A[First] + I * 8, Bytes))
        return std::move(E);
      V = llvm::support::endian::read64le(Bytes);
    } else {
      auto Argument =
          Arguments.next(Types[I] == 'L' || Types[I] == 'J' ? 8 : 4);
      if (!Argument)
        return Argument.takeError();
      V = *Argument;
    }
    // Narrow primitives consume their declared bits, regardless of carrier.
    if (Types[I] == 'Z')
      V = uint8_t(V);
    if (Types[I] == 'C')
      V = uint16_t(V);
    Values.push_back(V);
  }
  auto Ref = [&](std::shared_ptr<JNIObject> Object) -> BionicResult {
    auto R = jniReference(std::move(Object));
    if (!R)
      return R.takeError();
    return value(*R);
  };
  auto Throw = [&](llvm::StringRef Class) -> BionicResult {
    if (auto E = jniException(Class))
      return std::move(E);
    return value(0);
  };
  auto Make = [&](llvm::ArrayRef<uint8_t> Bytes,
                  bool Array = false) -> BionicResult {
    auto Object =
        jniNewObject(Array ? JNIObject::PrimitiveArray : JNIObject::String,
                     Array ? "[C" : "java/lang/String", Bytes.size());
    if (!Object)
      return Object.takeError();
    (*Object)->Bytes.assign(Bytes.begin(), Bytes.end());
    (*Object)->Length = Bytes.size() / 2;
    if (Array)
      (*Object)->ElementSize = 2;
    return Ref(*Object);
  };
  auto Cached = [&](const std::string &Key,
                    llvm::ArrayRef<uint8_t> Bytes) -> BionicResult {
    auto I = JNI.MethodConstants.find(Key);
    if (I != JNI.MethodConstants.end())
      return Ref(I->second);
    auto Object =
        jniNewObject(JNIObject::String, "java/lang/String", Bytes.size());
    if (!Object)
      return Object.takeError();
    (*Object)->Bytes.assign(Bytes.begin(), Bytes.end());
    (*Object)->Length = Bytes.size() / 2;
    JNI.MethodConstants.emplace(Key, *Object);
    return Ref(*Object);
  };
  const auto &Object = *Receiver;
  const auto &Bytes = Object->Bytes;
  auto Unit = [](llvm::ArrayRef<uint8_t> B, size_t I) {
    return llvm::support::endian::read16le(B.data() + I * 2);
  };
  if (M.Op == GetClass) {
    auto Class = jniClass(objectClass(*Object));
    if (!Class)
      return Class.takeError();
    return Ref(*Class);
  }
  if (M.Op == ClassName) {
    std::vector<uint8_t> Name;
    for (char C : Object->ClassName) {
      Name.push_back(C == '/' ? '.' : C);
      Name.push_back(0);
    }
    return Cached("class:" + Object->ClassName, Name);
  }
  if (M.Op == IsArray)
    return value(llvm::StringRef(Object->ClassName).starts_with("["));
  if (M.Op == Length || M.Op == Empty)
    return value(M.Op == Length ? Object->Length : Object->Length == 0);
  if (M.Op == ToString)
    return Ref(Object);
  if (M.Op == ToChars)
    return Make(Bytes, true);
  if (M.Op == Hash) {
    uint32_t Hash = 0;
    for (size_t I = 0; I < Object->Length; ++I)
      Hash = Hash * 31 + Unit(Bytes, I);
    return value(Hash);
  }
  if (M.Op == CharAt) {
    if (uint32_t(Values[0]) >= Object->Length)
      return Throw("java/lang/StringIndexOutOfBoundsException");
    return value(Unit(Bytes, uint32_t(Values[0])));
  }
  if (M.Op == Substring) {
    const uint32_t Begin = Values[0];
    const uint32_t End = Values.size() == 2 ? Values[1] : Object->Length;
    if (Begin > End || End > Object->Length)
      return Throw("java/lang/StringIndexOutOfBoundsException");
    if (Begin == 0 && End == Object->Length)
      return Ref(Object);
    return Make(llvm::ArrayRef(Bytes).slice(Begin * 2, (End - Begin) * 2));
  }
  if (M.Op == ValueOf && Types[0] != 'L') {
    if (Types[0] == 'C') {
      uint8_t B[2];
      llvm::support::endian::write16le(B, Values[0]);
      return Make(B);
    }
    std::string Text = Types[0] == 'Z'   ? (Values[0] ? "true" : "false")
                       : Types[0] == 'J' ? std::to_string(int64_t(Values[0]))
                                         : std::to_string(int32_t(Values[0]));
    std::vector<uint8_t> B;
    for (char C : Text) {
      B.push_back(C);
      B.push_back(0);
    }
    if (Types[0] == 'Z' ||
        (Types[0] == 'I' && int32_t(Values[0]) == INT32_MIN) ||
        (Types[0] == 'J' && int64_t(Values[0]) == INT64_MIN))
      return Cached("literal:" + Text, B);
    // Android libcore's Integer.toString shares the -99..99 value strings;
    // Long.toString and other integer values allocate independent strings.
    if (Types[0] == 'I' && int32_t(Values[0]) > -100 &&
        int32_t(Values[0]) < 100)
      return Cached("integer:" + Text, B);
    return Make(B);
  }
  if (M.Op == Construct && Values.empty())
    return Make({});
  // Resolve reference arguments before any object/array mutation.
  auto Other = jniObject(Values[M.Op == GetChars ? 2 : 0]);
  if (!Other)
    return Other.takeError();
  if (M.Op == IdentityEquals &&
      (Nonvirtual || Object->Type != JNIObject::String))
    return value(Object == *Other);
  if (M.Op == Equals || M.Op == IdentityEquals)
    return value(*Other && (*Other)->Type == JNIObject::String &&
                 Bytes == (*Other)->Bytes);
  if (M.Op == GetChars) {
    const uint32_t Begin = Values[0], End = Values[1], Destination = Values[3];
    if (!*Other)
      return Throw("java/lang/NullPointerException");
    if ((*Other)->ClassName != "[C")
      return failure("JNI method char array parameter type mismatch");
    if (Begin > End || End > Object->Length)
      return Throw("java/lang/StringIndexOutOfBoundsException");
    if (Destination > (*Other)->Length ||
        End - Begin > (*Other)->Length - Destination)
      return Throw("java/lang/ArrayIndexOutOfBoundsException");
    std::copy(Bytes.begin() + Begin * 2, Bytes.begin() + End * 2,
              (*Other)->Bytes.begin() + Destination * 2);
    return value(0);
  }
  if (!*Other)
    return Throw("java/lang/NullPointerException");
  const bool Array = Signature.contains("[C");
  if ((Array && (*Other)->ClassName != "[C") ||
      (!Array && (*Other)->Type != JNIObject::String))
    return failure("JNI method reference parameter type mismatch");
  const auto &Right = (*Other)->Bytes;
  if (M.Op == Construct || M.Op == ValueOf) {
    const uint32_t Begin = Values.size() == 3 ? Values[1] : 0;
    const uint32_t Count = Values.size() == 3 ? Values[2] : (*Other)->Length;
    if (Begin > (*Other)->Length || Count > (*Other)->Length - Begin)
      return Throw("java/lang/StringIndexOutOfBoundsException");
    return Make(llvm::ArrayRef(Right).slice(Begin * 2, Count * 2));
  }
  if (M.Op == Concat) {
    if (Right.empty())
      return Ref(Object);
    if (Right.size() > Options.MemoryLimit / 2 - Bytes.size())
      return failure("JNI concatenated string storage budget exhausted");
    std::vector<uint8_t> Joined = Bytes;
    Joined.insert(Joined.end(), Right.begin(), Right.end());
    return Make(Joined);
  }
  if (M.Op == Compare) {
    for (size_t I = 0; I < std::min(Object->Length, (*Other)->Length); ++I)
      if (Unit(Bytes, I) != Unit(Right, I))
        return value(
            uint32_t(int32_t(Unit(Bytes, I)) - int32_t(Unit(Right, I))));
    return value(uint32_t(Object->Length - (*Other)->Length));
  }
  int64_t Offset = Values.size() == 2 ? int32_t(Values[1]) : 0;
  if (M.Op == EndsWith)
    Offset = int64_t(Object->Length) - int64_t((*Other)->Length);
  auto Match = [&](uint64_t At) {
    return At <= Object->Length && (*Other)->Length <= Object->Length - At &&
           std::equal(Right.begin(), Right.end(), Bytes.begin() + At * 2);
  };
  if (M.Op == StartsWith || M.Op == EndsWith)
    return value(Offset >= 0 && Match(Offset));
  assert(M.Op == IndexOf);
  Offset = std::clamp<int64_t>(Offset, 0, Object->Length);
  // Both strings are bounded by the object budget; also bound quadratic search.
  if (Right.size() &&
      uint64_t(Object->Length - Offset) > 1048576 / Right.size())
    return failure("JNI string search work budget exhausted");
  for (uint64_t I = Offset; I <= Object->Length; ++I)
    if (Match(I))
      return value(I);
  return value(uint32_t(-1));
}
} // namespace neverd::emulation::android_model
