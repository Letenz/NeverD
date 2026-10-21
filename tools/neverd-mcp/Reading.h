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
///   every access repeats, a slot var_mN as sN;
/// - each run of declarations becomes one declaration per type, a pointer
///   type's names each with their own stars;
/// - an address at a constant offset into a byte-array global, read or
///   written through a typed dereference or taken as an integer, is named by
///   its address, g_ADDR.  The legend declares each name with the type most of
///   its accesses have; an access of another type keeps its cast, as
///   `*(T *)&g_ADDR`, and an address that is only taken is declared a byte
///   array.  The array's own pointer, and arithmetic on it, are left as
///   written.
///
/// - the temporaries the engine names by their values, tB_I, are numbered tN
///   in the order the whole function first uses them, so every page of it
///   spells them alike; a page is numbered only with its function's text.
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

/// The view of \p Text, a page of \p Function, the whole function's text,
/// when that is given.
ReadingView readingView(std::string_view Text, std::string_view Function = {});

} // namespace neverd::binmcp
