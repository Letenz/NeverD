//===- MobileDalvik.h - Mobile source recovery declarations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Mobile source recovery declarations.
//===----------------------------------------------------------------------===//
#pragma once

#include "MobileCommon.h"
#include "neverd/dalvik/Model.h"

namespace neverd::mobile::dalvik {
Class parseSmali(std::string_view text, std::string_view input_id,
                 Budget &budget);
llvm::json::Object recoverJava(const ClassMap &classes, Budget &budget);
} // namespace neverd::mobile::dalvik
