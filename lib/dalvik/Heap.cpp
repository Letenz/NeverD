//===- Heap.cpp - Bounded Java object identity and array types ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/dalvik/Heap.h"

namespace neverd::mobile::dalvik {
Heap::Heap(uint64_t byte_limit, uint32_t object_limit)
    : limit(byte_limit), count_limit(object_limit) {}
std::shared_ptr<Object> Heap::allocate(Object::Kind kind, std::string_view name,
                                       uint64_t bytes, uint64_t extra) {
  const uint64_t overhead = sizeof(Object) + name.size() + 64;
  if (objects.size() >= count_limit || bytes > limit || extra > limit - bytes ||
      overhead > limit - bytes - extra ||
      used > limit - bytes - extra - overhead)
    return {};
  auto object = std::make_shared<Object>();
  object->Type = kind;
  object->ClassName = name;
  object->Identity = uint32_t(objects.size()) + 1;
  object->Bytes.resize(bytes);
  objects.push_back(object);
  used += bytes + extra + overhead;
  return object;
}
std::shared_ptr<Object> Heap::get(uint64_t id) const {
  return id && id <= objects.size() ? objects[id - 1] : nullptr;
}
bool referenceType(std::string_view type) {
  return type.starts_with('L') || type.starts_with('[');
}
std::string objectDescriptor(const Object &object) {
  if (object.Type == Object::Class)
    return "Ljava/lang/Class;";
  return object.ClassName.starts_with('[') ? object.ClassName
                                           : "L" + object.ClassName + ";";
}
std::string_view javaParent(std::string_view name) {
  if (name == "java/lang/Object")
    return {};
  if (name == "java/lang/RuntimeException")
    return "java/lang/Exception";
  if (name == "java/lang/Exception" || name == "java/lang/Error")
    return "java/lang/Throwable";
  if (name == "java/lang/OutOfMemoryError" || name == "java/lang/LinkageError")
    return "java/lang/Error";
  if (name == "java/lang/ExceptionInInitializerError" ||
      name == "java/lang/NoClassDefFoundError")
    return "java/lang/LinkageError";
  if (name == "java/lang/ArrayIndexOutOfBoundsException" ||
      name == "java/lang/StringIndexOutOfBoundsException")
    return "java/lang/IndexOutOfBoundsException";
  if (name.starts_with("java/lang/") && name.ends_with("Exception"))
    return "java/lang/RuntimeException";
  return "java/lang/Object";
}
bool referenceAssignable(std::string_view source,
                         std::string_view destination) {
  if (source == destination)
    return true;
  if (!referenceType(source) || !referenceType(destination))
    return false;
  if (source.starts_with('[')) {
    if (destination == "Ljava/lang/Object;" ||
        destination == "Ljava/lang/Cloneable;" ||
        destination == "Ljava/io/Serializable;")
      return true;
    return destination.starts_with('[') && referenceType(source.substr(1)) &&
           referenceType(destination.substr(1)) &&
           referenceAssignable(source.substr(1), destination.substr(1));
  }
  if (destination.starts_with('['))
    return false;
  source = source.substr(1, source.size() - 2);
  destination = destination.substr(1, destination.size() - 2);
  for (; !source.empty(); source = javaParent(source))
    if (source == destination)
      return true;
  return false;
}
} // namespace neverd::mobile::dalvik
