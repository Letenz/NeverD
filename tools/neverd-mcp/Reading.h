//===- Reading.h - Lossless shorter spelling of NeverD C --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A reading view of NeverD C: the same code in fewer tokens, nothing lost.
///
/// The engine writes C that compiles: every fixed-width type spelled out, a
/// stack frame as a struct with one member per line, and each access into an
/// unnamed data region as an offset into a byte array.  A model reads the
/// code, so this view spells the same program shorter, and every rewrite can
/// be undone from the text and its legend:
///
/// - intN_t/uintN_t/uintptr_t are written iN/uN/uptr;
/// - a frame struct is declared on one line, and the members the engine names
///   by offset (var_mN, gapN) are written without the `stack_storage.` that
///   every access repeats;
/// - consecutive declarations of one type become one declaration;
/// - a value read or written at a constant offset into a byte-array global is
///   named by its address, g_ADDR, and the legend declares each name's type.
///   Only accesses through a typed dereference are named, and only where
///   every access at that address has one type; address arithmetic is left as
///   written.
///
/// String and character literals and comments are never rewritten, and a
/// short name is used only where the text does not already use it.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <string_view>

namespace neverd::binmcp {

struct ReadingView {
  std::string Text;
  /// What the view abbreviates; empty when the view is the text itself.
  std::string Legend;
};

ReadingView readingView(std::string_view Text);

} // namespace neverd::binmcp
