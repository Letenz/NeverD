//===- Heap.h - Shared workload Java object storage -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#pragma once

#include "neverd/dalvik/Model.h"

#include <memory>

namespace neverd::mobile::dalvik {
// Identities are workload-local tokens, never host or guest addresses. The
// owner retains bounded objects until teardown; weak edges admit cycles without
// leaking host ownership. JNI references and DEX registers use this same heap.
struct Object {
  enum Kind { Class, String, PrimitiveArray, ObjectArray, Throwable, Instance };
  Kind Type;
  uint32_t Identity = 0;
  std::string ClassName;
  std::vector<uint8_t> Bytes;
  std::vector<std::weak_ptr<Object>> Elements;
  std::map<const Field *, uint64_t> Fields;
  uint64_t Length = 0;
  unsigned ElementSize = 0;
  bool Initialized = true;
};
class Heap {
public:
  explicit Heap(uint64_t byte_limit = 8 * 1024 * 1024,
                uint32_t object_limit = 4096);
  // Returns null on the finite model budget; this is not a claim about a real
  // Java VM's allocation failure or collection policy. extra accounts for maps.
  std::shared_ptr<Object> allocate(Object::Kind kind, std::string_view name,
                                   uint64_t bytes, uint64_t extra = 0);
  std::shared_ptr<Object> get(uint64_t identity) const;

private:
  uint64_t limit, used = 0;
  uint32_t count_limit;
  std::vector<std::shared_ptr<Object>> objects;
};
bool referenceType(std::string_view type);
std::string objectDescriptor(const Object &object);
// The finite platform hierarchy and arrays; supplied classes currently have
// Object as their sole parent. Callers separately admit known declarations.
std::string_view javaParent(std::string_view name);
bool referenceAssignable(std::string_view source, std::string_view destination);
} // namespace neverd::mobile::dalvik
