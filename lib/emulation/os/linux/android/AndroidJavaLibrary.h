//===- AndroidJavaLibrary.h - Java library input -----------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#pragma once
#include <algorithm>
#include <string_view>

namespace neverd::emulation::android_model {
inline bool validJavaLibraryName(std::string_view Name) {
  return !Name.empty() && Name.size() <= 255 &&
         std::all_of(Name.begin(), Name.end(), [](unsigned char C) {
           return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
                  (C >= '0' && C <= '9') || C == '_' || C == '-' || C == '.';
         });
}
} // namespace neverd::emulation::android_model
